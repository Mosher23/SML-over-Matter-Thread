# SML Meter v2.1.10

Update the companion to **2.1.10** for firmware update checks with the new
GitHub release layout. GitHub's **Latest** release now identifies the companion
integration rather than firmware v1.21. Older companions that follow only
Latest can report a missing firmware manifest until updated.

The release source includes the complete English config-flow fallback at
`custom_components/smartmeter/translations/en.json`, identical to `strings.json`.
The official `home-assistant/actions/hassfest@master` action runs in its own
`.github/workflows/hassfest.yml` on pushes, pull requests, a daily schedule,
and manual dispatch. HACS validation runs without ignored checks.

When Latest is a companion release, **OTA Firmware** searches up to 50 recent
releases for the newest published stable firmware tag. It skips integration
releases, drafts, and prereleases. Firmware metadata, product identity,
compatibility, checksums, image headers, and explicit installation checks
remain enforced. Polling never transfers or installs firmware. An incomplete
newest firmware release produces an error rather than selecting older firmware.

The `smartmeter` domain, entity IDs, custom names, Matter pairing, and firmware
are unchanged. Firmware **v1.21** can remain installed. Update the integration
through Marketplace/HACS and restart Home Assistant to activate it.
