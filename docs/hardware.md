# Hardware — Wiring & Assembly

Practical guide for building the two-unit Catfish Feeder. Pin-level details for the display board live in the verified notes: [notes/hardware-4.3b.md](notes/hardware-4.3b.md) (source of truth — do not guess pins). Flashing and configuration are covered in [firmware.md](firmware.md); the relay unit's network interface is documented in [api.md](api.md).

## System overview

| Unit | Hardware | Job |
|------|----------|-----|
| Display Unit | Waveshare ESP32-S3-Touch-LCD-4.3B | 800x480 touch UI (Feed Now / Schedule / Settings), NTP clock, WiFi provisioning, sends feed commands |
| Relay Unit | ESP8266 NodeMCU V3 + 5 V 1CH optocoupler relay | Listens for HTTP/UDP commands, pulses the relay that runs the 5 V feeder motor, OTA updates |
| Power stage | 5 V PSU (>= 2 A), 1N4007 | Motor and relay contact power, flyback protection |

Both units connect to the same 2.4 GHz WiFi network. Only the relay unit touches the motor wiring.

## Display Unit — Waveshare ESP32-S3-Touch-LCD-4.3B

Key verified facts (full table in [notes/hardware-4.3b.md](notes/hardware-4.3b.md)):

- ESP32-S3-WROOM-1-N16R8: dual-core 240 MHz, 16 MB QIO flash, 8 MB octal PSRAM (`qio_opi`)
- 4.3" IPS 800x480 RGB565 parallel panel (ST7262-class), GT911 capacitive 5-point touch on I2C (SDA GPIO8 / SCL GPIO9 / INT GPIO4)
- CH422G IO expander drives LCD reset, touch reset, backlight enable (DISP) and TF card CS. The backlight is a **switch, not PWM** — on/off only
- USB Type-C: power + native USB flashing. Typical draw ~450 mA at 5 V

Power options (any one of):

| Option | Connector | Notes |
|--------|-----------|-------|
| 5 V USB-C | Type-C port | Default for development; use a data-capable cable, ~450 mA |
| 7-36 V DC | DC input terminal on the board | Good for a permanent install from a 12 V brick |
| 3.7 V Li-ion | Battery connector | Portable use; observe polarity and battery safety |

No other wiring is needed — the display unit talks to the relay unit purely over WiFi.

Flashing buttons: hold **BOOT** while connecting USB-C to enter the ROM bootloader (needed if an upload fails to sync). **RST** resets the board.

## Relay Unit — wiring

### Logic side (NodeMCU to relay module)

