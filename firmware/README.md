# SML over Matter/Thread — Firmware

**New to this project?** Start with the [main setup guide](../README.md) for
USB flashing, Matter commissioning, Marketplace installation, and OTA updates.
This page contains the firmware and hardware details.

This is an ESP32-C6 port of Claus Muus' `zigbee-smartmeter-reader`. It keeps the
byte-oriented SML parser and default OBIS mapping, but replaces the Arduino
Zigbee endpoint with a native ESP-IDF/ESP-Matter **Electrical Meter** endpoint
over the ESP32-C6's built-in Thread radio.

## Device identity

- Manufacturer: `SimpleIdeas`
- Product and commissionable name: `SML over Matter/Thread`
- Development VID/PID: `0xFFF1` / `0x8000`
- Software version: `25` (`1.21`)
- Hardware version: `1` (`1.0`)
- Development serial number: `00000001`
- ESP-IDF project name: `IR_Meter_Thread_Matter`

The serial number and test VID/PID are development values. Provision a unique
serial number and production Matter attestation credentials before producing
more than one device.

The preferred OBIS mappings are below; [automatic profiles and fallbacks](../docs/FIRMWARE_1_21.md) extend these mappings.

| OBIS | Meter value | Matter cluster/value |
| --- | --- | --- |
| `1-0:16.7.0*255` | Active power | Electrical Power Measurement / ActivePower, mW |
| `1-0:1.8.0*255` | Imported energy | Electrical Energy Measurement / cumulative imported energy, mWh |
| `1-0:2.8.0*255` | Exported energy | Electrical Energy Measurement / cumulative exported energy, mWh |
| `1-0:96.1.0*255` | Physical meter ID | Read-only manufacturer-specific diagnostic string |
| `129-129:199.130.3*255` | Physical meter manufacturer | Read-only manufacturer-specific diagnostic string |

Unlike the original Zigbee version, no ioBroker or Zigbee2MQTT converter is
needed. A Matter controller reads the standard Matter energy clusters.
Active power remains null until the first valid SML power reading; the reader
does not invent a zero during startup. For Home Assistant commissioning, attach
the IR head to a transmitting meter first so its native Matter Power entity is
discovered. If the meter starts reporting power later but the HA Matter Power
entity remains **Unavailable**, reload the HA Matter integration while a power
reading is present. Do not erase or re-pair the device as a first step.
The two identity diagnostics remain empty if the meter does not transmit those
OBIS entries. Binary octet strings are displayed as uppercase hexadecimal;
printable values are displayed as text. The reader's own Matter vendor, product,
and serial information remain distinct from the physical meter's identity.

## Version baseline

This port uses a pinned, reproducible Espressif/Matter build baseline:

- Matter specification/data model: **Matter 1.6**
- ESP-Matter branch: `release/v1.6`
- ESP-Matter commit: `c91ddfbb08ccc74bb73dd6eca7422178f48b75e1`
- Espressif connectedhomeip commit: `93abd8e6891bb578ea63254fb29d099936f345c8`
- ESP-IDF: **v5.5.5**, the version recommended by that ESP-Matter release
- Thread: **Thread 1.4.0**, native OpenThread FTD on the ESP32-C6 radio

ESP-Matter `main` already targets the in-development Matter 1.7 data model, so
this project deliberately uses `release/v1.6` rather than a moving development
branch. Pinning commits keeps the generated Matter APIs reproducible.

OpenThread comes from ESP-IDF and is configured as Thread 1.4. Do not replace it
independently with an arbitrary upstream OpenThread checkout: Matter's platform
integration and radio configuration must remain version-compatible. Border
Router-only Thread facilities belong on the Thread Border Router, not this
meter endpoint.

## Hardware

Defaults target the Seeed XIAO ESP32-C6:

| Signal | XIAO pin | GPIO |
| --- | --- | --- |
| IR head TX -> ESP RX | D7/RX | 17 |
| ESP TX -> IR head RX (optional) | D6/TX | 16 |
| User LED | internal | 15 |
| Factory-reset button | BOOT | 9 |
| RF-switch control enable | internal | 3 |
| RF-switch antenna select | internal | 14 |

