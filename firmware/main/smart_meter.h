#pragma once

#include <esp_err.h>
#include <stdint.h>

class SmartMeterPowerDelegate;

// Vendor-specific Matter diagnostics. The custom Home Assistant integration
// uses this read-only Boolean attribute to distinguish "no active-power OBIS
// has arrived" from an actual zero-watt reading.
constexpr uint32_t kSmartMeterDiagnosticsClusterId = 0xFFF1FC01;
constexpr uint32_t kSmartMeterActivePowerObisSeenAttributeId = 0x00000000;
constexpr uint32_t kSmartMeterIdAttributeId = 0x00000001;
constexpr uint32_t kSmartMeterManufacturerAttributeId = 0x00000002;
constexpr uint32_t kSmartMeterExternalAntennaAttributeId = 0x00000003;
constexpr uint32_t kSmartMeterOtaReadyAttributeId = 0x00000004;
constexpr uint16_t kSmartMeterIdentityMaxLength = 64;

esp_err_t smart_meter_start(uint16_t matter_endpoint_id, SmartMeterPowerDelegate *power_delegate);
esp_err_t smart_meter_submit_pin(const char *pin);
esp_err_t smart_meter_submit_pulse(bool long_pulse);
esp_err_t smart_meter_register_console_commands();

// Blink the status LED three times and wait for completion (task context only).
esp_err_t smart_meter_confirm_antenna_toggle();

// v1.21 read-only reliability diagnostics; existing IDs above are unchanged.
constexpr uint32_t kSmartMeterPowerStatus = 5;
constexpr uint32_t kSmartMeterImportStatus = 6;
constexpr uint32_t kSmartMeterExportStatus = 7;
constexpr uint32_t kSmartMeterProfile = 8;
constexpr uint32_t kSmartMeterDetection = 9;
constexpr uint32_t kSmartMeterPowerSource = 10;
constexpr uint32_t kSmartMeterPowerAge = 11;
constexpr uint32_t kSmartMeterImportAge = 12;
constexpr uint32_t kSmartMeterExportAge = 13;
constexpr uint32_t kSmartMeterValidFrames = 14;
constexpr uint32_t kSmartMeterCrcErrors = 15;
constexpr uint32_t kSmartMeterDecodeErrors = 16;
constexpr uint32_t kSmartMeterRejectedReadings = 17;
constexpr uint32_t kSmartMeterLastError = 18;
constexpr uint32_t kSmartMeterReaderHealth = 19;
constexpr uint32_t kSmartMeterFrameAge = 20;
constexpr uint32_t kSmartMeterUartOverflows = 21;
constexpr uint32_t kSmartMeterIdSource = 22;
constexpr uint32_t kSmartMeterImportSource = 23;
constexpr uint32_t kSmartMeterExportSource = 24;
