/**
 * @file net_worker.cpp
 * @brief Background worker owning all blocking relay HTTP/discovery calls
 *        (multi-tank aware: FEED fans out over a tank bitmask).
 */
#include "net_worker.h"
#include <freertos/queue.h>
#include <catfish_mech.h>
#include "prefs.h"
#include "relay_client.h"

namespace {

QueueHandle_t s_cmdq = nullptr;
QueueHandle_t s_resq = nullptr;

void fillError(net_worker::Result &r, const String &err) {
  strlcpy(r.error, err.c_str(), sizeof(r.error));
}

uint8_t sanitizeTank1(uint8_t tank1) {
  if (tank1 < 1 || tank1 > relay_client::MAX_TANKS) return 1;
  return tank1;
}

void workerTask(void * /*param*/) {
  net_worker::Cmd cmd;

  for (;;) {
    if (xQueueReceive(s_cmdq, &cmd, portMAX_DELAY) != pdTRUE) continue;

    net_worker::Result r;

    if (cmd.type == net_worker::Cmd::CMD_FEED) {
      // Restrict the mask to the configured tanks; empty -> tank 1.
      uint8_t count = prefs::getTankCount();
      if (count < 1) count = 1;
      if (count > relay_client::MAX_TANKS) count = relay_client::MAX_TANKS;
      uint8_t mask = cmd.tankMask & ((count >= 4) ? 0x0F : ((1u << count) - 1u));
      if (!mask) mask = 0x01;

      Serial.printf("[NET] worker: feed %u s -> tanks 0x%X (%s)%s\n",
                    (unsigned)cmd.seconds, (unsigned)mask,
                    cmd.manual ? "manual" : "scheduled", cmd.retry ? " RETRY" : "");

      r.kind = net_worker::Result::RES_FEED;
      r.manual = cmd.manual;
      r.retry = cmd.retry;
      r.catchup = cmd.catchup;
      r.hour = cmd.hour;
      r.minute = cmd.minute;
      r.seconds = cmd.seconds;
      for (uint8_t t = 1; t <= relay_client::MAX_TANKS; t++) {
        if (!(mask & (1u << (t - 1)))) continue;
        // v1.9.0 (A2): per-tank pulse override (0 = the shared seconds)
        uint8_t secs = cmd.seconds;
        if (cmd.secTank[t - 1]) secs = cmd.secTank[t - 1];
        const relay_client::Result fr = relay_client::feedTank(t, secs);
        if (fr.ok) {
          r.tanksOk |= (1u << (t - 1));
          r.secOk[t - 1] = fr.seconds_applied; // v1.9.0: per-tank applied
          r.seconds = fr.seconds_applied; // last applied wins (compat field)
          strlcpy(r.ip, fr.ip.c_str(), sizeof(r.ip));
          if (fr.clamped) {
            r.clamped = true;
            strlcpy(r.note, fr.note.c_str(), sizeof(r.note));
          }
        } else {
          r.tanksFail |= (1u << (t - 1));
          if (!r.error[0]) fillError(r, String("Tank ") + t + ": " + fr.error);
        }
      }
      r.ok = (r.tanksOk != 0);
    } else if (cmd.type == net_worker::Cmd::CMD_TOKEN_ROTATE) {
      const uint8_t tank1 = sanitizeTank1(cmd.tankId);
      Serial.printf("[NET] worker: token rotate request (tank %u)\n", (unsigned)tank1);
      const relay_client::Result tr = relay_client::rotateTokenTank(tank1, String(cmd.token));
      r.kind = net_worker::Result::RES_TOKEN;
      r.ok = tr.ok;
      r.tankId = tank1;
      strlcpy(r.token, cmd.token, sizeof(r.token));
      strlcpy(r.ip, tr.ip.c_str(), sizeof(r.ip));
      if (!tr.ok) fillError(r, tr.error);
    } else if (cmd.type == net_worker::Cmd::CMD_MECH) {
      const uint8_t tank1 = sanitizeTank1(cmd.tankId);
      Serial.printf("[NET] worker: mechanism -> %s (tank %u)\n",
                    catfishMechName(cmd.mechId), (unsigned)tank1);
      const relay_client::Result mr = relay_client::configureMechTank(
          tank1, cmd.mechId, cmd.mechRest, cmd.mechFeed, cmd.mechHz, cmd.mechDir);
      r.kind = net_worker::Result::RES_MECH;
      r.ok = mr.ok;
      r.tankId = tank1;
      strlcpy(r.ip, mr.ip.c_str(), sizeof(r.ip));
      if (!mr.ok) fillError(r, mr.error);
    } else if (cmd.type == net_worker::Cmd::CMD_SCAN_TANKS) {
      Serial.println("[NET] worker: tank scan request");
      uint8_t assigned = 0;
      const int live = relay_client::scanAndBindTanks(prefs::getTankCount(), &assigned);
      r.kind = net_worker::Result::RES_SCAN;
      r.ok = (live >= 0);
      r.boundMask = assigned;
      r.liveCount = (uint8_t)(live < 0 ? 0 : live);
      if (live < 0) fillError(r, "WiFi down");
    } else {
      // PROBE: discover when needed, then GET /status (per tank)
      const uint8_t tank1 = sanitizeTank1(cmd.tankId);
      Serial.printf("[NET] worker: probe tank %u\n", (unsigned)tank1);
      bool online = false;
      relay_client::Status st;
      if (!relay_client::hasIp(tank1)) {
        const relay_client::Result d = relay_client::discoverTank(tank1);
        online = d.ok;
      }
      if (relay_client::hasIp(tank1) || online) {
        st = relay_client::fetchStatusTank(tank1);
        online = st.ok;
      }
      r.kind = net_worker::Result::RES_PROBE;
      r.tankId = tank1;
      r.ok = online;
      r.online = online;
      r.feeding = st.feeding;
      r.report = cmd.report;
      r.verify = cmd.verify; // v1.9.0 (B3): pulse-verify window probe
      r.protoMismatch =
          st.protocolVersion >= 0 && st.protocolVersion != CATFISH_PROTOCOL_VERSION;
      strlcpy(r.ip, relay_client::ip(tank1).c_str(), sizeof(r.ip));
      strlcpy(r.version, st.version.c_str(), sizeof(r.version));
      strlcpy(r.mech, st.mechanism.c_str(), sizeof(r.mech));
      r.mechRest = st.servoRest;
      r.mechFeed = st.servoFeed;
      r.mechHz = st.stepHz;
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
  s_resq = xQueueCreate(8, sizeof(Result));
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
                 bool retry, uint8_t tankMask, const uint8_t *secTank,
                 bool catchup) {
  if (!s_cmdq) return false;
  Cmd c;
  c.type = Cmd::CMD_FEED;
  c.seconds = seconds;
  c.manual = manual;
  c.retry = retry;
  c.catchup = catchup;
  c.hour = hour;
  c.minute = minute;
  c.tankMask = tankMask;
  if (secTank) {
    for (int i = 0; i < 4; i++) c.secTank[i] = secTank[i];
  }
  return xQueueSend(s_cmdq, &c, 0) == pdTRUE;
}

bool enqueueProbe(bool report, uint8_t tank1, bool verify) {
  if (!s_cmdq) return false;
  Cmd c;
  c.type = Cmd::CMD_PROBE;
  c.report = report;
  c.verify = verify;
  c.tankId = sanitizeTank1(tank1);
  return xQueueSend(s_cmdq, &c, 0) == pdTRUE;
}

bool enqueueTokenRotate(const char *newToken, uint8_t tank1) {
  if (!s_cmdq || !newToken || !newToken[0]) return false;
  Cmd c;
  c.type = Cmd::CMD_TOKEN_ROTATE;
  strlcpy(c.token, newToken, sizeof(c.token));
  c.tankId = sanitizeTank1(tank1);
  return xQueueSend(s_cmdq, &c, 0) == pdTRUE;
}

bool enqueueMech(uint8_t mechId, uint8_t restDeg, uint8_t feedDeg,
                 uint16_t stepHz, uint8_t stepDir, uint8_t tank1) {
  if (!s_cmdq || mechId >= CATFISH_MECH_COUNT) return false;
  Cmd c;
  c.type = Cmd::CMD_MECH;
  c.mechId = mechId;
  c.mechRest = catfishClampServoDeg(restDeg);
  c.mechFeed = catfishClampServoDeg(feedDeg);
  c.mechHz = catfishClampStepHz(stepHz);
  c.mechDir = catfishClampStepDir(stepDir);
  c.tankId = sanitizeTank1(tank1);
  return xQueueSend(s_cmdq, &c, 0) == pdTRUE;
}

bool enqueueScanTanks() {
  if (!s_cmdq) return false;
  Cmd c;
  c.type = Cmd::CMD_SCAN_TANKS;
  return xQueueSend(s_cmdq, &c, 0) == pdTRUE;
}

bool receive(Result &out) {
  if (!s_resq) return false;
  return xQueueReceive(s_resq, &out, 0) == pdTRUE;
}

} // namespace net_worker