| NodeMCU V3 pin | Relay module pin | Notes |
|----------------|------------------|-------|
| D1 (GPIO5) | IN | Feed pulse from firmware; signal level depends on relay logic |
| 5V (VIN) | VCC | Relay coil + optocoupler supply (from the NodeMCU's USB/5 V rail) |
| GND | GND | Signal ground |

Optional: the NodeMCU on-board LED (D4/GPIO2, active-low) is used by the firmware as a status indicator — nothing to wire.

### Contact side (relay to motor and PSU)

| From | To | Notes |
|------|----|-------|
| Relay **COM** | PSU **+5 V** | Motor supply enters through the common contact |
| Relay **NO** (normally open) | Motor **+** | Motor only energized while feeding |
| Motor **−** | PSU **GND** (same rail as PSU −) | |
| 1N4007 **cathode (band)** | Motor **+** terminal | Flyback diode across the motor |
| 1N4007 **anode** | Motor **−** terminal | Reverse-biased during normal running, shorts the inductive kick |

The firmware always switches the **NO** contact, so the motor is idle when the board is unpowered or booting.

### Active-low relay boards

Many cheap 5 V 1-channel optocoupler relay boards are **ACTIVE-LOW**: the relay pulls in when IN is driven LOW, and is energized while the MCU pin floats during ESP8266 boot. The firmware exposes `RELAY_ACTIVE_LEVEL` in `config.h` so the idle level can be matched to your board. After flashing, **verify the relay's idle state at boot** — the motor must not run while the device starts or sits idle. If it does, flip `RELAY_ACTIVE_LEVEL` and re-flash, or move the motor lead to the NC contact and invert the logic accordingly (not recommended; prefer the config change).

### Grounding

NodeMCU GND, relay module GND and PSU GND must share a **common ground**. The relay contacts switch the PSU +5 V rail, but the control side only works if the grounds are tied together. Use one star point at the PSU terminal block.

## Power sizing

- Measure or look up the motor's **stall current** — an auger jammed by pellets can draw several times its free-run current. The PSU must supply stall current plus the NodeMCU (~80-200 mA) plus the relay coil (~70 mA) without sagging below 5 V.
- A **5 V >= 2 A** supply is the baseline for a typical small DC auger motor; upsize if your motor stalls higher.
- **Never power the motor from the NodeMCU 3V3 pin.** The 3.3 V regulator cannot supply motor currents and the inductive spike will reset or damage the board. The motor runs from the external 5 V PSU through the relay contacts only.
- Keep motor leads reasonably short and thick (20 AWG or better); a long thin pair will drop voltage and cause brown-outs.
- If both units sit near each other, one PSU can power the relay unit (via its micro-USB or VIN) and the motor; the display unit has its own supply (USB-C / 7-36 V / battery).

## Physical mounting

- Mount the **auger outlet above the water line** — pellets should fall into the tank, and the mechanism must never touch or siphon water.
- Form a **drip loop** in every cable running toward the enclosure so condensation and splashes run off instead of into the case.
- Use an **IP-rated enclosure** (IP54 or better) with cable glands; keep the display unit sealed or splash-shielded if mounted near the tank rim.
- Isolate the hopper and auger tube from motor vibration with foam tape; pellets powder up when crushed and dust jams augers — leave drain/clean-out access.
- Food-safe materials for anything pellets touch (PETG, HDPE, stainless).

## Safety

- **Low voltage only.** Everything here is 5 V DC. Never switch mains voltage with this build, and never bring mains wiring near the tank. If you need a mains-powered variant, hire a licensed electrician.
- **Test with a multimeter first**: verify PSU polarity, verify 5 V at the relay VCC, verify the motor polarity before connecting, and verify the relay contact opens/closes when expected.
- **Verify relay idle state at boot** (see above) before leaving the feeder unattended — a misconfigured active level can grind the motor continuously.
- Add a physical on/off switch on the PSU and do not leave the feeder powered while unattended until you have observed several scheduled feeds.

## Feeding mechanism options

| Mechanism | How it works | Pros | Watch out for |
|-----------|--------------|------|---------------|
| Auger screw | Motor turns a screw in a tube; pellets advance one pitch per turn | Precise portions, cheap, common | Jamming from damp pellets; needs clean, dry feed |
| Roller drum | Motor turns a paddle drum over an opening | Simple, tolerant of pellets | Portions less repeatable |
| Archimedes screw (large pitch) | Same principle as auger with a wider tube | Handles bigger pellets | More motor torque needed |

**Portion calibration:** start with **2-3 s feed pulses** (`POST /feed` with `duration_ms: 2000`, or the Feed Now button), weigh the dispensed pellets, and adjust the duration until the portion matches your stocking density. Record the calibrated duration in the Schedule screen. Re-check after changing pellet brand or size — pellet length strongly affects grams-per-second.

## Assembly checklist

1. Flash both units ([firmware.md](firmware.md)) **before** final assembly and confirm the UI boots and the portal appears.
2. Wire the relay logic side, then the contact side, then the flyback diode. Re-check diode orientation (band to motor +).
3. Power everything from the bench PSU and verify: relay idle at boot, relay clicks on Feed Now, motor runs only during the pulse.
4. Mount the mechanism above the water, add the drip loop, close the enclosure.
5. Calibrate portions (2-3 s start), set the schedule, observe a full day before trusting it.
