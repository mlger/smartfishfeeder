/**
 * @file catfish_rfnode.h
 * @brief Catfish Feeder — RF "node" datagram contract (v1.8.0).
 *
 * Shared by the optional wireless feed-button/sensor transports on the
 * display unit: ESP-NOW (net_espnow) and nRF24L01+ (net_rf24). Both carry
 * the SAME 12-byte payload; only the radio layer differs. v1.13.0: the relay
 * unit also listens for FEED datagrams (direct puck feed, opt-in). v1.14.0:
 * two new commands turn the same 12-byte envelope into an offline relay
 * DISCOVERY protocol — the display unit can find ESP-NOW-capable relays with
 * the router off (a UDP LAN scan cannot work without WiFi).
 *
 * Trust model: identical to the LAN HTTP API — the packet must echo a 32-bit
 * key derived (FNV-1a) from the current pairing token. Pairing a puck means
 * programming it with the token; rotating the token invalidates all pucks
 * until re-provisioned.
 *
 * Wire format (little-endian, packed, 12 bytes):
 *   [0]  magic 'C' (0x43)
 *   [1]  magic 'F' (0x46)
 *   [2]  ver     (CATFISH_RFNODE_VER)
 *   [3]  cmd     (CATFISH_RFNODE_CMD_FEED / _CMD_DISCOVER / _CMD_DISCOVER_RSP)
 *   [4]  tankMask (bit 0 = tank 1 .. bit 3 = tank 4; 0 = hub default)
 *   [5]  seconds (0 = hub default pulse; else clamped 1..60)
 *   [6..7] seq   (uint16 LE, sender increments; hub drops repeats)
 *   [8..11] key (uint32 LE, FNV-1a of the pairing token)
 *
 * Discovery (v1.14.0, same envelope):
 *   REQ:  cmd=2, broadcast by the display hub; tankMask/seconds unused (0).
 *   RSP:  cmd=3, unicast by the relay to the REQ sender:
 *           tankMask = relay's direct-feed tank filter (0 = accepts all),
 *           seconds  = relay's default pulse seconds (0..60),
 *           seq      = CATFISH_PROTOCOL_VERSION the relay speaks,
 *           key      = token key (same gate as every datagram).
 *   The sender's ESP-NOW MAC is the transport-layer source address — the hub
 *   learns it from the radio, not from the payload.
 *
 * Header-only + dependency-free so the host test suite pins the exact bytes
 * the firmware enforces.
 */
#ifndef CATFISH_RFNODE_H
#define CATFISH_RFNODE_H

#include <stdint.h>
#include <stddef.h>

#define CATFISH_RFNODE_MAGIC0   0x43u // 'C'
#define CATFISH_RFNODE_MAGIC1   0x46u // 'F'
#define CATFISH_RFNODE_VER      1
#define CATFISH_RFNODE_CMD_FEED 1
// v1.14.0: offline relay discovery (broadcast REQ, unicast RSP). Additive
// commands — pre-1.14.0 firmware drops unknown cmds at the strict parser,
// so mixed ponds never misroute a discovery burst into a feed.
#define CATFISH_RFNODE_CMD_DISCOVER     2
#define CATFISH_RFNODE_CMD_DISCOVER_RSP 3
#define CATFISH_RFNODE_PKT_LEN  12

/** v1.12.1: channel the hub listens on when it has never joined a network. */
#define CATFISH_ESPNOW_DEFAULT_CHANNEL 1

/**
 * v1.12.1: pick the channel the hub's ESP-NOW receiver should listen on.
 *  - STA connected: the AP's channel (pucks are paired to it).
 *  - STA down: the last AP channel we saw — the wifi task pins the radio
 *    there between reconnect attempts, so pucks keep matching while the
 *    router is off (a scanning STA hops channels and deafens reception).
 *  - Never connected: CATFISH_ESPNOW_DEFAULT_CHANNEL.
 */
inline uint8_t catfishRfnodeListenChannel(bool connected, uint8_t apChan,
                                          uint8_t lastChan) {
  if (connected && apChan) return apChan;
  if (lastChan) return lastChan;
  return CATFISH_ESPNOW_DEFAULT_CHANNEL;
}

/** Clamp a node-requested tank mask to the 4 supported tanks (0 stays 0). */
inline uint8_t catfishRfnodeClampMask(uint8_t mask) { return mask & 0x0Fu; }

/** Clamp node-requested seconds: 0 = hub default, else 1..60 (hard cap). */
inline uint8_t catfishRfnodeClampSeconds(uint8_t seconds) {
  if (seconds == 0) return 0;
  if (seconds > 60) return 60;
  return seconds;
}

/** FNV-1a 32-bit of the pairing token -> node key. */
inline uint32_t catfishRfnodeKeyFromToken(const char *token) {
  uint32_t h = 2166136261u;
  if (token) {
    for (const char *p = token; *p; ++p) {
      h ^= (uint8_t)*p;
      h *= 16777619u;
    }
  }
  return h;
}

/** Build a feed packet. @return bytes written (CATFISH_RFNODE_PKT_LEN) or 0. */
inline size_t catfishRfnodeBuild(uint8_t *buf, size_t bufLen, uint8_t cmd,
                                 uint8_t tankMask, uint8_t seconds,
                                 uint16_t seq, uint32_t key) {
  if (!buf || bufLen < CATFISH_RFNODE_PKT_LEN) return 0;
  buf[0] = CATFISH_RFNODE_MAGIC0;
  buf[1] = CATFISH_RFNODE_MAGIC1;
  buf[2] = CATFISH_RFNODE_VER;
  buf[3] = cmd;
  buf[4] = catfishRfnodeClampMask(tankMask);
  buf[5] = catfishRfnodeClampSeconds(seconds);
  buf[6] = (uint8_t)(seq & 0xFF);
  buf[7] = (uint8_t)((seq >> 8) & 0xFF);
  buf[8] = (uint8_t)(key & 0xFF);
  buf[9] = (uint8_t)((key >> 8) & 0xFF);
  buf[10] = (uint8_t)((key >> 16) & 0xFF);
  buf[11] = (uint8_t)((key >> 24) & 0xFF);
  return CATFISH_RFNODE_PKT_LEN;
}

