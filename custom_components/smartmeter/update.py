"""Opt-in installation of validated GitHub firmware through Matter Server."""

from __future__ import annotations

import asyncio
from datetime import timedelta
import json
import logging
from urllib.parse import urljoin, urlsplit

from aiohttp import ClientTimeout
from matter_server.common.errors import NodeNotExists
from matter_server.common.models import EventType

from homeassistant.components.update import UpdateDeviceClass, UpdateEntity, UpdateEntityFeature
from homeassistant.const import EntityCategory
from homeassistant.core import callback
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .const import INTEGRATION_VERSION, companion_device_info
from .ota import REPOSITORY, MAX_JSON, compatibility, parse_manifest, validate_image

SCAN_INTERVAL = timedelta(hours=6)
_LOGGER = logging.getLogger(__name__)
_DOWNLOAD_HOSTS = {"api.github.com", "github.com", "release-assets.githubusercontent.com",
                   "objects.githubusercontent.com"}


async def _download(session, url: str, limit: int, *, missing_ok=False):
    """Bound memory use, timeout and GitHub redirects. No tokens needed for public releases."""
    for _ in range(6):
        parsed = urlsplit(url)
        if parsed.scheme != "https" or parsed.hostname not in _DOWNLOAD_HOSTS or parsed.port not in (None, 443) or parsed.username:
            raise ValueError("Release download redirected outside trusted GitHub hosts")
        async with session.get(url, allow_redirects=False,
                               headers={"User-Agent": f"SML-over-Matter-Thread-HA/{INTEGRATION_VERSION}"},
                               timeout=ClientTimeout(total=120)) as response:
            if response.status in (301, 302, 303, 307, 308):
                url = urljoin(url, response.headers["Location"])
                continue
            if missing_ok and response.status == 404:
                return None
            response.raise_for_status()
            if response.content_length is not None and response.content_length > limit:
                raise ValueError("Release download exceeds its size limit")
            data = bytearray()
            async for chunk in response.content.iter_chunked(65536):
                data.extend(chunk)
                if len(data) > limit:
                    raise ValueError("Release download exceeds its size limit")
            return bytes(data)
    raise ValueError("Too many release redirects")


async def async_setup_entry(hass, entry, async_add_entities):
    async_add_entities([GitHubFirmwareUpdate(entry.runtime_data)], update_before_add=True)


