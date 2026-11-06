"""Offline contract tests for the HA companion's diagnostics and PIN UI."""

import asyncio
import importlib.util
import json
from pathlib import Path
import sys
from time import monotonic
import types
import unittest
from unittest.mock import AsyncMock, patch

ROOT = Path(__file__).resolve().parents[1] / "custom_components/smartmeter"


def module(name, **members):
    result = types.ModuleType(name)
    result.__dict__.update(members)
    return result


class FakeEntity:
    def async_write_ha_state(self):
        pass

    async def async_added_to_hass(self):
        pass

    async def async_will_remove_from_hass(self):
        pass


class FakeCoordinator:
    def __class_getitem__(cls, _item):
        return cls

    def __init__(self, _hass, _logger, *, name, update_interval):
        self.name = name
        self.update_interval = update_interval
        self.data = None


class FakeCoordinatorEntity:
    def __init__(self, coordinator):
        self.coordinator = coordinator


class HAError(Exception):
    pass


class SendKey:
    def __init__(self, *, keyCode):
        self.keyCode = keyCode


package = module("companion_test", __path__=[str(ROOT)], SmartMeterRuntimeData=object)
stubs = {
    "companion_test": package,
    "homeassistant": module("homeassistant", __path__=[]),
    "homeassistant.helpers": module("homeassistant.helpers", __path__=[]),
    "matter_server.common.helpers.util": module(
        "util", create_attribute_path=lambda endpoint, cluster, attribute: f"{endpoint}/{cluster}/{attribute}"
    ),
    "matter_server.common.models": module(
        "models", EventType=types.SimpleNamespace(NODE_UPDATED=1)
    ),
    "chip.clusters": module(
        "clusters", Objects=types.SimpleNamespace(
            KeypadInput=types.SimpleNamespace(Commands=types.SimpleNamespace(SendKey=SendKey))
        )
    ),
    "homeassistant.components.binary_sensor": module("binary_sensor", BinarySensorEntity=FakeEntity),
    "homeassistant.components.button": module("button", ButtonEntity=FakeEntity),
    "homeassistant.components.sensor": module("sensor", SensorEntity=FakeEntity),
    "homeassistant.components.text": module(
        "text", TextEntity=FakeEntity, TextMode=types.SimpleNamespace(PASSWORD="password")
    ),
    "homeassistant.config_entries": module("config_entries", ConfigEntry=object),
    "homeassistant.const": module(
        "const", EntityCategory=types.SimpleNamespace(CONFIG="config", DIAGNOSTIC="diagnostic"),
    ),
    "homeassistant.core": module("core", HomeAssistant=object, callback=lambda fn: fn),
    "homeassistant.exceptions": module("exceptions", HomeAssistantError=HAError),
    "homeassistant.helpers.entity_platform": module(
        "entity_platform", AddEntitiesCallback=object
    ),
    "homeassistant.helpers.device_registry": module(
        "device_registry", DeviceEntryType=types.SimpleNamespace(SERVICE="service")
    ),
    "homeassistant.helpers.entity_registry": module("entity_registry", async_get=lambda _hass: None),
    "homeassistant.helpers.update_coordinator": module(
        "update_coordinator", DataUpdateCoordinator=FakeCoordinator,
        CoordinatorEntity=FakeCoordinatorEntity, UpdateFailed=HAError,
    ),
}


def load(name):
    spec = importlib.util.spec_from_file_location(f"companion_test.{name}", ROOT / f"{name}.py")
    result = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = result
    spec.loader.exec_module(result)
    return result


with patch.dict(sys.modules, stubs):
    const = load("const")
    coordinator = load("coordinator")
    binary_sensor = load("binary_sensor")
    sensor = load("sensor")
    text = load("text")
    button = load("button")


class Runtime:
    def __init__(self, client, meter_coordinator):
        self.matter_client = client
        self.coordinator = meter_coordinator
        self.node_id = 5
        self.endpoint_id = 1
        self.device_id = "meter-device"
        self.pending_pin = None
        self.pin_expires_at = 0
        self.clear_pin_display = None
        self.pin_send_lock = asyncio.Lock()

    def stage_pin(self, pin):
        self.pending_pin = pin
        self.pin_expires_at = monotonic() + 120

    def take_pin(self):
        pin = self.pending_pin if monotonic() < self.pin_expires_at else None
        self.pending_pin = None
        if self.clear_pin_display:
            self.clear_pin_display()
        return pin


