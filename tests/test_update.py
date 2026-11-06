"""HA entity contract tests with fake HA/Matter clients; no live updates."""
import asyncio
from enum import IntFlag
import importlib.util
import json
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import AsyncMock, patch

from test_ota import image_fixture, manifest_fixture, ready_attributes

ROOT = Path(__file__).resolve().parents[1]


class FakeEntity:
    def async_write_ha_state(self):
        pass


class Features(IntFlag):
    INSTALL = 1
    RELEASE_NOTES = 16


class HAError(Exception):
    pass


class MissingNode(Exception):
    pass


def module(name, **members):
    value = types.ModuleType(name)
    value.__dict__.update(members)
    return value


stubs = {
    "aiohttp": module("aiohttp", ClientTimeout=lambda **kwargs: kwargs),
    "matter_server.common.errors": module("errors", NodeNotExists=MissingNode),
    "matter_server.common.models": module("models", EventType=types.SimpleNamespace(ATTRIBUTE_UPDATED=1, NODE_UPDATED=2)),
    "homeassistant.components.update": module("update", UpdateEntity=FakeEntity, UpdateEntityFeature=Features,
                                               UpdateDeviceClass=types.SimpleNamespace(FIRMWARE="firmware")),
    "homeassistant.const": module("const", EntityCategory=types.SimpleNamespace(CONFIG="config")),
    "homeassistant.core": module("core", callback=lambda fn: fn),
    "homeassistant.exceptions": module("exceptions", HomeAssistantError=HAError),
    "homeassistant.helpers.device_registry": module(
        "device_registry", DeviceEntryType=types.SimpleNamespace(SERVICE="service")
    ),
    "homeassistant.helpers.aiohttp_client": module("aiohttp_client", async_get_clientsession=lambda hass: None),
    "test_smartmeter": module("test_smartmeter", __path__=[str(ROOT / "custom_components/smartmeter")]),
}
with patch.dict(sys.modules, stubs):
    spec = importlib.util.spec_from_file_location("test_smartmeter.update", ROOT / "custom_components/smartmeter/update.py")
    update = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = update
    spec.loader.exec_module(update)


def release_response():
    manifest = manifest_fixture()
    return json.dumps({"tag_name": "v1.8", "draft": False, "prerelease": False, "assets": [
        {"browser_download_url": manifest["url"]},
        {"browser_download_url": manifest["url"].replace("SML-over-Matter-Thread.ota", "ota-manifest.json")},
    ]}).encode()


class UpdateTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.attrs = ready_attributes()
        self.node = types.SimpleNamespace(available=True, node_data=types.SimpleNamespace(attributes=self.attrs))
        self.client = types.SimpleNamespace(
            get_node=lambda _: self.node,
            server_info=types.SimpleNamespace(schema_version=13),
            upload_ota_file=AsyncMock(return_value=types.SimpleNamespace(vid=0xFFF1, pid=0x8000, software_version=9)),
            update_node=AsyncMock(),
            read_attribute=AsyncMock(side_effect=self.read_attribute),
            refresh_attribute=AsyncMock(),
        )
        runtime = types.SimpleNamespace(matter_client=self.client, node_id=5, endpoint_id=1, device_id="device")
        self.entity = update.GitHubFirmwareUpdate(runtime)

        async def executor(fn, *args):
            return fn(*args)

        self.entity.hass = types.SimpleNamespace(async_add_executor_job=executor)

    async def test_name_and_icon_distinguish_github_ota(self):
        self.assertEqual(self.entity._attr_name, "OTA Firmware")
        self.assertEqual(self.entity._attr_icon, "mdi:github")
        self.assertIsNone(self.entity.entity_picture)
        self.assertEqual(self.entity._attr_device_info["identifiers"], {("smartmeter", "device")})
        manifest = json.loads((ROOT / "custom_components/smartmeter/manifest.json").read_text())
        self.assertEqual(self.entity._attr_device_info["sw_version"], manifest["version"])

    async def test_polled_version_overrides_stale_node_cache(self):
        self.entity._runtime.coordinator = types.SimpleNamespace(
            data={"0/40/9": 9, "0/40/10": "1.8"}
        )
        self.assertEqual(self.entity.installed_version, "1.8")

    async def read_attribute(self, _node_id, paths):
        if isinstance(paths, list):
            return dict(self.attrs)
        return {"0/40/9": 9}

    def downloads(self, image=None):
        return patch.object(update, "_download", AsyncMock(side_effect=[
            release_response(), json.dumps(manifest_fixture()).encode(), image or image_fixture()]))

    async def test_poll_never_uploads_or_installs(self):
        with self.downloads():
            await self.entity.async_update()
        self.assertEqual(self.entity.latest_version, "1.8")
        self.client.upload_ota_file.assert_not_awaited()
        self.client.update_node.assert_not_awaited()

    async def test_usb_only_release_has_no_install_action(self):
        manifest = {"schema": 1, "delivery": "usb", "vendor_id": 0xFFF1,
                    "product_id": 0x8000, "hardware_version": 1,
                    "software_version": 8, "version_string": "1.7"}
        release = {"tag_name": "v1.7", "draft": False, "prerelease": False,
                   "assets": [{"browser_download_url":
                   "https://github.com/Mosher23/SML-over-Matter-Thread/releases/download/v1.7/ota-manifest.json"}]}
        self.attrs["0/40/9"] = 7
        self.attrs["0/40/10"] = "1.6"
        with patch.object(update, "_download", AsyncMock(side_effect=[
                json.dumps(release).encode(), json.dumps(manifest).encode()])):
            await self.entity.async_update()
        self.assertEqual(self.entity.latest_version, "1.7")
        self.assertEqual(self.entity.supported_features, Features.RELEASE_NOTES)
        self.client.upload_ota_file.assert_not_awaited()

    async def test_shorter_release_label_uses_numeric_matter_update_order(self):
        image = image_fixture(version=24, version_string="1.2", **{"6": 23})
        manifest = dict(manifest_fixture(image), software_version=24,
                        version_string="1.2", max_version=23,
                        url="https://github.com/Mosher23/SML-over-Matter-Thread/releases/download/v1.2/SML-over-Matter-Thread.ota")
        response = json.dumps({"tag_name": "v1.2", "draft": False, "prerelease": False,
                               "assets": [{"browser_download_url": manifest["url"]},
                                          {"browser_download_url": manifest["url"].replace(
                                              "SML-over-Matter-Thread.ota", "ota-manifest.json")}]}).encode()
        self.attrs.update({"0/40/9": 19, "0/40/10": "1.18"})
        with patch.object(update, "_download", AsyncMock(side_effect=[
                response, json.dumps(manifest).encode()])):
            await self.entity.async_update()
        self.assertEqual(self.entity.latest_version, "1.2")
        self.assertTrue(self.entity.version_is_newer("1.2", "1.18"))
        self.attrs.update({"0/40/9": 23, "0/40/10": "1.19-test4"})
        self.assertTrue(self.entity.version_is_newer("1.2", "1.19-test4"))
        for current in (24, 25, None, True):
            self.attrs["0/40/9"] = current
            self.assertFalse(self.entity.version_is_newer("1.2", "1.19-test4"))
        self.entity._release = None
        self.attrs["0/40/9"] = 23
        self.assertFalse(self.entity.version_is_newer("1.2", "1.19-test4"))
        self.client.upload_ota_file.assert_not_awaited()
        self.client.update_node.assert_not_awaited()

    async def test_renamed_release_downloads_existing_manifest_and_new_ota_alias(self):
        manifest = manifest_fixture()
        manifest["url"] = manifest["url"].replace(
            update.REPOSITORY, "Mosher23/IR-Thread-Meter").replace(
                "SML-over-Matter-Thread.ota", "IR_Power_Meter.ota")
        with patch.object(update, "_download", AsyncMock(side_effect=[
                release_response(), json.dumps(manifest).encode()])) as download:
            await self.entity.async_update()
        self.assertIsNone(self.entity.extra_state_attributes["last_error"])
        self.assertIn("Mosher23/SML-over-Matter-Thread/releases/download/v1.8/SML-over-Matter-Thread.ota",
                      self.entity._release.url)
        self.assertIn("/repos/Mosher23/SML-over-Matter-Thread/releases/latest",
                      download.await_args_list[0].args[1])
        self.assertIn("/SML-over-Matter-Thread/releases/download/v1.8/ota-manifest.json",
                      download.await_args_list[1].args[1])
        self.client.upload_ota_file.assert_not_awaited()
        self.client.update_node.assert_not_awaited()

    async def test_empty_repository_is_up_to_date_not_unknown(self):
        with patch.object(update, "_download", AsyncMock(return_value=None)):
            await self.entity.async_update()
        self.assertEqual(self.entity.latest_version, "1.7")

    async def test_http_error_is_not_faked_as_up_to_date(self):
        with patch.object(update, "_download", AsyncMock(side_effect=RuntimeError("rate limit"))):
            await self.entity.async_update()
        self.assertIsNone(self.entity.latest_version)
        self.assertIn("rate limit", self.entity.extra_state_attributes["last_error"])

    async def test_corrupt_image_never_reaches_matter_server(self):
        with self.downloads(image_fixture()[:-1] + b"X"), self.assertRaises(HAError):
            await self.entity.async_install()
        self.client.upload_ota_file.assert_not_awaited()
        self.client.update_node.assert_not_awaited()
        self.assertFalse(self.entity._attr_in_progress)

    async def test_old_firmware_requires_usb(self):
        self.attrs["0/40/9"] = 7
        with self.downloads(), self.assertRaisesRegex(HAError, "USB bootstrap"):
            await self.entity.async_install()
        self.client.upload_ota_file.assert_not_awaited()

    async def test_no_downgrades(self):
        self.attrs["0/40/9"] = 10
        self.attrs["0/40/10"] = "1.9"
        with self.downloads(), self.assertRaisesRegex(HAError, "No compatible newer"):
            await self.entity.async_install()
        self.client.upload_ota_file.assert_not_awaited()

    async def test_offline_blocks_install(self):
        self.node.available = False
        with self.downloads(), self.assertRaisesRegex(HAError, "offline"):
            await self.entity.async_install()
        self.client.upload_ota_file.assert_not_awaited()

    async def test_schema_gate(self):
        self.client.server_info.schema_version = 12
        with self.downloads(), self.assertRaisesRegex(HAError, "schema 13"):
            await self.entity.async_install()
        self.client.upload_ota_file.assert_not_awaited()

    async def test_success_waits_for_reported_version(self):
        with self.downloads(), patch.object(update.asyncio, "sleep", AsyncMock()):
            await self.entity.async_install()
        self.client.upload_ota_file.assert_awaited_once()
        self.client.update_node.assert_awaited_once_with(5, 9)
        self.assertEqual(self.client.read_attribute.await_count, 4)
        self.assertEqual(self.entity._phase, "Installed version verified after reboot")

    async def test_wrong_upload_identity_blocks_transfer(self):
        self.client.upload_ota_file.return_value.software_version = 11
        with self.downloads(), self.assertRaisesRegex(HAError, "unexpected firmware"):
            await self.entity.async_install()
        self.client.update_node.assert_not_awaited()

    async def test_transfer_error_clears_busy_flag(self):
        self.client.update_node.side_effect = RuntimeError("transfer failed")
        with self.downloads(), self.assertRaisesRegex(HAError, "transfer failed"):
            await self.entity.async_install()
        self.assertFalse(self.entity._attr_in_progress)

    async def test_concurrent_install_is_rejected(self):
        async with self.entity._install_lock:
            with self.assertRaisesRegex(HAError, "already in progress"):
                await self.entity.async_install()

    async def test_missing_node_does_not_crash(self):
        self.client.get_node = lambda _: (_ for _ in ()).throw(MissingNode())
        self.assertFalse(self.entity.available)
        self.assertIsNone(self.entity.installed_version)

    async def test_redirect_host_guard(self):
        with self.assertRaisesRegex(ValueError, "trusted GitHub"):
            await update._download(None, "https://example.com/image.ota", 100)