class GitHubFirmwareUpdate(UpdateEntity):
    """Poll metadata only; upload/install solely after the user presses Install."""

    _attr_has_entity_name = True
    _attr_name = "OTA Firmware"
    _attr_icon = "mdi:github"
    _attr_device_class = UpdateDeviceClass.FIRMWARE
    _attr_entity_category = EntityCategory.CONFIG
    _attr_supported_features = UpdateEntityFeature.INSTALL | UpdateEntityFeature.RELEASE_NOTES
    _attr_should_poll = True
    _attr_in_progress = False

    @property
    def entity_picture(self) -> None:
        """Use the GitHub MDI icon instead of HA's missing integration logo."""
        return None

    def __init__(self, runtime):
        self._runtime = runtime
        self._release = None
        self._error = None
        self._phase = "Not checked"
        self._install_lock = asyncio.Lock()
        self._attr_unique_id = f"{runtime.node_id}-github-firmware"
        self._attr_device_info = companion_device_info(runtime.device_id)

    def _node(self):
        try:
            return self._runtime.matter_client.get_node(self._runtime.node_id)
        except NodeNotExists:
            return None

    @property
    def supported_features(self):
        # A USB-only migration must never present an Install OTA action.
        if self._release is not None and self._release.usb_only:
            return UpdateEntityFeature.RELEASE_NOTES
        return self._attr_supported_features

    def _attributes(self):
        node = self._node()
        attrs = node.node_data.attributes if node else {}
        # The shared 30-second read is fresher than Matter Server's periodic
        # node-cache report immediately after a firmware change.
        data = getattr(getattr(self._runtime, "coordinator", None), "data", None)
        return {**attrs, **data} if data else attrs

    @property
    def available(self):
        node = self._node()
        return bool(node and node.available)

    @property
    def installed_version(self):
        value = self._attributes().get("0/40/10")
        return value if isinstance(value, str) else None

    @property
    def latest_version(self):
        current = self._attributes().get("0/40/9")
        if self._release and type(current) is int and self._release.version > current:
            # Matter orders numeric versions, not human-readable strings.
            value = self._release.version_string
            return f"{value} ({self._release.version})" if value == self.installed_version else value
        return self.installed_version if self._error is None else None

    def version_is_newer(self, latest_version: str, installed_version: str) -> bool:
        """Follow Matter's numeric update order, independent of release labels."""
        current = self._attributes().get("0/40/9")
        return bool(self._release and type(current) is int and self._release.version > current)

    @property
    def release_url(self):
        return self._release.notes_url if self._release else None

    @property
    def extra_state_attributes(self):
        attrs = self._attributes()
        return {
            "repository": REPOSITORY,
            "ota_status": self._phase,
            "compatibility_issue": compatibility(self._runtime.matter_client, attrs, self._runtime.endpoint_id),
            "last_error": self._error,
            "installed_numeric_version": attrs.get("0/40/9"),
            "target_numeric_version": self._release.version if self._release else None,
            "ota_download_percent": attrs.get("0/42/3"),
        }

    async def async_release_notes(self):
        return f"Release notes: {self.release_url}" if self.release_url else "No published firmware release."

    async def async_update(self):
        """Discover stable releases; never upload or install during polling."""
        if self._install_lock.locked():
            return
        try:
            session = async_get_clientsession(self.hass)
            raw = await _download(session, f"https://api.github.com/repos/{REPOSITORY}/releases/latest",
                                  MAX_JSON, missing_ok=True)
            self._release = None
            if raw is None:
                self._phase = "No published firmware release"
            else:
                release = json.loads(raw)
                if release.get("draft") or release.get("prerelease"):
                    raise ValueError("Only stable published releases are supported")
                tag = release["tag_name"]
                urls = {asset["browser_download_url"] for asset in release["assets"]}
                manifest_url = f"https://github.com/{REPOSITORY}/releases/download/{tag}/ota-manifest.json"
                if manifest_url not in urls:
                    raise ValueError("Latest release is missing ota-manifest.json")
                manifest = json.loads(await _download(session, manifest_url, MAX_JSON))
                self._release = parse_manifest(manifest, tag, urls)
                self._phase = "USB bootstrap release; see release instructions" if self._release.usb_only else "Release checked"
            self._error = None
        except Exception as err:
            # Do not break PIN/diagnostic setup on GitHub outages or an old client.
            self._release = None
            self._error = str(err)
            self._phase = "Release check failed"
            _LOGGER.warning("IR meter release check failed: %s", err)

    async def async_install(self, version=None, backup=False, **kwargs):
        if version is not None or backup:
            raise HomeAssistantError("Only the latest stable release is supported; firmware cannot back up meter data")
        if self._install_lock.locked():
            raise HomeAssistantError("A firmware installation is already in progress")
        # Re-fetch immediately before installing; never act on stale release metadata.
        await self.async_update()
        if self._install_lock.locked():
            raise HomeAssistantError("A firmware installation is already in progress")
        async with self._install_lock:
            release = self._release
            client = self._runtime.matter_client
            if not self.available:
                raise HomeAssistantError("The meter is offline")
            # Matter Server might not keep unknown vendor attributes in its
            # node cache. Read readiness and identity from the actual device.
            paths = ["0/40/2", "0/40/4", "0/40/7", "0/40/9", f"{self._runtime.endpoint_id}/{0xFFF1FC01}/4"]
            try:
                attrs = dict(self._attributes())
                attrs.update(await client.read_attribute(self._runtime.node_id, paths))
            except Exception as err:
                raise HomeAssistantError("Unable to read OTA readiness from the meter") from err
            reason = compatibility(client, attrs, self._runtime.endpoint_id)
            if reason:
                raise HomeAssistantError(reason)
            current = attrs["0/40/9"]
            if release is None or release.usb_only or not release.minimum <= current <= release.maximum or release.version <= current:
                raise HomeAssistantError(self._error or "No compatible newer firmware release is available")
            self._attr_in_progress = True
            self._error = None
            try:
                self._phase = "Downloading and validating GitHub image"
                self.async_write_ha_state()
                image = await _download(async_get_clientsession(self.hass), release.url, release.size)
                await self.hass.async_add_executor_job(validate_image, image, release)
                # Check again after the download: the node may have been updated elsewhere.
                if not self.available:
                    raise HomeAssistantError("Meter went offline during download")
                attrs.update(await client.read_attribute(self._runtime.node_id, paths))
                if attrs.get("0/40/9") != current or compatibility(client, attrs, self._runtime.endpoint_id):
                    raise HomeAssistantError("Meter state changed during download; check it before retrying")
                self._phase = "Uploading validated image to Matter Server"
                self.async_write_ha_state()
                async with asyncio.timeout(180):
                    result = await client.upload_ota_file(image)
                if (result.vid, result.pid, result.software_version) != (0xFFF1, 0x8000, release.version):
                    raise HomeAssistantError("Matter Server returned an unexpected firmware identity")
                self._phase = "Transferring over Matter; waiting for reboot"
                self.async_write_ha_state()
                async with asyncio.timeout(1800):
                    await client.update_node(self._runtime.node_id, release.version)
                    # A successful command is not proof that the new image booted.
                    while True:
                        await asyncio.sleep(10)
                        if not self.available:
                            continue
                        try:
                            values = await client.read_attribute(self._runtime.node_id, "0/40/9")
                        except Exception:
                            continue  # reconnect can lag the availability event
                        if values.get("0/40/9") == release.version:
                            # Allow the firmware's 30-second rollback health check to finish.
                            await asyncio.sleep(40)
                            values = await client.read_attribute(self._runtime.node_id, "0/40/9")
                            if values.get("0/40/9") == release.version:
                                break
                    await client.refresh_attribute(self._runtime.node_id, "0/40/10")
                    await client.refresh_attribute(self._runtime.node_id, "0/40/9")
                self._phase = "Installed version verified after reboot"
            except asyncio.CancelledError:
                self._phase = "Monitoring interrupted; the server may still be updating"
                raise
            except Exception as err:
                self._error = str(err) or type(err).__name__
                self._phase = "Installation not confirmed; check device before retrying"
                raise HomeAssistantError(f"OTA installation not confirmed: {self._error}") from err
            finally:
                self._attr_in_progress = False
                self.async_write_ha_state()

    async def async_added_to_hass(self):
        await super().async_added_to_hass()
        coordinator = getattr(self._runtime, "coordinator", None)
        if coordinator is not None:
            self.async_on_remove(coordinator.async_add_listener(self._handle_update))
        for event in (EventType.ATTRIBUTE_UPDATED, EventType.NODE_UPDATED):
            self.async_on_remove(self._runtime.matter_client.subscribe_events(
                self._handle_update, event_filter=event, node_filter=self._runtime.node_id))

    @callback
    def _handle_update(self, event=None, data=None):
        self.async_write_ha_state()
