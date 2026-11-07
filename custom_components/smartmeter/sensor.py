"""Read-only diagnostics identifying the physical meter behind the IR head."""

from __future__ import annotations

from matter_server.common.helpers.util import create_attribute_path

from homeassistant.components.sensor import SensorEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from . import SmartMeterRuntimeData
from .const import (
    DEFAULT_ENABLED_RELIABILITY_ATTRIBUTE_IDS,
    DIAGNOSTICS_CLUSTER_ID,
    DOMAIN,
    EXTERNAL_ANTENNA_ATTRIBUTE_ID,
    METER_ID_ATTRIBUTE_ID,
    METER_MANUFACTURER_ATTRIBUTE_ID,
    ON_OFF_ATTRIBUTE_ID,
    ON_OFF_CLUSTER_ID,
    RELIABILITY_FIELDS,
    companion_device_info,
)
from .meter_identity import format_meter_id


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    """Add SML identity fields; power is provided by the Matter integration."""
    runtime = entry.runtime_data
    registry = er.async_get(hass)
    legacy_power_id = registry.async_get_entity_id(
        "sensor", DOMAIN, f"{runtime.node_id}-meter-active-power"
    )
    if legacy_power_id is not None:
        legacy_power = registry.async_get(legacy_power_id)
        if legacy_power is not None and legacy_power.config_entry_id == entry.entry_id:
            registry.async_remove(legacy_power_id)
    async_add_entities(
        [
            MeterIdSensor(runtime),
            MeterIdentitySensor(
                runtime,
                METER_MANUFACTURER_ATTRIBUTE_ID,
                "Manufacturer",
                "factory",
            ),
            ActiveAntennaSensor(runtime),
            *(ReliabilitySensor(runtime, *field) for field in RELIABILITY_FIELDS),
        ]
    )


class ActiveAntennaSensor(CoordinatorEntity, SensorEntity):
    """Report the actual RF-switch state, independently of the UI command."""

    _attr_has_entity_name = True
    _attr_name = "Active antenna"
    _attr_icon = "mdi:antenna"
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _attr_entity_registry_enabled_default = True

    def __init__(self, runtime: SmartMeterRuntimeData) -> None:
        super().__init__(runtime.coordinator)
        self._attr_unique_id = f"{runtime.node_id}-active-antenna"
        self._attr_device_info = companion_device_info(runtime.device_id)
        self._switch_path = create_attribute_path(
            runtime.endpoint_id, ON_OFF_CLUSTER_ID, ON_OFF_ATTRIBUTE_ID
        )
        self._diagnostic_path = create_attribute_path(
            runtime.endpoint_id,
            DIAGNOSTICS_CLUSTER_ID,
            EXTERNAL_ANTENNA_ATTRIBUTE_ID,
        )

    @property
    def native_value(self) -> str | None:
        data = self.coordinator.data or {}
        state = data.get(self._switch_path, data.get(self._diagnostic_path))
        if type(state) is not bool:
            return None
        return "External" if state else "Internal"


class MeterIdentitySensor(CoordinatorEntity, SensorEntity):
    """Display a validated SML identity value reported over Matter."""

    _attr_has_entity_name = True
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _attr_entity_registry_enabled_default = True

    def __init__(
        self, runtime: SmartMeterRuntimeData, attribute_id: int, name: str, icon: str
    ) -> None:
        """Initialize one diagnostic text sensor."""
        super().__init__(runtime.coordinator)
        self._runtime = runtime
        self._attr_name = name
        self._attr_icon = f"mdi:{icon}"
        self._attr_unique_id = f"{runtime.node_id}-meter-identity-{attribute_id}"
        self._attr_device_info = companion_device_info(runtime.device_id)
        self._attribute_path = create_attribute_path(
            runtime.endpoint_id, DIAGNOSTICS_CLUSTER_ID, attribute_id
        )

    @property
    def native_value(self) -> str | None:
        """Return unknown until a valid SML frame contains this OBIS field."""
        value = (self.coordinator.data or {}).get(self._attribute_path)
        if isinstance(value, bytes):
            value = value.decode("ascii", errors="replace")
        return value if isinstance(value, str) and value else None


class MeterIdSensor(MeterIdentitySensor):
    """Show the printed meter ID while retaining the firmware's raw value."""

    def __init__(self, runtime: SmartMeterRuntimeData) -> None:
        super().__init__(runtime, METER_ID_ATTRIBUTE_ID, "Meter ID", "identifier")

    @property
    def native_value(self) -> str | None:
        return format_meter_id(super().native_value)

    @property
    def extra_state_attributes(self) -> dict[str, str] | None:
        raw = super().native_value
        return {"raw_meter_id": raw} if raw is not None else None


class ReliabilitySensor(MeterIdentitySensor):
    """Firmware-validated status, mapping, age or receive counter.

    Power and energy remain native Matter entities. Ages are sampled on the
    board every five seconds and polled together every thirty seconds.
    """

    def __init__(self, runtime, attribute_id, name, icon, kind):
        super().__init__(runtime, attribute_id, name, icon)
        self._kind = kind
        self._attr_entity_registry_enabled_default = (
            attribute_id in DEFAULT_ENABLED_RELIABILITY_ATTRIBUTE_IDS
        )
        if kind == "age":
            self._attr_native_unit_of_measurement = "s"
            self._attr_device_class = "duration"

    @property
    def native_value(self):
        value = (self.coordinator.data or {}).get(self._attribute_path)
        if self._kind == "text":
            return super().native_value
        if type(value) is not int or not 0 <= value <= 0xFFFFFFFF:
            return None
        if self._kind == "age" and value == 0xFFFFFFFF:
            return None  # Never received is unknown, not a multi-year age.
        return value
