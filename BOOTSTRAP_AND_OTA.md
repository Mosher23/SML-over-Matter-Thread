# Flashing and updating the XIAO ESP32-C6

There are two separate installs:

- **USB flash:** puts firmware on a new XIAO.
- **Marketplace install (Home Assistant 2026.11+):** adds the optional Home
  Assistant companion. It does not flash the board. Open **Settings → Marketplace → Add from link** and use
  this repository’s GitHub URL. After setup, the companion's
  **OTA Firmware** entity can install later releases over Thread.

## Before you start

- Use a Seeed **XIAO ESP32-C6 with 4 MB flash**, a USB **data** cable, and
  stable power. Choose **internal** for the ceramic antenna or **external**
  only when an external antenna is physically connected.
- On macOS, have `python3` available. Release flashing needs `esptool`
  and `pyserial`, but **not** a local ESP-IDF installation.
- For OTA, use Home Assistant Core 2026.9+ and a Matter Server app with API
  schema 13+. Core 2026.9.2 / Matter Server app 9.2.0 were tested.
- Do **not** use `erase_flash` merely to update. It destroys Matter/Thread
  commissioning data.

## First-time USB flash

Open [GitHub releases](https://github.com/Mosher23/SML-over-Matter-Thread/releases)
and choose a release containing the USB files below. OTA-only releases update
boards that have already been set up and do not contain USB bootstrap files.
Download these **individual files into one folder** (no ZIP archive):

```text
flash.py
SHA256SUMS
bootloader.bin
partition-table.bin
ota_data_initial.bin
IR_Power_Meter_internal.bin   # choose this or the external image
IR_Power_Meter_external.bin
```

You need only the image matching your antenna. On a Mac, open Terminal and
create a Python environment in that folder:

```bash
cd "/path/to/downloaded-release-folder"
python3 -m venv .venv
source .venv/bin/activate
python -m pip install esptool pyserial
python -m serial.tools.list_ports
```

Find the XIAO's **USB JTAG/serial** port (for example,
`/dev/cu.usbmodem21201`). Use your actual port in **one** of these commands:

```bash
python flash.py --port /dev/cu.usbmodem21201 --antenna internal
python flash.py --port /dev/cu.usbmodem21201 --antenna external
```

The script verifies the downloaded files and asks you to type `FLASH`.
It writes the bootloader, partition table, OTA selection, and application—not
the whole flash. The selected antenna is stored separately on first boot;
flashing the other antenna image later does **not** override an existing
saved choice. You can change that saved choice through
the Matter On/Off antenna switch without flashing. Off selects the internal
antenna; On selects the external antenna. If a switch to the external antenna
disconnects Thread, use the XIAO's USB serial console command
`antenna internal` to restore the internal antenna and restart, or tap BOOT
three times while the firmware is running. A Matter
factory reset does not change the saved antenna setting.

Current firmware exposes one Electrical Meter endpoint. HA also exposes its
On/Off cluster as the antenna control; Apple Home shows the meter without a
separate antenna outlet. Off selects internal; On selects external. Keep the
HA antenna entity out of any HomeKit Bridge exports.

Hold BOOT for 3 seconds and release to toggle the saved antenna. The status
LED flashes three times quickly before restart. Three short taps do the same;
a 10-second hold factory-resets Matter pairing. Connect an external antenna
before selecting it. See [antenna controls](firmware/README.md#antenna-selection).

After reboot, [add the meter to Matter](README.md#add-the-meter-to-matter)
and [install the optional companion](README.md#install-the-home-assistant-companion).

## Update over Thread

The latest [v1.21 release](https://github.com/Mosher23/SML-over-Matter-Thread/releases/tag/v1.21)
contains one universal OTA image. Update the HA companion through
**Settings → Updates** (Marketplace on Home Assistant 2026.11+) to
**2.1.7**, restarting Home Assistant when prompted, before checking
for it. Update order follows Matter software version **25**, independently of
the display label.

After flashing and commissioning the meter, use
**SML over Matter/Thread → OTA Firmware** in Home Assistant:

1. Keep the XIAO powered and connected to Thread.
2. Open **OTA Firmware** and check **Installed version** and **Latest version**.
3. If a compatible newer version is offered, click **Install**. Publishing a
   GitHub release never starts an update automatically.
4. Allow time for the transfer, reboot, and health check. Confirm the new
   firmware version on the Matter device afterward.

The companion checks the latest **stable** GitHub release every six hours.
To check sooner, use HA's **Update entity** action on **OTA Firmware**.
Before transfer, the integration validates the release manifest, SHA-256,
Matter OTA header, and embedded application version. If it fails, inspect
the entity's `ota_status`, `compatibility_issue`, and `last_error`
attributes in **Developer tools → States**.

Marketplace updates the companion integration; **OTA Firmware** updates the
XIAO. Existing HACS installations move to Marketplace automatically with Home
Assistant 2026.11. The [main setup guide](README.md#install-the-home-assistant-companion)
covers Marketplace, earlier Home Assistant versions, and manual installation.

The native Matter **Firmware** entity is not this GitHub release checker.
It may show different available-version information. Use the companion
**OTA Firmware** entity for this repository's releases.

## Building and publishing (maintainers)

The CI build uses ESP-IDF **v5.5.5** and ESP-Matter commit
`c91ddfbb08ccc74bb73dd6eca7422178f48b75e1` from `release/v1.6`.
See [firmware build instructions](firmware/README.md#build). Apply
[`patches/esp-idf-nimble-conversion.patch`](patches/esp-idf-nimble-conversion.patch)
to a fresh local SDK; CI applies it automatically.

```bash
bash scripts/build.sh ota
python scripts/package_release.py --ota-only --output dist/release
```

The release workflow builds and packages one universal OTA image. It does not
compile the antenna-specific USB variants. The universal OTA build requires
an antenna choice already saved during initial setup;
that choice can subsequently be changed over Matter. The packaging
script checks version consistency and image size. Increment numeric and
text version markers together in `firmware/main/MatterProjConfig.h`,
`firmware/CMakeLists.txt`, and `firmware/sdkconfig.defaults` as appropriate.

Push tested source, then publish a stable GitHub release whose tag matches
the firmware string version (for example, `v1.21`). GitHub Actions builds
the pinned SDK, runs tests, checks the tag, and attaches individual assets
with `ota-manifest.json` **last**. Do not replace published assets or reuse
a Matter numeric software version. Test an image on an accessible meter
before recommending OTA broadly.

This development release trusts the GitHub repository and checksums; it is
not a production signed-firmware chain. Production devices need unique
serial numbers, production Matter attestation, signed images, and secure
boot.