For a continuously transmitting SML meter, only `IR head TX -> GPIO17` and a
common ground are required. PIN transmission additionally requires
`GPIO16 -> IR head RX`. The UART is configured as 9600 baud, 8 data bits, no
parity, and 1 stop bit.

If reusing the Volkszaehler USB reader, connect GPIO17 to the meter-side TX
trace/CP2104 RXD trace. Do not connect the USB D+ or D- wires to the UART. Verify
that the tapped signal is 3.3 V before connecting it to the ESP32-C6.

## Build

Install ESP-IDF v5.5.5, then check out the pinned ESP-Matter release:

```bash
git clone --recursive --branch v5.5.5 https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32c6
source ./export.sh

cd ..
git clone --recursive --branch release/v1.6 https://github.com/espressif/esp-matter.git
cd esp-matter
git checkout c91ddfbb08ccc74bb73dd6eca7422178f48b75e1
git submodule update --init --recursive
./install.sh
source ./export.sh
```

Then build this project:

```bash
cd /path/to/SML-over-Matter-Thread/firmware
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

The project deliberately uses USB Serial/JTAG for logs so UART1 remains
dedicated to the IR reader.

The clean reference build completed successfully with a binary size of
approximately `0x19ed70` bytes and 14% free in the smallest application partition.

## Antenna selection

Build one universal OTA for both antenna choices:

```bash
cd /path/to/SML-over-Matter-Thread
bash scripts/build.sh ota
```

The universal firmware reads the antenna choice from the separate `board_cfg`
NVS partition before enabling the radio. The board flasher provisions that
setting for a new board. Both choices drive GPIO3 low before configuring
GPIO14: low for the internal ceramic antenna, high for external U.FL/IPEX,
following the [official Seeed RF-switch sequence](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/#hardware-overview).

Firmware `1.21` uses a single **Electrical Meter**
(`0x0514`) on endpoint 1: power (`0x0090`), energy (`0x0091`), antenna On/Off
(`0x0006`), PIN input and vendor diagnostics. No second application endpoint,
On/Off Plug-in Unit, added Electrical Sensor type, label provider or Power
Topology server is created. The original meter cluster construction is retained.

HA exposes the On/Off cluster as a native antenna control. If the control
is missing, check disabled entities and let Matter discovery refresh. Exclude that HA entity from HomeKit Bridge if it
should not be bridged to Apple. This uses the controller presentation confirmed
during development; it does not deny Apple protocol-level access to the On/Off
attribute.

**Off = internal, On = external.** The saved `board_cfg` selection remains
authoritative during startup. The companion diagnostic reflects this setting.
Connect the external antenna before selecting it, since losing Thread prevents
a remote reverse command.

While firmware is running, **hold BOOT for at least 3 seconds and release before
10 seconds** to toggle the saved antenna, flash the status LED three times
quickly, and restart. Three short BOOT taps still perform the same action. Neither antenna gesture erases pairings. A
**10-second hold** performs the Matter factory reset.
The USB console also accepts `antenna internal` or `antenna external`.

The firmware label is **1.21** and its Matter software version is **25**.
Matter orders updates by the numeric version, independently of the display label.
One universal OTA supports either saved antenna choice. Antenna gestures show
three quick flashes (80 ms on / 80 ms off) on the onboard status LED before
restarting. The LED pattern overrides breathing and receive pulses.
See [release validation](../docs/RELEASE_TESTING.md) for test coverage.

To change pins, baud rate, or report intervals:

```bash
idf.py menuconfig
```

Open **Smart-meter reader**. The defaults preserve the original five-second
minimum update interval and one-hour maximum refresh interval.

## Commissioning

You need a Matter controller and a Thread Border Router. After flashing, the
serial monitor prints the standard Matter setup payload/QR information. Add it
from your Matter ecosystem in the usual way.

For `chip-tool`, obtain the active Thread operational dataset from your Border
Router and commission with the setup code printed in the log:

```bash
chip-tool pairing ble-thread 1 hex:<ACTIVE_OPERATIONAL_DATASET> 20202021 3840
```

Example reads after commissioning (endpoint 1 is typical; use the endpoint
printed by the firmware):

```bash
# Electrical Power Measurement: ActivePower attribute 0x0008
chip-tool electricalpowermeasurement read active-power 1 1

