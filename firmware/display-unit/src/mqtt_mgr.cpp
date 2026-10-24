/**
 * @file mqtt_mgr.cpp
 * @brief MQTT connection manager — espMqttClient lifecycle + state publishing.
 *
 * Threading recap: espMqttClient callbacks run on the AsyncTCP task. They
 * enqueue events and post UiEvents; everything else happens in loop().
 */
#include "mqtt_mgr.h"

#include <freertos/queue.h>
#include <espMqttClient.h>

#include <catfish_protocol.h>
#include "mqtt_payloads.h"
#include "net_wifi.h"
#include "prefs.h"
#include "ui/ui.h"

namespace {

// Internal client task: prio 1, core 0 (below net-worker prio 2; loop + LVGL
// stay on core 1).
espMqttClient s_client(1, 0);

QueueHandle_t s_evtq = nullptr;

mqtt_mgr::Status s_status = mqtt_mgr::Status::Disabled;
bool s_wanted = false;   // enabled + configured -> keep trying to connect
uint32_t s_next_attempt = 0;
uint32_t s_backoff_ms = 5000;

// Command topics cached by applyConfig() (loop ctx) so the AsyncTCP-task
// onMessage callback only reads immutable buffers (changed while disconnected).
char s_topic_feed_cmd[96] = {0};
char s_topic_pulse_set[96] = {0};

// ---------------------------------------------------------------- callbacks
void onConnectCb(bool /*sessionPresent*/) {
  // AsyncTCP task: queueing is thread-safe; subscribing queues packets.
  s_client.subscribe(s_topic_feed_cmd, 1);
  s_client.subscribe(s_topic_pulse_set, 1);
  Serial.printf("[MQTT] connected as '%s'\n", CATFISH_DISPLAY_HOSTNAME);
  if (s_evtq) {
    const bool connected = true;
    xQueueSend(s_evtq, &connected, 0);
  }
}

void onDisconnectCb(espMqttClientTypes::DisconnectReason reason) {
  Serial.printf("[MQTT] disconnected (%s)\n",
                espMqttClientTypes::disconnectReasonToString(reason));
  if (s_evtq) {
    const bool connected = false;
    xQueueSend(s_evtq, &connected, 0);
  }
}

void onMessageCb(const espMqttClientTypes::MessageProperties & /*properties*/,
                 const char *topic, const uint8_t *payload, size_t len,
                 size_t /*index*/, size_t /*total*/) {
  char value[12] = {0};
  if (len >= sizeof(value)) return; // commands are tiny ("ON" / "30")
  memcpy(value, payload, len);

  if (strcmp(topic, s_topic_feed_cmd) == 0 && strcmp(value, "ON") == 0) {
    UiEvent ev;
    ev.type = UI_EVT_FEED;
    ev.a = prefs::getDefaultPulse(); // benign snapshot read (single byte)
    ev.flag = true;                  // manual
    ui::ui_event_send(ev);
    Serial.println("[MQTT] feed command received");
    return;
  }

  if (strcmp(topic, s_topic_pulse_set) == 0) {
    const long secs = strtol(value, nullptr, 10);
    UiEvent ev;
    ev.type = UI_EVT_SET_PULSE;
    ev.a = mqtt_payloads::clampPulse(secs);
    ui::ui_event_send(ev);
    Serial.printf("[MQTT] pulse command received: %u s\n", (unsigned)ev.a);
    return;
  }
}

// ---------------------------------------------------------------- helpers
bool publishTopic(const char *topic, const String &payload, bool retain) {
  if (!s_client.connected() || !topic[0]) return false;
  s_client.publish(topic, 0, retain, payload.c_str());
  return true;
}

} // anonymous namespace

