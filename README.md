<p align="center">
  <img src="custom_components/smartmeter/brand/icon@2x.png" alt="SML over Matter/Thread integration logo" width="180" height="180">
</p>

# ⚡ SML over Matter/Thread

Read an electricity meter's **SML data through its infrared interface** using
a Seeed XIAO ESP32-C6, and send the readings over **Matter/Thread**. Home
Assistant shows native **Energy**, **Power**, and **External antenna** controls.
Apple Home displays the energy meter without a separate antenna outlet.
The optional companion adds diagnostics, optical PIN entry, and GitHub OTA
updates.

**Latest firmware: [v1.20](https://github.com/Mosher23/SML-over-Matter-Thread/releases/tag/v1.20).**
**Latest companion integration: [v2.1.7](https://github.com/Mosher23/SML-over-Matter-Thread/releases/tag/integration-v2.1.7).**
One universal OTA image supports either antenna. The product and discovery
name is **SML over Matter/Thread**; controllers may retain an existing name
until you rename it in their app.

✅ **Tested with an EMH eHZB electricity meter.** Other meters need compatible
SML output and OBIS values; optical PIN and display controls depend on the
meter model. See the [supported readings](firmware/README.md#device-identity).

> This is a development project, not a certified Matter product. It uses a
> test certificate, development vendor ID, and sample serial number.

## 🧰 What you need

- A Seeed **XIAO ESP32-C6 with 4 MB flash**, a USB data cable, and stable power.
- An electricity meter with **SML output** and a compatible **3.3 V UART** optical head.
- A **Thread Border Router** and a Matter controller, such as Home Assistant.
- For companion OTA updates: **Home Assistant Core 2026.9+** and a Matter
  Server app with **API schema 13+**.

Connect **head TX → XIAO GPIO17 (D7/RX)** and a common ground. For optical
PIN transmission, also connect **XIAO GPIO16 (D6/TX) → head RX**. A head
powered from the XIAO's 5 V pin must still have a **3.3 V-safe UART output**.
See [wiring details](firmware/README.md#hardware).

## 🧲 3D-printable magnetic case

Download the [magnetic case on Printables](https://www.printables.com/model/1895951-magnetic-case-for-sml-over-matterthread-ir-meter-r)
to mount the IR meter reader onto your electricity meter. Two versions are
available, each using four round neodymium magnets:

- **6 × 3 mm:** 6 mm diameter, 3 mm thick.
- **8 × 3 mm:** 8 mm diameter, 3 mm thick.

Choose the case version that matches your magnets.

## 🚀 Setup

### Flash the firmware

Follow the [first-time USB flashing guide](BOOTSTRAP_AND_OTA.md#first-time-usb-flash)
and choose a [release with USB bootstrap files](https://github.com/Mosher23/SML-over-Matter-Thread/releases).
OTA-only releases are for boards that have already been set up.
For an already configured board, use the single **SML-over-Matter-Thread.ota** from
[v1.20](https://github.com/Mosher23/SML-over-Matter-Thread/releases/tag/v1.20). It keeps
the saved antenna selection. This OTA release does not include USB flash files.

### Add the meter to Matter

1. Flash the firmware and power the XIAO within range of your Thread network.
2. Attach the IR head to the meter. If available, unlock detailed SML output
   (**InF ON**) first, so Power is present when Home Assistant discovers it.
3. In **Home Assistant → Settings → Devices & services**, add a **Matter**
   device using the setup code/QR from the firmware's USB serial monitor.
   Alternatively, add it to Apple Home first, then use **Turn On Pairing Mode**
   to share it with Home Assistant. Do not factory-reset an already-paired
   device just to add a second controller.
4. Look under the **Matter** device for native **Energy** and **Power** entities.
   Power is unknown until a valid SML power reading arrives.

If Power is **Unavailable** in HA but Apple Home or the Matter Server app
shows a live `ActivePower` value, reload the HA **Matter** integration while
the head is reading. HA Core 2026.9 can skip Power discovery when the value is
null at startup. Do not erase or re-pair the XIAO as a first step.

### Install the Home Assistant companion

The companion is optional and does **not** replace the Matter integration.
Install it after your meter appears in Home Assistant Matter.

On **Home Assistant 2026.11+**, use the built-in **Marketplace** under Settings.
It replaces HACS; existing HACS installations and custom repositories carry over
automatically. See the [official Marketplace guide](https://www.home-assistant.io/integrations/marketplace/).

[![Open in Home Assistant Marketplace](https://my.home-assistant.io/badges/marketplace_repository.svg)](https://my.home-assistant.io/redirect/marketplace_repository/?owner=Mosher23&repository=SML-over-Matter-Thread&category=integration)

To add the repository manually:

1. Open **Settings → Marketplace → Add from link**.
2. Enter `https://github.com/Mosher23/SML-over-Matter-Thread` and select **Add**.
   Complete the GitHub connection if requested. Choose **Integration** if asked
   for the content type.
3. Open **SML over Matter/Thread** and select **Install**. Companion releases
   use `integration-v…` tags; XIAO firmware releases use `v…` tags.
4. Follow the integration setup flow and select your existing Matter device.
   If setup does not open, use **Settings → Devices & services → Add integration
   → SML over Matter/Thread**. Restart only if Home Assistant asks you to.

Marketplace installs the HA companion. Flash the XIAO firmware separately over
USB or with **OTA Firmware**. Companion updates appear under **Settings →
Updates**; restart Home Assistant when prompted after an integration update.

For **Home Assistant 2026.9–2026.10**, use your existing HACS setup or install
manually. Copy `custom_components/smartmeter` to
`/config/custom_components/smartmeter`, restart HA, and add the integration.
Do not install HACS separately on Home Assistant 2026.11+.

| Integration | Entities and controls |
| --- | --- |
| Home Assistant Matter | Native **Energy**, **Power**, and **External antenna** on/off control. |
| SML over Matter/Thread companion | Seven default diagnostics listed below; **Meter PIN**, **Send PIN**, **Short light pulse**, **Long light pulse**, and **OTA Firmware**. |

Companion **2.1.7** enables only these diagnostics by default:

- **Active antenna**
- **Imported energy OBIS source**
- **Manufacturer**
- **Meter ID**
- **Meter ID OBIS source**
- **OBIS Received**
- **Power OBIS source**

The other 17 diagnostics, including profiles, freshness, reading ages and error
counters, are disabled by default. Enable them individually in **Settings →
Devices & services → Entities** when needed. Source and reliability diagnostics
require firmware **1.21+**; older firmware keeps its existing companion functions.
Home Assistant preserves existing entity preferences during an update, so
already-enabled diagnostics keep their current settings until you change them.

The companion is registered as a software service: its Device info card shows
**Version**, while the separate Matter device shows its actual **Firmware**.

The companion uses your existing Matter device and does not create another
Power sensor. No re-pairing is needed to add it. Keep the companion updated
through Marketplace; its version is independent of the XIAO firmware version.

## 🎛️ Using the meter

### Apple Home and Home Assistant

The firmware exposes one **Electrical Meter** endpoint. Apple Home shows the
meter and its power reading in Energy; Home Assistant also exposes the meter's
On/Off cluster as **External antenna**. There is no separate outlet endpoint.
This layout was confirmed working in Apple Home during development.

Attach the IR head to a transmitting meter to receive live power readings.
Keep **External antenna** out of any HA **HomeKit Bridge** exports if you want
antenna control only in HA. Controller presentation differs; the firmware does
not apply separate access permissions for HA and Apple Home.

### Select the antenna

**Off = built-in ceramic antenna. On = external U.FL/IPEX antenna.**
Use the native HA **External antenna** control; the companion's **Active antenna**
diagnostic shows the saved selection. The same universal OTA serves both choices.

Connect the external antenna before selecting it. BOOT offers local recovery:

- **Hold for 3 seconds, then release:** toggle the saved antenna, flash the
  status LED three times quickly, and restart.
- **Three short taps:** the same antenna toggle, LED confirmation and restart.
- **Hold for 10 seconds:** factory-reset Matter pairing and Thread credentials.

The three flashes confirm that the antenna choice was saved, before reboot.
Antenna gestures preserve pairings, serial number and Matter setup code. The
USB console also accepts `antenna internal` or `antenna external`.

### Enter a meter PIN

Type a four-digit PIN in **Meter PIN**, then press **Send PIN** within two
minutes. The digits remain only in memory; HA state/history get a masked
placeholder. The field clears after the send attempt. The IR transmit LED
must face the meter's optical **control** point, which may differ from the
SML reading point. See [optical PIN details](firmware/README.md#optical-pin-entry).

### Navigate the meter display manually

Press **Short light pulse** for one 500 ms flash or **Long light pulse** for
one 5-second flash. These use the same GPIO16 optical TX head as Send PIN and
pause SML reception while transmitting. Watch the physical meter display;
the button confirms that the pulse was accepted for transmission, not that
the meter acted on it. EMH defines a long press as over 4.5 seconds.

**Be careful with Long light pulse:** its action depends on the currently
displayed meter menu item. On some screens it can toggle InF or erase
historical readings. Do not press it on an E CLr or HIS CLr confirmation
screen unless you intend to clear those values.

## 🔄 Firmware updates

Update the HA companion through **Settings → Updates** (Marketplace) first,
restart Home Assistant when prompted, then open the companion’s **OTA Firmware**
entity. Companion **2.1.7** uses Matter's numeric version
order when determining which firmware updates are available.
Refresh **OTA Firmware** to check immediately. The repository is now
`Mosher23/SML-over-Matter-Thread`; use that URL with Marketplace’s **Add from link**.
Existing entity identifiers and pairings are preserved.
Click **Install** when a compatible newer release appears. The image is
downloaded from GitHub, validated, transferred via Matter Server, and followed
by a XIAO reboot. **Updates never install
automatically.** Keep power and Thread connectivity stable. The separate
native Matter **Firmware** entity may show different update information; use
**OTA Firmware** for this repository's releases.
See the [USB and OTA guide](BOOTSTRAP_AND_OTA.md#update-over-thread).

## 🛠️ Troubleshooting

| Symptom | First check |
| --- | --- |
| Energy works; Power is unknown | Is the head aligned, and is detailed SML output enabled? Check **OBIS Received**. |
| Power is unavailable in HA but visible elsewhere | Reload **Matter** while the meter is transmitting. No re-pairing is needed. |
| Companion diagnostics are unknown | Confirm the Matter device is online and SML is received. Some meters omit ID/manufacturer OBIS fields. |
| Sending a PIN does not change the meter display | Check GPIO16 → head RX, transmit support, and optical control-point alignment. |
| OTA Firmware offers no update | Check `compatibility_issue` and `last_error` in Developer tools → States. The GitHub check runs every six hours; refresh the entity to check sooner. |
| Device disconnects after changing the antenna | Check that the selected antenna is attached. Tap BOOT three times while running to toggle to the other antenna, or select one with `antenna internal` / `antenna external` in the USB console. |
| External antenna control is missing in HA | Check disabled entities on the Matter device and enable **External antenna**. If needed, run **Interview** in Matter Server and reload **Matter**. |

## 📚 More information

- [USB flashing, OTA, and release publishing](BOOTSTRAP_AND_OTA.md)
- [Firmware build, wiring, OBIS mapping, and commissioning](firmware/README.md)
- [Licensing and attribution](firmware/LICENSES.md)

Release checksums guard against accidental corruption; this test-device setup
does not provide production signing, secure boot, or production Matter attestation.

## 💛 Sponsor this project

If this project helps you, you can support its development. Thank you! ☕

- [Ko-fi](https://ko-fi.com/mosher23)
- [Buy Me a Coffee](https://buymeacoffee.com/sergiit)
- [PayPal](https://paypal.me/mosher88)
