/**
 * @file net_worker.cpp
 * @brief Background worker owning all blocking relay HTTP/discovery calls.
 */
#include "net_worker.h"
#include <freertos/queue.h>
#include "relay_client.h"

namespace {

QueueHandle_t s_cmdq = nullptr;
QueueHandle_t s_resq = nullptr;

void fillError(net_worker::Result &r, const String &err) {
  strlcpy(r.error, err.c_str(), sizeof(r.error));
}

void workerTask(void * /*param*/) {
  net_worker::Cmd cmd;

  for (;;) {
    if (xQueueReceive(s_cmdq, &cmd, portMAX_DELAY) != pdTRUE) continue;

    net_worker::Result r;

    if (cmd.type == net_worker::Cmd::CMD_FEED) {
      Serial.printf("[NET] worker: feed %u s (%s)%s\n", (unsigned)cmd.seconds,
                    cmd.manual ? "manual" : "scheduled", cmd.retry ? " RETRY" : "");
      const relay_client::Result fr = relay_client::feed(cmd.seconds);
      r.kind = net_worker::Result::RES_FEED;
      r.ok = fr.ok;
      r.manual = cmd.manual;
      r.retry = cmd.retry;
      r.hour = cmd.hour;
      r.minute = cmd.minute;
      r.seconds = fr.ok ? fr.seconds_applied : cmd.seconds;
      r.clamped = fr.clamped;
      strlcpy(r.note, fr.note.c_str(), sizeof(r.note));
      strlcpy(r.ip, fr.ip.c_str(), sizeof(r.ip));
      if (!fr.ok) fillError(r, fr.error);
    } else if (cmd.type == net_worker::Cmd::CMD_TOKEN_ROTATE) {
      Serial.println("[NET] worker: token rotate request");
      const relay_client::Result tr = relay_client::rotateToken(String(cmd.token));
      r.kind = net_worker::Result::RES_TOKEN;
      r.ok = tr.ok;
      strlcpy(r.token, cmd.token, sizeof(r.token));
      strlcpy(r.ip, tr.ip.c_str(), sizeof(r.ip));
      if (!tr.ok) fillError(r, tr.error);
    } else {
      // PROBE: discover when needed, then GET /status
      bool online = false;
      relay_client::Status st;
      if (!relay_client::hasIp()) {
        const relay_client::Result d = relay_client::discover();
        online = d.ok;
      }
      if (relay_client::hasIp() || online) {
        st = relay_client::fetchStatus();
        online = st.ok;
      }
      r.kind = net_worker::Result::RES_PROBE;
      r.ok = online;
      r.online = online;
      r.feeding = st.feeding;
      r.report = cmd.report;
      strlcpy(r.ip, relay_client::ip().c_str(), sizeof(r.ip));
      strlcpy(r.version, st.version.c_str(), sizeof(r.version));
      if (!st.ok && st.error.length()) fillError(r, st.error);
    }

    if (xQueueSend(s_resq, &r, 0) != pdTRUE) {
      Serial.println("[NET] worker: result queue full, result dropped");
    }
  }
}

} // anonymous namespace

namespace net_worker {

bool begin() {
  if (s_cmdq) return true; // already started
  s_cmdq = xQueueCreate(4, sizeof(Cmd));
  s_resq = xQueueCreate(6, sizeof(Result));
  if (!s_cmdq || !s_resq) {
    Serial.println("[NET] ERROR: worker queue alloc failed");
    return false;
  }
  // 12 KB stack: HTTPClient + ArduinoJson + discovery chains measured well
  // under 8 KB, but headroom is free on this part (L5 review item).
  if (xTaskCreatePinnedToCore(workerTask, "net-worker", 12288, nullptr, 2,
                              nullptr, 0) != pdPASS) {
    Serial.println("[NET] ERROR: worker task create failed");
    return false;
  }
  Serial.println("[NET] net-worker started (core 0)");
  return true;
}

bool enqueueFeed(uint8_t seconds, bool manual, uint8_t hour, uint8_t minute,
                 bool retry) {
  if (!s_cmdq) return false;
  Cmd c;
  c.type = Cmd::CMD_FEED;
  c.seconds = seconds;
  c.manual = manual;
  c.retry = retry;
  c.hour = hour;
  c.minute = minute;
  return xQueueSend(s_cmdq, &c, 0) == pdTRUE;
}

bool enqueueProbe(bool report) {
  if (!s_cmdq) return false;
  Cmd c;
  c.type = Cmd::CMD_PROBE;
  c.report = report;
  return xQueueSend(s_cmdq, &c, 0) == pdTRUE;
}

bool enqueueTokenRotate(const char *newToken) {
  if (!s_cmdq || !newToken || !newToken[0]) return false;
  Cmd c;
  c.type = Cmd::CMD_TOKEN_ROTATE;
  strlcpy(c.token, newToken, sizeof(c.token));
  return xQueueSend(s_cmdq, &c, 0) == pdTRUE;
}

bool receive(Result &out) {
  if (!s_resq) return false;
  return xQueueReceive(s_resq, &out, 0) == pdTRUE;
}

} // namespace net_worker
