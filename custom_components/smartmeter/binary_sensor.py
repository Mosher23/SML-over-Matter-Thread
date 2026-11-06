"""Diagnostic entity for whether active-power OBIS has been received."""

from __future__ import annotations

from matter_server.common.helpers.util import create_attribute_path

from homeassistant.components.binary_sensor import BinarySensorEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from . import SmartMeterRuntimeData
from .const import ACTIVE_POWER_OBIS_SEEN_ATTRIBUTE_ID, DIAGNOSTICS_CLUSTER_ID, companion_device_info


async def async_setup_entry(
    hass: HomeAssistant, entry: ConfigEntry, async_add_entities: AddEntitiesCallback
) -> None:
    """Set up the Matter diagnostic binary sensor."""
    async_add_entities([ActivePowerObisSeenSensor(entry.runtime_data)])


class ActivePowerObisSeenSensor(CoordinatorEntity, BinarySensorEntity):
    """Latch whether validated active power has been received during this boot."""

    _attr_has_entity_name = True
    _attr_name = "OBIS Received"
    _attr_icon = "mdi:meter-electric-outline"
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _attr_entity_registry_enabled_default = True

    def __init__(self, runtime: SmartMeterRuntimeData) -> None:
        """Initialize the entity."""
        super().__init__(runtime.coordinator)
        self._runtime = runtime
        self._attr_unique_id = f"{runtime.node_id}-active-power-obis-seen"
        self._attr_device_info = companion_device_info(runtime.device_id)
        self._attribute_path = create_attribute_path(
            runtime.endpoint_id,
            DIAGNOSTICS_CLUSTER_ID,
            ACTIVE_POWER_OBIS_SEEN_ATTRIBUTE_ID,
        )

    @property
    def is_on(self) -> bool:
        """Return true only after the meter has sent active-power OBIS."""
        return bool((self.coordinator.data or {}).get(self._attribute_path, False))