/**
 * Parse the shared 12-byte envelope: magic + version + per-cmd clamps.
 * Outputs are clamped; *cmd = 0xFF on any rejection. All three cmd parsers
 * below build on this so the wire bytes have exactly one definition.
 */
inline bool catfishRfnodeEnvelope(const uint8_t *buf, size_t len, uint8_t *cmd,
                                  uint8_t *tankMask, uint8_t *seconds,
                                  uint16_t *seq, uint32_t *key) {
  if (cmd) *cmd = 0xFF;
  if (!buf || len < CATFISH_RFNODE_PKT_LEN) return false;
  if (buf[0] != CATFISH_RFNODE_MAGIC0 || buf[1] != CATFISH_RFNODE_MAGIC1)
    return false;
  if (buf[2] != CATFISH_RFNODE_VER) return false;
  if (cmd) *cmd = buf[3];
  if (tankMask) *tankMask = catfishRfnodeClampMask(buf[4]);
  if (seconds) *seconds = catfishRfnodeClampSeconds(buf[5]);
  if (seq)
    *seq = (uint16_t)(buf[6] | ((uint16_t)buf[7] << 8));
  if (key)
    *key = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8) |
           ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
  return true;
}

/**
 * Validate + parse a received packet.
 * @return true when the packet is a well-formed, token-keyed feed request.
 *         Outputs are clamped; *cmd = 0xFF on any rejection.
 */
inline bool catfishRfnodeParse(const uint8_t *buf, size_t len, uint8_t *cmd,
                               uint8_t *tankMask, uint8_t *seconds,
                               uint16_t *seq, uint32_t *key) {
  uint8_t c = 0xFF;
  const bool ok = catfishRfnodeEnvelope(buf, len, &c, tankMask, seconds, seq,
                                        key) &&
                  c == CATFISH_RFNODE_CMD_FEED; // strict: feed datagrams only
  if (cmd) *cmd = ok ? c : 0xFF; // failure contract: rejected -> 0xFF
  return ok;
}

// -------------------------------------------------------- discovery (v1.14.0)
// Pure, host-testable helpers for the offline relay scan. The REQ reuses the
// feed builder with cmd=2 (mask/seconds zeroed inside so callers cannot craft
// a discovery burst that looks like a feed). The RSP packs the relay's
// capability snapshot into the spare envelope fields (mask, default pulse,
// protocol version) — everything the hub needs to name the relay in a toast
// without opening any HTTP session (there IS no WiFi session when this runs).

/**
 * Build a DISCOVER broadcast request. seq should increment per burst so
 * serial traces can correlate REQ/RSP pairs. @return bytes written or 0.
 */
inline size_t catfishRfnodeBuildDiscover(uint8_t *buf, size_t bufLen,
                                         uint16_t seq, uint32_t key) {
  return catfishRfnodeBuild(buf, bufLen, CATFISH_RFNODE_CMD_DISCOVER, 0, 0,
                            seq, key);
}

/**
 * Parse a DISCOVER request (relay side). Rejects FEED and RSP commands —
 * a strict cmd check is what keeps a discovery burst from ever arming a
 * direct feed on pre-1.14.0 semantics.
 */
inline bool catfishRfnodeParseDiscover(const uint8_t *buf, size_t len,
                                       uint16_t *seq, uint32_t *key) {
  uint8_t cmd = 0xFF;
  if (!catfishRfnodeEnvelope(buf, len, &cmd, nullptr, nullptr, seq, key))
    return false;
  return cmd == CATFISH_RFNODE_CMD_DISCOVER;
}

/**
 * Build a DISCOVER_RSP for a relay: @p tankMask is its direct-feed filter
 * (0 = accepts all), @p pulseS its default pulse seconds (clamped 1..60,
 * 0 stays 0 = "uses factory default"), @p protoVer the wire contract the
 * relay speaks (CATFISH_PROTOCOL_VERSION). @return bytes written or 0.
 */
inline size_t catfishRfnodeBuildDiscoverRsp(uint8_t *buf, size_t bufLen,
                                            uint8_t tankMask, uint8_t pulseS,
                                            uint16_t protoVer, uint32_t key) {
  return catfishRfnodeBuild(buf, bufLen, CATFISH_RFNODE_CMD_DISCOVER_RSP,
                            tankMask, pulseS, protoVer, key);
}

/** Inverse of catfishRfnodeBuildDiscoverRsp (hub side). Rejects any cmd
 *  that is not DISCOVER_RSP. pulseS 0 = relay default pulse. */
inline bool catfishRfnodeParseDiscoverRsp(const uint8_t *buf, size_t len,
                                          uint8_t *tankMask, uint8_t *pulseS,
                                          uint16_t *protoVer, uint32_t *key) {
  uint8_t cmd = 0xFF, mask = 0, seconds = 0;
  uint16_t seq = 0;
  uint32_t k = 0;
  if (!catfishRfnodeEnvelope(buf, len, &cmd, &mask, &seconds, &seq, &k))
    return false;
  if (cmd != CATFISH_RFNODE_CMD_DISCOVER_RSP) return false;
  if (tankMask) *tankMask = mask;
  if (pulseS) *pulseS = seconds;
  if (protoVer) *protoVer = seq;
  if (key) *key = k;
  return true;
}

#endif // CATFISH_RFNODE_H
