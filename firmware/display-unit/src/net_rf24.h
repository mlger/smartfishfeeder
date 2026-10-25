/**
 * @file net_rf24.h
 * @brief Optional nRF24L01+ receiver for wireless feed nodes (v1.8.0).
 *
 * Wiring (display unit, shared with the TF-card SPI bus):
 *   nRF24 VCC  -> 3.3 V (with a 10-100 uF cap across VCC/GND)
 *   nRF24 GND  -> GND
 *   nRF24 SCK  -> GPIO12 (shared with TF)
 *   nRF24 MOSI -> GPIO11 (shared with TF)
 *   nRF24 MISO -> GPIO13 (shared with TF)
 *   nRF24 CE   -> GPIO43 (RS485 RXD terminal, freed for this)
 *   nRF24 CSN  -> GPIO44 (RS485 TXD terminal, freed for this)
 *   IRQ        -> not connected (polling task)
 *
 * All radio transfers go through spi_bus_guard so the card and the radio
 * coexist on one controller. A 4 KB "rf-task" polls the radio every 50 ms
 * and pushes accepted node datagrams into the shared UiEvent queue.
 *
 * Puck-side settings: 1 Mbps, channel CATFISH_RF24_CHANNEL (default 76),
 * 5-byte address CATFISH_RF24_ADDR (see net_rf24.cpp), payload = the
 * catfish_rfnode.h datagram.
 *
 * Feature gate: v1.10.0 defaults to OFF (ESP-NOW is the default radio —
 * no extra hardware). Opt in with -DCATFISH_FEATURE_NRF24=1.
 */
#ifndef CATFISH_NET_RF24_H
#define CATFISH_NET_RF24_H

#include <Arduino.h>
#include <stdint.h>

#ifndef CATFISH_FEATURE_NRF24
#define CATFISH_FEATURE_NRF24 0
#endif

#ifndef CATFISH_RF24_CHANNEL
#define CATFISH_RF24_CHANNEL 76
#endif
#ifndef CATFISH_RF24_CE
#define CATFISH_RF24_CE 43
#endif
#ifndef CATFISH_RF24_CSN
#define CATFISH_RF24_CSN 44
#endif

namespace net_rf24 {

enum State : uint8_t {
  Disabled = 0, // compiled out
  Error,        // begin() failed (module absent / wiring)
  Listening,
};

State state();

/** Init the radio + start the poll task. Call once from setup(). */
bool begin();

/** Re-derive the packet key from the pairing token (loop() context). */
void onTokenChanged();

} // namespace net_rf24

#endif // CATFISH_NET_RF24_H