namespace mqtt_mgr {

bool begin() {
  if (s_evtq) return true;
  s_evtq = xQueueCreate(4, sizeof(bool));
  if (!s_evtq) {
    Serial.println("[MQTT] ERROR: event queue alloc failed");
    return false;
  }

  s_client.onConnect(onConnectCb);
  s_client.onDisconnect(onDisconnectCb);
  s_client.onMessage(onMessageCb);
  s_client.setClientId(CATFISH_DISPLAY_HOSTNAME);
  s_client.setKeepAlive(15);

  Serial.println("[MQTT] manager ready");
  return true;
}

void applyConfig() {
  const bool en = prefs::getMqttEnabled();
  const String host = prefs::getMqttHost();

  if (!en || !host.length()) {
    s_wanted = false;
    if (s_client.connected() || !s_client.disconnected()) s_client.disconnect(true);
    s_status = mqtt_mgr::Status::Disabled;
    return;
  }

  const uint16_t port = prefs::getMqttPort();
  const String user = prefs::getMqttUser();
  const String pass = prefs::getMqttPass();
  const String prefix = prefs::getMqttPrefix();

  // Reconfigure from a clean state (espMqttClient setters want it disconnected).
  if (s_client.connected() || !s_client.disconnected()) s_client.disconnect(true);

  strlcpy(s_topic_feed_cmd,
          mqtt_payloads::feedCmdTopic(prefix).c_str(), sizeof(s_topic_feed_cmd));
  strlcpy(s_topic_pulse_set,
          mqtt_payloads::pulseSetTopic(prefix).c_str(), sizeof(s_topic_pulse_set));

  s_client.setServer(host.c_str(), port);
  if (user.length()) {
    s_client.setCredentials(user.c_str(), pass.c_str());
  }
  // Retained LWT: brokers publish "offline" for us when the connection drops.
  s_client.setWill(mqtt_payloads::availabilityTopic(prefix).c_str(), 1, true,
                   "offline");

  s_wanted = true;
  s_backoff_ms = 5000;            // config change resets the retry backoff
  s_next_attempt = millis();      // try immediately (WiFi-gated)
  s_status = mqtt_mgr::Status::Offline;
  Serial.printf("[MQTT] config applied: %s:%u (topics under '%s')\n",
                host.c_str(), (unsigned)port, prefix.c_str());
}

void retryIfDue(uint32_t nowMs) {
  if (!s_wanted || s_client.connected()) return;
  if (!net_wifi::wifi_is_connected()) return;
  if ((int32_t)(nowMs - s_next_attempt) < 0) return;

  if (s_client.connect()) {
    s_status = mqtt_mgr::Status::Connecting;
  } else {
    // connect() only fails synchronously on invalid state; treat as failure.
    s_next_attempt = nowMs + s_backoff_ms;
  }
}

bool receive(bool &connected) {
  if (!s_evtq) return false;
  if (xQueueReceive(s_evtq, &connected, 0) != pdTRUE) return false;

  if (connected) {
    s_status = mqtt_mgr::Status::Connected;
    s_backoff_ms = 5000;
  } else {
    s_status = mqtt_mgr::Status::Offline;
    s_next_attempt = millis() + s_backoff_ms;
    s_backoff_ms = (s_backoff_ms < 30000UL) ? s_backoff_ms * 2 : 60000UL;
  }
  return true;
}

Status status() { return s_status; }

bool connected() { return s_client.connected(); }

void publishDiscovery() {
  if (!s_client.connected()) return;
  const String p = prefs::getMqttPrefix();
  struct Cfg {
    const char *path;
    String (*fn)(const String &);
  };
  const Cfg cfgs[] = {
      {"homeassistant/button/catfish_feed_now/config", mqtt_payloads::discoveryFeedButton},
      {"homeassistant/number/catfish_pulse/config", mqtt_payloads::discoveryPulseNumber},
      {"homeassistant/binary_sensor/catfish_relay_online/config", mqtt_payloads::discoveryRelayOnline},
      {"homeassistant/binary_sensor/catfish_feeding/config", mqtt_payloads::discoveryFeeding},
      {"homeassistant/sensor/catfish_last_feed/config", mqtt_payloads::discoveryLastFeed},
      {"homeassistant/sensor/catfish_next_feed/config", mqtt_payloads::discoveryNextFeed},
  };
  for (const Cfg &c : cfgs) {
    s_client.publish(c.path, 0, true, c.fn(p).c_str());
  }
  Serial.println("[MQTT] discovery configs published (retained)");
}

void publishAvailable() {
  publishTopic(mqtt_payloads::availabilityTopic(prefs::getMqttPrefix()).c_str(),
               String("online"), true);
}

void publishLastFeed(uint32_t epoch) {
  publishTopic(
      mqtt_payloads::lastFeedTopic(prefs::getMqttPrefix()).c_str(),
      mqtt_payloads::epochToIso8601(epoch), true);
}

void publishNextFeed(const String &hhmm) {
  publishTopic(mqtt_payloads::nextFeedTopic(prefs::getMqttPrefix()).c_str(),
               hhmm.length() ? hhmm : String("none"), true);
}

void publishRelayOnline(bool online) {
  publishTopic(mqtt_payloads::relayOnlineTopic(prefs::getMqttPrefix()).c_str(),
               String(online ? "ON" : "OFF"), true);
}

void publishFeeding(bool feeding) {
  publishTopic(mqtt_payloads::feedingTopic(prefs::getMqttPrefix()).c_str(),
               String(feeding ? "ON" : "OFF"), true);
}

void publishPulse(uint8_t seconds) {
  publishTopic(mqtt_payloads::pulseStateTopic(prefs::getMqttPrefix()).c_str(),
               String((unsigned)seconds), true);
}

} // namespace mqtt_mgr
