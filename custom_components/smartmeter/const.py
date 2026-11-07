"""Constants for the SML over Matter/Thread integration."""

from __future__ import annotations

from homeassistant.helpers.device_registry import DeviceEntryType

NAME = "SML over Matter/Thread"
DOMAIN = "smartmeter"
CONF_DEVICE_ID = "device_id"
INTEGRATION_VERSION = "2.1.8"
PROJECT_URL = "https://github.com/Mosher23/SML-over-Matter-Thread"


def companion_device_info(device_id: str) -> dict:
    """Describe the HA companion, not the XIAO's separate Matter device."""
    return {
        "identifiers": {(DOMAIN, device_id)},
        "name": NAME,
        "manufacturer": "Mosher23",
        "model": "Home Assistant companion integration",
        "sw_version": INTEGRATION_VERSION,
        "entry_type": DeviceEntryType.SERVICE,
        "configuration_url": PROJECT_URL,
    }


def normalize_legacy_name(name: str | None) -> str | None:
    """Update default project names while preserving custom names."""
    if name in {"IR Smart Meter", "IR Smart Meter PIN"}:
        return NAME
    if name == "Smart Meter PIN":
        return "Smart Meter"
    return name


# Matter MEI vendor cluster exposed by firmware v1.4 and later. The low
# 16-bit cluster suffix must be in Matter's manufacturer-specific range.
DIAGNOSTICS_CLUSTER_ID = 0xFFF1FC01
ACTIVE_POWER_OBIS_SEEN_ATTRIBUTE_ID = 0x00000000
METER_ID_ATTRIBUTE_ID = 0x00000001
METER_MANUFACTURER_ATTRIBUTE_ID = 0x00000002
EXTERNAL_ANTENNA_ATTRIBUTE_ID = 0x00000003
ON_OFF_CLUSTER_ID = 0x0006
ON_OFF_ATTRIBUTE_ID = 0x0000

# Additive v1.21 diagnostics: attribute ID, display name, icon, value kind.
RELIABILITY_FIELDS = (
    (5, "Power status", "pulse", "text"),
    (6, "Imported energy status", "transmission-tower-import", "text"),
    (7, "Exported energy status", "transmission-tower-export", "text"),
    (8, "Meter profile", "file-search", "text"),
    (9, "Profile detection", "magnify", "text"),
    (10, "Power OBIS source", "code-braces", "text"),
    (11, "Power reading age", "timer-outline", "age"),
    (12, "Imported energy reading age", "timer-outline", "age"),
    (13, "Exported energy reading age", "timer-outline", "age"),
    (14, "Valid SML frames", "check-network", "counter"),
    (15, "SML CRC errors", "alert-circle-outline", "counter"),
    (16, "SML decode errors", "alert-circle-outline", "counter"),
    (17, "Rejected readings", "alert-circle-outline", "counter"),
    (18, "Last validation error", "alert-outline", "text"),
    (19, "Reader health", "heart-pulse", "text"),
    (20, "Valid SML frame age", "timer-outline", "age"),
    (21, "UART receive errors", "serial-port", "counter"),
    (22, "Meter ID OBIS source", "code-braces", "text"),
    (23, "Imported energy OBIS source", "code-braces", "text"),
    (24, "Exported energy OBIS source", "code-braces", "text"),
)

# Keep the standard diagnostic view compact. Other reliability entities remain
# available in HA's entity registry and can be enabled individually.
DEFAULT_ENABLED_RELIABILITY_ATTRIBUTE_IDS = frozenset({10, 22, 23})