class CompanionTests(unittest.IsolatedAsyncioTestCase):
    def test_domain_matches_install_directory(self):
        self.assertEqual(const.DOMAIN, ROOT.name)
        manifest = json.loads((ROOT / "manifest.json").read_text())
        self.assertEqual(manifest["domain"], ROOT.name)
        self.assertEqual(manifest["version"], const.INTEGRATION_VERSION)

    def test_old_default_names_use_project_name_and_preserve_custom_names(self):
        self.assertEqual(const.normalize_legacy_name("IR Smart Meter PIN"), const.NAME)
        self.assertEqual(const.normalize_legacy_name("IR Smart Meter"), const.NAME)
        self.assertEqual(const.normalize_legacy_name(const.NAME), const.NAME)
        self.assertIsNone(const.normalize_legacy_name(None))
        self.assertEqual(const.normalize_legacy_name("Smart Meter PIN"), "Smart Meter")
        self.assertEqual(const.normalize_legacy_name("My meter"), "My meter")

    def setUp(self):
        self.node = types.SimpleNamespace(available=True)
        self.client = types.SimpleNamespace(
            get_node=lambda _: self.node,
            read_attribute=AsyncMock(),
            send_device_command=AsyncMock(),
        )
        self.client.send_device_command.return_value = None
        self.coordinator = coordinator.SmartMeterCoordinator(None, self.client, 5, 1)
        self.runtime = Runtime(self.client, self.coordinator)

    async def test_one_poll_reads_all_diagnostics_without_duplicate_power(self):
        self.client.read_attribute.return_value = {"0/40/9": 12}
        data = await self.coordinator._async_update_data()
        self.assertEqual(data["0/40/9"], 12)
        paths = self.client.read_attribute.await_args_list[0].args[1]
        self.assertEqual(len(paths), 6)
        self.assertNotIn("1/144/8", paths)
        self.assertIn("0/40/9", paths)
        self.assertIn("0/40/10", paths)
        self.assertIn("1/4294048769/3", paths)
        self.assertEqual(self.client.read_attribute.await_args_list[1].args[1], "1/6/0")
        self.assertEqual(self.coordinator.update_interval.total_seconds(), 30)

    async def test_sensor_platform_only_adds_meter_identity(self):
        entities = []
        entry = types.SimpleNamespace(runtime_data=self.runtime, entry_id="companion-entry")
        registry = types.SimpleNamespace(async_get_entity_id=lambda *_: None)
        with patch.object(sensor.er, "async_get", return_value=registry):
            await sensor.async_setup_entry(None, entry, entities.extend)
        self.assertEqual([entity._attr_name for entity in entities],
                         ["Meter ID", "Manufacturer", "Active antenna"] + [field[1] for field in const.RELIABILITY_FIELDS])

    async def test_exactly_seven_diagnostics_are_enabled_by_default(self):
        entities = []
        entry = types.SimpleNamespace(runtime_data=self.runtime, entry_id="companion-entry")
        updates = []
        registry = types.SimpleNamespace(
            async_get_entity_id=lambda *_: None,
            async_update_entity=lambda *args, **kwargs: updates.append((args, kwargs)),
        )
        with patch.object(sensor.er, "async_get", return_value=registry):
            await sensor.async_setup_entry(None, entry, entities.extend)
        await binary_sensor.async_setup_entry(None, entry, entities.extend)
        enabled = {entity._attr_name for entity in entities
                   if entity._attr_entity_registry_enabled_default}
        self.assertEqual(enabled, {
            "Active antenna", "Imported energy OBIS source", "Manufacturer",
            "Meter ID", "Meter ID OBIS source", "OBIS Received", "Power OBIS source",
        })
        self.assertEqual(len(entities), 24)
        self.assertEqual(sum(not entity._attr_entity_registry_enabled_default for entity in entities), 17)
        self.assertEqual(updates, [])  # Existing registry choices are not overwritten.
        self.assertEqual(len({entity._attr_unique_id for entity in entities}), len(entities))
        for entity in entities:
            self.assertEqual(entity._attr_entity_category, "diagnostic")

    async def test_reliability_reads_only_supported_firmware_and_isolates_failures(self):
        self.client.read_attribute.side_effect = [
            {"0/40/9": 25, "0/40/10": "1.21"},
            {"1/4294048769/5": "fresh", "1/4294048769/11": 0},
        ]
        data = await self.coordinator._async_update_data()
        self.assertEqual(data["1/4294048769/5"], "fresh")
        self.assertEqual(len(self.client.read_attribute.await_args_list[1].args[1]), 20)
        self.client.read_attribute.side_effect = [
            {"0/40/9": 25, "1/4294048769/1": "meter123"}, RuntimeError("partial interview"),
        ]
        data = await self.coordinator._async_update_data()
        self.assertEqual(data["1/4294048769/1"], "meter123")

    def test_reliability_age_and_counter_validation(self):
        age = sensor.ReliabilitySensor(self.runtime, 11, "Power reading age", "timer", "age")
        count = sensor.ReliabilitySensor(self.runtime, 15, "SML CRC errors", "alert", "counter")
        self.assertIsNone(age.native_value)
        for invalid in (None, True, -1, "30", 0xFFFFFFFF):
            self.coordinator.data = {age._attribute_path: invalid}
            self.assertIsNone(age.native_value)
        self.coordinator.data = {age._attribute_path: 0, count._attribute_path: 0}
        self.assertEqual(age.native_value, 0)
        self.assertEqual(count.native_value, 0)
        self.coordinator.data[count._attribute_path] = 0xFFFFFFFF
        self.assertEqual(count.native_value, 0xFFFFFFFF)

    async def test_antenna_diagnostic_reports_live_switch_and_legacy_setting(self):
        active = sensor.ActiveAntennaSensor(self.runtime)
        self.assertIsNone(active.native_value)
        self.coordinator.data = {active._diagnostic_path: False}
        self.assertEqual(active.native_value, "Internal")
        self.coordinator.data[active._switch_path] = True
        self.assertEqual(active.native_value, "External")
        self.coordinator.data[active._switch_path] = None
        self.assertIsNone(active.native_value)

    async def test_antenna_switch_read_failure_preserves_other_diagnostics(self):
        self.client.read_attribute.side_effect = [
            {"1/4294048769/3": True, "0/40/9": 12, "0/40/10": "1.11"},
            RuntimeError("On/Off cluster not installed"),
        ]
        data = await self.coordinator._async_update_data()
        self.coordinator.data = data
        self.assertEqual(sensor.ActiveAntennaSensor(self.runtime).native_value, "External")
        self.assertEqual(data["0/40/10"], "1.11")

    async def test_old_firmware_uses_saved_antenna_without_probing_switch(self):
        self.client.read_attribute.return_value = {
            "1/4294048769/3": False, "0/40/9": 11, "0/40/10": "1.10"
        }
        self.coordinator.data = await self.coordinator._async_update_data()
        self.assertEqual(self.client.read_attribute.await_count, 1)
        self.assertEqual(sensor.ActiveAntennaSensor(self.runtime).native_value, "Internal")

    async def test_firmware_1_14_uses_diagnostic_without_reading_removed_switch(self):
        self.client.read_attribute.return_value = {
            "1/4294048769/3": True, "0/40/9": 15, "0/40/10": "1.14"
        }
        self.coordinator.data = await self.coordinator._async_update_data()
        self.assertEqual(self.client.read_attribute.await_count, 1)
        self.assertNotIn("1/6/0", self.coordinator.data)
        self.assertEqual(sensor.ActiveAntennaSensor(self.runtime).native_value, "External")

    def test_default_entity_names_match_home_assistant(self):
        self.assertEqual(text.MeterPinTextEntity._attr_name, "Meter PIN")
        self.assertEqual(button.SendMeterPinButton._attr_name, "Send PIN")
        self.assertEqual(binary_sensor.ActivePowerObisSeenSensor._attr_name, "OBIS Received")

    async def test_manual_pulse_buttons_require_firmware_1_11(self):
        short = button.ManualMeterPulseButton(self.runtime, long_pulse=False)
        long = button.ManualMeterPulseButton(self.runtime, long_pulse=True)
        self.assertEqual(short._attr_name, "Short light pulse")
        self.assertEqual(long._attr_name, "Long light pulse")
        self.assertFalse(short.available)
        with self.assertRaisesRegex(HAError, "firmware 1.11"):
            await short.async_press()
        self.coordinator.data = {"0/40/9": 11}
        self.assertFalse(short.available)
        self.coordinator.data = {"0/40/9": 12}
        self.assertTrue(short.available)
        self.assertTrue(long.available)

    async def test_manual_pulses_use_standard_navigation_keys(self):
        self.coordinator.data = {"0/40/9": 12}
        short = button.ManualMeterPulseButton(self.runtime, long_pulse=False)
        long = button.ManualMeterPulseButton(self.runtime, long_pulse=True)
        await short.async_press()
        await long.async_press()
        codes = [call.kwargs["command"].keyCode for call in self.client.send_device_command.await_args_list]
        self.assertEqual(codes, [0x01, 0x02])

    async def test_button_platform_exposes_pin_and_both_pulses(self):
        entities = []
        entry = types.SimpleNamespace(runtime_data=self.runtime)
        await button.async_setup_entry(None, entry, entities.extend)
        self.assertEqual(
            [entity._attr_name for entity in entities],
            ["Send PIN", "Short light pulse", "Long light pulse"],
        )

    async def test_manual_pulse_rejection_is_reported(self):
        self.coordinator.data = {"0/40/9": 12}
        self.client.send_device_command.return_value = {"status": 2}
        short = button.ManualMeterPulseButton(self.runtime, long_pulse=False)
        with self.assertRaisesRegex(HAError, "transmitter is busy"):
            await short.async_press()

    def test_entity_devices_use_new_domain(self):
        entities = [
            sensor.MeterIdentitySensor(self.runtime, const.METER_ID_ATTRIBUTE_ID, "Meter ID", "identifier"),
            sensor.ActiveAntennaSensor(self.runtime),
            binary_sensor.ActivePowerObisSeenSensor(self.runtime),
            text.MeterPinTextEntity(self.runtime),
            button.SendMeterPinButton(self.runtime),
            button.ManualMeterPulseButton(self.runtime, long_pulse=False),
            button.ManualMeterPulseButton(self.runtime, long_pulse=True),
        ]
        for entity in entities:
            self.assertEqual(entity._attr_device_info["identifiers"], {("smartmeter", "meter-device")})
            self.assertEqual(entity._attr_device_info["manufacturer"], "Mosher23")
            self.assertEqual(entity._attr_device_info["model"], "Home Assistant companion integration")
            self.assertEqual(entity._attr_device_info["sw_version"], const.INTEGRATION_VERSION)
            self.assertEqual(entity._attr_device_info["entry_type"], "service")
            self.assertFalse(entity._attr_device_info.get("hw_version"))
            self.assertEqual(entity._attr_device_info["configuration_url"], const.PROJECT_URL)

    async def test_removes_only_its_legacy_power_entity(self):
        removed = []
        entry = types.SimpleNamespace(runtime_data=self.runtime, entry_id="companion-entry")
        registry = types.SimpleNamespace(
            async_get_entity_id=lambda *_: "sensor.legacy_power",
            async_get=lambda _entity_id: types.SimpleNamespace(config_entry_id="other-entry"),
            async_remove=removed.append,
        )
        with patch.object(sensor.er, "async_get", return_value=registry):
            await sensor.async_setup_entry(None, entry, lambda _: None)
            self.assertEqual(removed, [])
            registry.async_get = lambda _entity_id: types.SimpleNamespace(config_entry_id=entry.entry_id)
            await sensor.async_setup_entry(None, entry, lambda _: None)
        self.assertEqual(removed, ["sensor.legacy_power"])

    async def test_pin_requires_explicit_button_and_never_enters_state(self):
        field = text.MeterPinTextEntity(self.runtime)
        send = button.SendMeterPinButton(self.runtime)
        await field.async_set_value("1234")
        self.assertEqual(field._attr_native_value, "••••")
        self.client.send_device_command.assert_not_awaited()
        await send.async_press()
        codes = [call.kwargs["command"].keyCode for call in self.client.send_device_command.await_args_list]
        self.assertEqual(codes, [0x2C, 0x21, 0x22, 0x23, 0x24, 0x00])
        self.assertEqual(field._attr_native_value, "")
        self.assertIsNone(self.runtime.pending_pin)

    async def test_button_without_pin_does_not_send(self):
        send = button.SendMeterPinButton(self.runtime)
        with self.assertRaisesRegex(HAError, "Enter a four-digit PIN"):
            await send.async_press()
        self.client.send_device_command.assert_not_awaited()

    async def test_button_reports_firmware_rejection(self):
        field = text.MeterPinTextEntity(self.runtime)
        send = button.SendMeterPinButton(self.runtime)
        await field.async_set_value("1234")
        self.client.send_device_command.side_effect = [
            {"status": 0}, {"status": 0}, {"status": 0},
            {"status": 0}, {"status": 0}, {"status": 2}, {"status": 0},
        ]
        with self.assertRaisesRegex(HAError, "transmitter is busy"):
            await send.async_press()
        self.assertEqual(self.client.send_device_command.await_count, 7)

    async def test_offline_meter_does_not_expose_stale_values(self):
        self.node.available = False
        with self.assertRaisesRegex(HAError, "offline"):
            await self.coordinator._async_update_data()
        self.client.read_attribute.assert_not_awaited()
