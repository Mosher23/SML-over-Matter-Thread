"""SML Meter custom integration."""

from __future__ import annotations

import asyncio
from dataclasses import dataclass, field
from time import monotonic
from typing import Callable

from chip.clusters import Objects as Clusters

from homeassistant.components.matter.helpers import (
    get_matter,
    node_from_ha_device_id,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import Platform
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import ConfigEntryNotReady
from homeassistant.helpers import device_registry as dr

from .const import CONF_DEVICE_ID, DOMAIN, normalize_legacy_name
from .coordinator import SmartMeterCoordinator

PLATFORMS: list[Platform] = [Platform.BINARY_SENSOR, Platform.BUTTON, Platform.SENSOR, Platform.TEXT, Platform.UPDATE]


@dataclass
class SmartMeterRuntimeData:
    """Matter objects used by the integration's entities."""

    matter_client: object
    node_id: int
    endpoint_id: int
    device_id: str
    coordinator: SmartMeterCoordinator
    pending_pin: str | None = None
    pin_expires_at: float = 0
    clear_pin_display: Callable[[], None] | None = None
    pin_send_lock: asyncio.Lock = field(default_factory=asyncio.Lock)

    def stage_pin(self, pin: str) -> None:
        """Hold a PIN only in memory until the user presses Send."""
        self.pending_pin = pin
        self.pin_expires_at = monotonic() + 120

    def take_pin(self) -> str | None:
        """Consume a staged PIN once, discarding expired entries."""
        pin = self.pending_pin if monotonic() < self.pin_expires_at else None
        self.pending_pin = None
        self.pin_expires_at = 0
        if self.clear_pin_display is not None:
            self.clear_pin_display()
        return pin


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up PIN entry and diagnostics for the selected Matter meter."""
    # Existing config entries retain their original title across integration
    # updates. Migrate only known old titles; preserve other user-chosen names.
    new_title = normalize_legacy_name(entry.title)
    if new_title != entry.title:
        hass.config_entries.async_update_entry(entry, title=new_title)

    node = node_from_ha_device_id(hass, entry.data[CONF_DEVICE_ID])
    if node is None:
        raise ConfigEntryNotReady("The selected Matter device is not available")

    endpoint = next(
        (
            endpoint
            for endpoint in node.endpoints.values()
            if endpoint.has_cluster(Clusters.KeypadInput)
        ),
        None,
    )
    if endpoint is None:
        raise ConfigEntryNotReady(
            "The selected device does not expose the Smart Meter Keypad Input cluster"
        )

    matter_client = get_matter(hass).matter_client
    if matter_client.server_info is None:
        raise ConfigEntryNotReady("Matter server information is not available")

    coordinator = SmartMeterCoordinator(hass, matter_client, node.node_id, endpoint.endpoint_id)
    entry.runtime_data = SmartMeterRuntimeData(
        matter_client=matter_client,
        node_id=node.node_id,
        endpoint_id=endpoint.endpoint_id,
        device_id=entry.data[CONF_DEVICE_ID],
        coordinator=coordinator,
    )
    # An offline meter must not prevent the PIN integration from loading.
    await coordinator.async_refresh()
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)

    # Migrate only the exact legacy default names, including a user-set device
    # name. Other names chosen by the user remain untouched.
    registry = dr.async_get(hass)
    device = registry.async_get_device_by_identifier(
        (DOMAIN, entry.data[CONF_DEVICE_ID]), entry.entry_id
    )
    if device is not None:
        new_user_name = normalize_legacy_name(device.name_by_user)
        if new_user_name != device.name_by_user:
            registry.async_update_device(device.id, name_by_user=new_user_name)
        elif device.name_by_user is None:
            new_name = normalize_legacy_name(device.name)
            if new_name != device.name:
                registry.async_update_device(device.id, name=new_name)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Unload the integration."""
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
