"""Configuration flow for the SML Meter integration."""

from __future__ import annotations

import voluptuous as vol

from chip.clusters import Objects as Clusters

from homeassistant.components.matter.helpers import node_from_ha_device_id
from homeassistant import config_entries
from homeassistant.const import CONF_DEVICE_ID
from homeassistant.core import callback
from homeassistant.helpers import selector

from .const import DOMAIN, NAME


class ConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    """Configure a PIN field for an existing Matter smart meter."""

    VERSION = 1

    async def async_step_user(self, user_input=None):
        """Select the Matter device that owns the IR head."""
        errors = {}
        if user_input is not None:
            device_id = user_input[CONF_DEVICE_ID]
            node = node_from_ha_device_id(self.hass, device_id)
            if node is None or not any(
                endpoint.has_cluster(Clusters.KeypadInput)
                for endpoint in node.endpoints.values()
            ):
                errors["base"] = "not_supported"
            else:
                await self.async_set_unique_id(f"smartmeter-{node.node_id}")
                self._abort_if_unique_id_configured()
                return self.async_create_entry(title=NAME, data=user_input)

        return self.async_show_form(
            step_id="user",
            data_schema=vol.Schema(
                {
                    vol.Required(CONF_DEVICE_ID): selector.DeviceSelector(
                        selector.DeviceSelectorConfig(integration="matter")
                    )
                }
            ),
            errors=errors,
        )
