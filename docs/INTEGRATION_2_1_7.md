# Companion integration v2.1.7

The default Diagnostic view now enables exactly these seven entities:

- Active antenna
- Imported energy OBIS source
- Manufacturer
- Meter ID
- Meter ID OBIS source
- OBIS Received
- Power OBIS source

The remaining 17 diagnostics are disabled by default and remain available to
enable individually from the entity settings. Existing enabled/disabled choices
are preserved on upgrade. No entity IDs or stored history are deleted.

The companion device is now registered as a software service. Home Assistant
shows **Version: 2.1.7** for the integration, instead of labeling the integration
version as firmware. The separate Matter device continues to show XIAO firmware.

This release also publishes the v1.21 reliability diagnostics previously supplied
as companion 2.1.6: independent freshness/status/age, selected OBIS sources,
automatic mapping profile, and receive/validation counters. These diagnostics
require firmware 1.21 or newer. Older supported firmware retains identity,
antenna, PIN/pulse and OTA features.

## Install or update

Use Settings → Marketplace → Add from link and add
https://github.com/Mosher23/SML-over-Matter-Thread if it is not already present.
Install/update the companion release **integration-v2.1.7**. Restart Home Assistant
when prompted after updating.

For manual installation, extract **smartmeter-2.1.7.zip** into the Home Assistant
configuration directory; it contains `custom_components/smartmeter`. Restart HA.
No meter removal or re-pairing is required.

This is a Home Assistant integration release. It does not install or publish a
new XIAO firmware image. Firmware releases retain their separate `v…` tags and
GitHub Latest designation, so the OTA Firmware checker keeps working.
