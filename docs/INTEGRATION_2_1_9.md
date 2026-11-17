# SML Meter v2.1.9

SML electricity readings over Matter and Thread.

The Home Assistant companion is now **SML Meter**. Its setup dialog,
integration listing, companion service device, and Marketplace metadata use
the new title. Existing entries using the old default **SML over Matter/Thread**
name migrate automatically on setup. Custom names remain unchanged.

The `smartmeter` domain, config entry data, entity unique IDs, and Matter
pairings are unchanged. Existing entity IDs remain valid. Firmware **v1.21**
can remain installed; no device reset, firmware OTA, or re-pairing is needed.

This release also ships `translations/en.json` for the setup dialog and
validation errors, declares the Energy / Meters & monitoring category,
corrects the integration's data classification to local polling, and runs
HACS validation without ignored checks, hassfest, and companion tests.
Meter data stays local through Matter; firmware update checks still use GitHub.

Update the companion through Marketplace (or HACS on older Home Assistant
versions) and restart Home Assistant to load the new code. To roll back,
select the previous **integration-v2.1.8** release and restart Home Assistant.
