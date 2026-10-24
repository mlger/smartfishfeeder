/**
 * @file catfish_rfnode.h
 * @brief Catfish Feeder — RF "node" datagram contract (v1.8.0).
 *
 * Shared by the optional wireless feed-button/sensor transports on the
 * display unit: ESP-NOW (net_espnow) and nRF24L01+ (net_rf24). Both carry
 * the SAME 12-byte payload; only the radio layer differs. The relay unit is
 * NOT involved (the display hub resolves node requests into the same feed
 * pipeline as touch/MQTT/dashboard requests).
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
 *   [3]  cmd     (CATFISH_RFNODE_CMD_FEED)
 *   [4]  tankMask (bit 0 = tank 1 .. bit 3 = tank 4; 0 = hub default)
 *   [5]  seconds (0 = hub default pulse; else clamped 1..60)
 *   [6..7] seq   (uint16 LE, sender increments; hub drops repeats)
 *   [8..11] key (uint32 LE, FNV-1a of the pairing token)
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
#define CATFISH_RFNODE_PKT_LEN  12

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
 * Validate + parse a received packet.
 * @return true when the packet is a well-formed, token-keyed feed request.
 *         Outputs are clamped; *cmd = 0xFF on any rejection.
 */
inline bool catfishRfnodeParse(const uint8_t *buf, size_t len, uint8_t *cmd,
                               uint8_t *tankMask, uint8_t *seconds,
                               uint16_t *seq, uint32_t *key) {
  if (cmd) *cmd = 0xFF;
  if (!buf || len < CATFISH_RFNODE_PKT_LEN) return false;
  if (buf[0] != CATFISH_RFNODE_MAGIC0 || buf[1] != CATFISH_RFNODE_MAGIC1)
    return false;
  if (buf[2] != CATFISH_RFNODE_VER) return false;
  if (buf[3] != CATFISH_RFNODE_CMD_FEED) return false;

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

#endif // CATFISH_RFNODE_H
