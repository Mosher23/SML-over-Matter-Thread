"""Fetch meter diagnostics and software version in one bounded Matter read."""

from __future__ import annotations

import asyncio
from datetime import timedelta
import logging

from matter_server.common.helpers.util import create_attribute_path

from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .const import (
    ACTIVE_POWER_OBIS_SEEN_ATTRIBUTE_ID,
    DIAGNOSTICS_CLUSTER_ID,
    EXTERNAL_ANTENNA_ATTRIBUTE_ID,
    METER_ID_ATTRIBUTE_ID,
    METER_MANUFACTURER_ATTRIBUTE_ID,
    ON_OFF_ATTRIBUTE_ID,
    ON_OFF_CLUSTER_ID,
    RELIABILITY_FIELDS,
)

_LOGGER = logging.getLogger(__name__)

POLL_INTERVAL = timedelta(seconds=30)


class SmartMeterCoordinator(DataUpdateCoordinator[dict[str, object]]):
    """Refresh all companion readings together instead of waiting minutes."""

    def __init__(self, hass, matter_client, node_id: int, endpoint_id: int) -> None:
        super().__init__(
            hass,
            _LOGGER,
            name=f"SML over Matter/Thread {node_id}",
            update_interval=POLL_INTERVAL,
        )
        self.matter_client = matter_client
        self.node_id = node_id
        self.paths = [
            create_attribute_path(endpoint_id, DIAGNOSTICS_CLUSTER_ID, attribute_id)
            for attribute_id in (
                ACTIVE_POWER_OBIS_SEEN_ATTRIBUTE_ID,
                METER_ID_ATTRIBUTE_ID,
                METER_MANUFACTURER_ATTRIBUTE_ID,
                EXTERNAL_ANTENNA_ATTRIBUTE_ID,
            )
        ]
        self.reliability_paths = [
            create_attribute_path(endpoint_id, DIAGNOSTICS_CLUSTER_ID, field[0])
            for field in RELIABILITY_FIELDS
        ]
        # Basic Information software version and string, for prompt HA update
        # entity refresh after an OTA reboot rather than a multi-minute wait.
        self.paths.extend(("0/40/9", "0/40/10"))
        self.antenna_switch_path = create_attribute_path(
            endpoint_id, ON_OFF_CLUSTER_ID, ON_OFF_ATTRIBUTE_ID
        )

    async def _async_update_data(self) -> dict[str, object]:
        try:
            node = self.matter_client.get_node(self.node_id)
            if not node.available:
                raise UpdateFailed("The Matter meter is offline")
            values = await asyncio.wait_for(
                self.matter_client.read_attribute(self.node_id, self.paths), timeout=15
            )
            # Firmware before the antenna switch has no On/Off cluster. Keep
            # every existing diagnostic available during a staged OTA upgrade.
            # 1.14 (software version 15) removes the endpoint-1 switch.
            # Its read-only diagnostic remains authoritative, so do not probe
            # the removed attribute or overwrite it with an unsupported/null value.
            if isinstance(values.get("0/40/9"), int) and 12 <= values["0/40/9"] < 15:
                try:
                    values.update(
                        await asyncio.wait_for(
                            self.matter_client.read_attribute(
                                self.node_id, self.antenna_switch_path
                            ),
                            timeout=5,
                        )
                    )
                except Exception:
                    _LOGGER.debug("Matter antenna switch is not readable on node %s", self.node_id)
            # Keep legacy reads working on firmware without the new attributes.
            # Isolate this optional read so a partial upgrade cannot hide identity.
            if type(values.get("0/40/9")) is int and values["0/40/9"] >= 25:
                try:
                    values.update(await asyncio.wait_for(
                        self.matter_client.read_attribute(self.node_id, self.reliability_paths),
                        timeout=10,
                    ))
                except Exception:
                    _LOGGER.debug("Reliability diagnostics unavailable on node %s", self.node_id)
            return values
        except UpdateFailed:
            raise
        except Exception as err:
            raise UpdateFailed(f"Could not read meter attributes: {err}") from err