# Electrical Energy Measurement: cumulative imported/exported energy
chip-tool electricalenergymeasurement read cumulative-energy-imported 1 1
chip-tool electricalenergymeasurement read cumulative-energy-exported 1 1
```

Controller UI support for the newer Matter Electrical Meter device type varies.
The device can commission and expose standards-compliant attributes even when a
particular controller has not yet created dashboard entities for those values.
Use `chip-tool` reads to distinguish controller UI limitations from firmware or
Thread failures.

## Operation

- The GPIO15 user LED slowly breathes while the firmware is running.
- Every CRC-valid SML frame produces a short full-brightness LED pulse, after
  which the breathing animation resumes.
- At startup, the serial log reports `Saved RF antenna: internal` or
  `Saved RF antenna: external`.
- The Matter On/Off antenna switch persists its choice. The USB console command
  `antenna internal` (or `antenna external`) saves the choice and restarts;
  use `antenna internal` to recover from a wrong remote selection.
- Three short taps of BOOT while firmware is running toggle the saved
  antenna and restart, without erasing Matter credentials. A 3-second hold
  followed by release performs the same action.
- Reports are emitted only after the complete SML frame passes its CRC check.
- Hold BOOT for ten seconds while the firmware is running to erase Matter
  fabrics and Thread credentials.
- The Matter shell is enabled; `matter esp factoryreset` is an alternative.

## Optical PIN entry

The Electrical Meter endpoint also exposes a Matter **Keypad Input** server
with Number Keys and Navigation Key Codes. A controller sends four numeric `SendKey`
commands followed by `Enter` (or `Select`). `Clear` or `Exit` cancels a partial
entry. The PIN is kept only in RAM, is never written to NVS, and is not printed
in firmware logs.

On `Enter`, the firmware pauses SML reception and drives the IR head through
UART TX on GPIO16. It sends two short activation flashes, emits the number of
short flashes represented by each digit, and leaves at least three seconds of
dark time between digits. A zero digit is represented by no flash during that
digit interval. SML reception resumes after the meter has checked the fourth
digit.

For manual menu navigation, `SendKey(Up)` requests one short optical pulse
(500 ms by default), while `SendKey(Down)` requests one long pulse (5 seconds
by default). The Home Assistant companion provides buttons for both actions.
Both commands use the same UART queue as PIN entry and reject overlapping
transmissions. The Matter response acknowledges queue acceptance; it cannot
confirm that the physical meter sensed the light. An EMH long optical press is
more than 4.5 seconds and may toggle settings or clear historical values,
depending on the screen. Avoid long presses on a clear-history menu unless
that is intended.

For first-time alignment and timing calibration, use the USB console while
watching the meter display:

```text
meter-pin 1234
```

The console echoes typed input, so use it only for local testing. Normal use
should send the Keypad Input commands over the encrypted Matter session. The
short-flash, long-flash, inter-flash, wake-settle, digit-wait, and TX inversion settings are
available under **Smart-meter reader** in `idf.py menuconfig`.

On the EMH eHZB, the optical control element and the INFO/SML data interface
are physically distinct front-panel elements. The write LED must illuminate
the optical control element; depending on the head's optics, this can require
repositioning the head or using a dedicated IR LED. Confirm that the display
changes to PIN entry before trying a real PIN repeatedly, because the meter can
temporarily block further attempts after too many incorrect entries.

## SML reliability and mapping profiles

See [v1.21 reliability details](../docs/FIRMWARE_1_21.md) for validated mappings,
independent freshness timeouts, diagnostics, and compatibility limits.

To extend the OBIS map, add an exact identifier and its documented units in
`smart_meter.cpp`, add selection/validation rules in `meter_readings.h`, and
exercise the mapping with a complete CRC-valid telegram in the host tests.
Do not infer a meter model from its manufacturer alone, sum overlapping energy
registers, or relax CRC/unit checks globally for one meter's quirks.

## Attribution and licensing

The port is derived from:

- `zigbee-smartmeter-reader`, Copyright Claus Muus, GPL-3.0
- `sml_parser`, by olliiiver, LGPL-2.1
- ESP-Matter example patterns, Espressif Systems, CC0/public-domain example code

See [LICENSES.md](LICENSES.md). Preserve the upstream notices when
redistributing modified source.
