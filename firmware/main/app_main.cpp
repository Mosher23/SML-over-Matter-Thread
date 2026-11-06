#include "app_openthread_config.h"
#include "electrical_meter_delegate.h"
#include "meter_pin_delegate.h"
#include "smart_meter.h"
#include "board_config.h"
#include "boot_button.h"

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <app/data-model/List.h>
#include <data_model_provider/clusters/electrical_energy_measurement/integration.h>
#include <driver/gpio.h>
#include <esp_console.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_matter.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <platform/CHIPDeviceLayer.h>
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cstring>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;
using namespace esp_matter;

namespace {
constexpr char TAG[] = "matter_smartmeter";

SmartMeterPowerDelegate g_power_delegate;
MeterPinDelegate g_pin_delegate;
uint16_t g_meter_endpoint_id = 0;
bool g_external_antenna = false;
bool g_antenna_controls_ready = false;
std::atomic<bool> g_energy_initialized{false};

void publish_antenna_diagnostic(intptr_t)
{
    // Preserve v1.12's meter On/Off cluster for the native HA antenna entity.
    // BOOT/USB recovery and Matter commands share the saved board setting.
    attribute_t *on_off = attribute::get(g_meter_endpoint_id, OnOff::Id,
                                        OnOff::Attributes::OnOff::Id);
    esp_matter_attr_val_t current = esp_matter_invalid(nullptr);
    if (on_off == nullptr || attribute::get_val(on_off, &current) != ESP_OK) {
        ESP_LOGE(TAG, "Could not read meter antenna state");
    } else if (current.val.b != g_external_antenna) {
        esp_matter_attr_val_t state = esp_matter_bool(g_external_antenna);
        const esp_err_t error = attribute::update(
            g_meter_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &state);
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Could not synchronize meter antenna state: %s",
                     esp_err_to_name(error));
        }
    }
    esp_matter_attr_val_t value = esp_matter_bool(g_external_antenna);
    const esp_err_t error = attribute::update(
        g_meter_endpoint_id, kSmartMeterDiagnosticsClusterId,
        kSmartMeterExternalAntennaAttributeId, &value);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not update antenna diagnostic: %s", esp_err_to_name(error));
    }
}

void initialize_antenna_controls(intptr_t)
{
    // Matter may restore old OnOff/StartUpOnOff values during cluster startup.
    // board_cfg (including USB/BOOT recovery choices) must remain authoritative.
    publish_antenna_diagnostic(0);
    g_antenna_controls_ready = true;
}

esp_err_t attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                              uint32_t cluster_id, uint32_t attribute_id,
                              esp_matter_attr_val_t *value, void *)
{
    if (type != attribute::PRE_UPDATE ||
        endpoint_id != g_meter_endpoint_id ||
        cluster_id != OnOff::Id || attribute_id != OnOff::Attributes::OnOff::Id) {
        return ESP_OK;
    }
    if (value == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_antenna_controls_ready) {
        return ESP_OK;
    }

    const bool external = value->val.b;
    if (external == g_external_antenna) {
        return ESP_OK;
    }
    // Keep the independent board setting authoritative across OTA and Matter
    // factory resets. Reject the Matter command if the change cannot be saved.
    esp_err_t error = board_config_set_antenna(external);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not save antenna selection: %s", esp_err_to_name(error));
        return error;
    }
    error = gpio_set_level(
        static_cast<gpio_num_t>(CONFIG_SMARTMETER_RF_SWITCH_SELECT_GPIO), external ? 1 : 0);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not change antenna: %s", esp_err_to_name(error));
        const esp_err_t rollback = board_config_set_antenna(g_external_antenna);
        if (rollback != ESP_OK) {
            ESP_LOGE(TAG, "Could not restore previous antenna setting: %s",
                     esp_err_to_name(rollback));
        }
        return error;
    }
    g_external_antenna = external;
    ESP_LOGW(TAG, "RF antenna switched to %s", external ? "external" : "internal");
    const CHIP_ERROR schedule_error = DeviceLayer::PlatformMgr().ScheduleWork(
        publish_antenna_diagnostic, 0);
    if (schedule_error != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Could not schedule antenna diagnostic: %" CHIP_ERROR_FORMAT,
                 schedule_error.Format());
    }
    return ESP_OK;
}

esp_err_t identification_cb(identification::callback_type_t type, uint16_t endpoint_id,
                            uint8_t effect_id, uint8_t effect_variant, void *)
{
    ESP_LOGI(TAG, "Identify endpoint=%u type=%u effect=%u variant=%u", endpoint_id,
             static_cast<unsigned>(type), effect_id, effect_variant);
    return ESP_OK;
}

void open_commissioning_window_if_needed()
{
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() != 0) {
        return;
    }

    chip::CommissioningWindowManager &manager =
        chip::Server::GetInstance().GetCommissioningWindowManager();
    if (manager.IsCommissioningWindowOpen()) {
        return;
    }

    CHIP_ERROR error = manager.OpenBasicCommissioningWindow(
        chip::System::Clock::Seconds16(300),
        chip::CommissioningWindowAdvertisement::kDnssdOnly);
    if (error != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to open commissioning window: %" CHIP_ERROR_FORMAT,
                 error.Format());
    }
}

void configure_rf_antenna()
{
    const gpio_num_t enable_gpio =
        static_cast<gpio_num_t>(CONFIG_SMARTMETER_RF_SWITCH_ENABLE_GPIO);
    const gpio_num_t select_gpio =
        static_cast<gpio_num_t>(CONFIG_SMARTMETER_RF_SWITCH_SELECT_GPIO);

    ESP_ERROR_CHECK(gpio_reset_pin(enable_gpio));
    ESP_ERROR_CHECK(gpio_set_direction(enable_gpio, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(enable_gpio, 0));

    // Seeed specifies enabling RF-switch control on GPIO3 before selecting
    // the antenna with GPIO14.
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_ERROR_CHECK(gpio_reset_pin(select_gpio));
    ESP_ERROR_CHECK(gpio_set_direction(select_gpio, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(select_gpio, g_external_antenna ? 1 : 0));
    ESP_LOGI(TAG, "Saved RF antenna: %s", g_external_antenna ? "external" : "internal");
}

int antenna_console_command(int argc, char **argv)
{
    if (argc != 2 || (std::strcmp(argv[1], "internal") != 0 &&
                      std::strcmp(argv[1], "external") != 0)) {
        std::printf("Usage: antenna internal|external\n");
        return 1;
    }
    const bool external = std::strcmp(argv[1], "external") == 0;
    const esp_err_t error = board_config_set_antenna(external);
    if (error != ESP_OK) {
        std::printf("Could not save antenna selection: %s\n", esp_err_to_name(error));
        return 1;
    }
    std::printf("Saved %s antenna; restarting.\n", external ? "external" : "internal");
    std::fflush(stdout);
    esp_restart();
    return 0;
}

void matter_event_cb(const ChipDeviceEvent *event, intptr_t)
{
    switch (event->Type) {
    case DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Matter commissioning complete");
        break;
    case DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGW(TAG, "Matter commissioning failed: fail-safe timer expired");
        break;
    case DeviceLayer::DeviceEventType::kFabricRemoved:
        ESP_LOGI(TAG, "Matter fabric removed");
        open_commissioning_window_if_needed();
        break;
    case DeviceLayer::DeviceEventType::kBLEDeinitialized:
        ESP_LOGI(TAG, "BLE commissioning memory reclaimed");
        break;
    default:
        break;
    }
}

void initialize_energy_cluster(intptr_t argument)
{
    const EndpointId endpoint_id = static_cast<EndpointId>(argument);

    static const ElectricalEnergyMeasurement::Structs::MeasurementAccuracyRangeStruct::Type
        accuracy_ranges[] = {
            {
                .rangeMin = 0,
                .rangeMax = 1'000'000'000'000'000LL,
                .percentMax = MakeOptional(static_cast<Percent100ths>(100)),
                .percentMin = NullOptional,
                .percentTypical = NullOptional,
                .fixedMax = NullOptional,
                .fixedMin = NullOptional,
                .fixedTypical = NullOptional,
            },
        };

    ElectricalEnergyMeasurement::Structs::MeasurementAccuracyStruct::Type accuracy = {
        .measurementType = ElectricalEnergyMeasurement::MeasurementTypeEnum::kElectricalEnergy,
        .measured = true,
        .minMeasuredValue = 0,
        .maxMeasuredValue = 1'000'000'000'000'000LL,
        .accuracyRanges = DataModel::List<const ElectricalEnergyMeasurement::Structs::MeasurementAccuracyRangeStruct::Type>(accuracy_ranges),
    };

    CHIP_ERROR error = ElectricalEnergyMeasurement::SetMeasurementAccuracy(endpoint_id, accuracy);
    if (error != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to initialize energy accuracy: %" CHIP_ERROR_FORMAT,
                 error.Format());
    } else {
        g_energy_initialized.store(true);
    }
}

void ota_health_task(void *)
{
    // Startup must survive 30 seconds with Matter and the SML task started.
    // A missing meter or temporarily unavailable Thread network is NOT a fault.
    vTaskDelay(pdMS_TO_TICKS(30000));
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (!g_energy_initialized.load()) {
            ESP_LOGE(TAG, "OTA startup health check failed; rolling back");
            ESP_ERROR_CHECK(esp_ota_mark_app_invalid_rollback_and_reboot());
        }
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
        ESP_LOGI(TAG, "OTA startup health check passed");
    }
    vTaskDelete(nullptr);
}

void toggle_antenna_and_restart(const char *reason)
{
    const bool next_external = !g_external_antenna;
    ESP_LOGW(TAG, "%s: selecting %s antenna", reason,
             next_external ? "external" : "internal");
    const esp_err_t error = board_config_set_antenna(next_external);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not save antenna selection: %s", esp_err_to_name(error));
        return;
    }
    // Confirm only after persistence succeeds, before rebooting. The LED task
    // owns PWM and overrides normal breathing/receive pulses for this pattern.
    const esp_err_t indication = smart_meter_confirm_antenna_toggle();
    if (indication != ESP_OK) {
        ESP_LOGW(TAG, "Antenna saved but LED confirmation failed: %s",
                 esp_err_to_name(indication));
    }
    // Restart applies the saved RF selection and rejoins Thread without
    // erasing Matter pairings or the per-board commissioning identity.
    esp_restart();
}

void factory_reset_button_task(void *)
{
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << CONFIG_SMARTMETER_FACTORY_RESET_GPIO;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&config));

    BootButtonGestures gestures;
    while (true) {
        const uint32_t now_ms = static_cast<uint32_t>(
            (static_cast<uint64_t>(xTaskGetTickCount()) * 1000) / configTICK_RATE_HZ);
        const bool pressed = gpio_get_level(
                                 static_cast<gpio_num_t>(CONFIG_SMARTMETER_FACTORY_RESET_GPIO)) == 0;
        switch (gestures.Update(pressed, now_ms)) {
        case BootButtonGestures::Action::ToggleAntenna:
            toggle_antenna_and_restart("BOOT antenna gesture");
            break;
        case BootButtonGestures::Action::FactoryReset:
            ESP_LOGW(TAG, "Ten-second BOOT hold: factory reset requested");
            esp_matter::factory_reset();
            vTaskDelete(nullptr);
            break;
        case BootButtonGestures::Action::None:
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
} // namespace

extern "C" void app_main()
{
    g_external_antenna = board_config_init();
    configure_rf_antenna();

    esp_err_t error = nvs_flash_init();
    // Do not erase pairings automatically on a failed NVS migration.
    ESP_ERROR_CHECK(error);

    node::config_t node_config;
    std::snprintf(node_config.root_node.basic_information.node_label,
                  sizeof(node_config.root_node.basic_information.node_label),
                  "%s", CHIP_DEVICE_CONFIG_DEVICE_NAME);
    node_t *node = node::create(&node_config, attribute_update_cb, identification_cb);
    if (node == nullptr) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        abort();
    }

    // SerialNumber is optional in Matter Basic Information. Enable the
    // attribute so the configured device-instance serial is exposed to
    // commissioners and controllers.
    cluster_t *basic_information_cluster =
        cluster::get(static_cast<uint16_t>(0), BasicInformation::Id);
    if (basic_information_cluster == nullptr ||
        cluster::basic_information::attribute::create_serial_number(
            basic_information_cluster, nullptr, 0) == nullptr) {
        ESP_LOGE(TAG, "Failed to enable Basic Information serial number");
        abort();
    }

    endpoint::electrical_meter::config_t meter_config;
    meter_config.electrical_power_measurement.delegate = &g_power_delegate;
    meter_config.electrical_power_measurement.feature_flags =
        cluster::electrical_power_measurement::feature::alternating_current::get_id();
    meter_config.electrical_energy_measurement.feature_flags =
        cluster::electrical_energy_measurement::feature::imported_energy::get_id() |
        cluster::electrical_energy_measurement::feature::exported_energy::get_id() |
        cluster::electrical_energy_measurement::feature::cumulative_energy::get_id();

    endpoint_t *meter_endpoint =
        endpoint::electrical_meter::create(node, &meter_config, ENDPOINT_FLAG_NONE, nullptr);
    if (meter_endpoint == nullptr) {
        ESP_LOGE(TAG, "Failed to create Matter Electrical Meter endpoint");
        abort();
    }
    g_meter_endpoint_id = endpoint::get_id(meter_endpoint);

    // Read-only vendor diagnostics remain separate from the standard Matter
    // On/Off antenna control. Unknown controllers safely ignore these values.
    cluster_t *diagnostics_cluster =
        cluster::create(meter_endpoint, kSmartMeterDiagnosticsClusterId, CLUSTER_FLAG_SERVER);
    static char empty_meter_identity[] = "";
    if (diagnostics_cluster == nullptr ||
        attribute::create(diagnostics_cluster, kSmartMeterExternalAntennaAttributeId,
                          ATTRIBUTE_FLAG_NONE,
                          esp_matter_bool(g_external_antenna)) == nullptr ||
        attribute::create(diagnostics_cluster, kSmartMeterOtaReadyAttributeId,
                          ATTRIBUTE_FLAG_NONE,
                          esp_matter_bool(true)) == nullptr ||
        attribute::create(diagnostics_cluster, kSmartMeterActivePowerObisSeenAttributeId,
                          ATTRIBUTE_FLAG_NONE, esp_matter_bool(false)) == nullptr ||
        attribute::create(diagnostics_cluster, kSmartMeterIdAttributeId,
                          ATTRIBUTE_FLAG_NONE,
                          esp_matter_char_str(empty_meter_identity, 0),
                          kSmartMeterIdentityMaxLength) == nullptr ||
        attribute::create(diagnostics_cluster, kSmartMeterManufacturerAttributeId,
                          ATTRIBUTE_FLAG_NONE,
                          esp_matter_char_str(empty_meter_identity, 0),
                          kSmartMeterIdentityMaxLength) == nullptr) {
        ESP_LOGE(TAG, "Failed to create smart-meter diagnostics cluster");
        abort();
    }

    // Reliability diagnostics are additive; the standard meter topology stays fixed.
    for (uint32_t id : {kSmartMeterPowerStatus, kSmartMeterImportStatus, kSmartMeterExportStatus,
            kSmartMeterProfile, kSmartMeterDetection, kSmartMeterPowerSource,
            kSmartMeterLastError, kSmartMeterReaderHealth, kSmartMeterIdSource,
            kSmartMeterImportSource, kSmartMeterExportSource}) {
        if (!attribute::create(diagnostics_cluster, id, ATTRIBUTE_FLAG_NONE,
                esp_matter_char_str(empty_meter_identity, 0), kSmartMeterIdentityMaxLength)) abort();
    }
    for (uint32_t id : {kSmartMeterPowerAge, kSmartMeterImportAge, kSmartMeterExportAge,
            kSmartMeterValidFrames, kSmartMeterCrcErrors, kSmartMeterDecodeErrors,
            kSmartMeterRejectedReadings, kSmartMeterFrameAge, kSmartMeterUartOverflows}) {
        const bool age = id == kSmartMeterPowerAge || id == kSmartMeterImportAge ||
            id == kSmartMeterExportAge || id == kSmartMeterFrameAge;
        if (!attribute::create(diagnostics_cluster, id, ATTRIBUTE_FLAG_NONE,
                esp_matter_uint32(age ? UINT32_MAX : 0))) abort();
    }

    // On means external U.FL antenna, Off means internal ceramic antenna.
    // Keep the exact v1.12 meter layout. HA exposes this cluster as its
    // antenna switch; no separate outlet endpoint is advertised to Apple.
    cluster::on_off::config_t antenna_config = {};
    antenna_config.on_off = g_external_antenna;
    cluster_t *antenna_cluster = cluster::on_off::create(
        meter_endpoint, &antenna_config, CLUSTER_FLAG_SERVER);
    // The pinned ESP-Matter legacy cluster helper registers Off only. HA
    // sends On (command 0x01) when selecting the external antenna; without
    // explicitly advertising it, Matter rejects the request with 0x81.
    if (antenna_cluster == nullptr ||
        cluster::on_off::command::create_on(antenna_cluster) == nullptr) {
        ESP_LOGE(TAG, "Failed to create Matter antenna switch");
        abort();
    }

    cluster::keypad_input::config_t keypad_config;
    keypad_config.delegate = &g_pin_delegate;
    cluster_t *keypad_cluster = cluster::keypad_input::create(
        meter_endpoint, &keypad_config, CLUSTER_FLAG_SERVER);
    if (keypad_cluster == nullptr ||
        cluster::keypad_input::feature::number_keys::add(keypad_cluster) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create Matter PIN-input cluster");
        abort();
    }

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    esp_openthread_platform_config_t thread_config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&thread_config);
#else
#error "This application requires Matter over Thread"
#endif

    error = esp_matter::start(matter_event_cb);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %s", esp_err_to_name(error));
        abort();
    }

    CHIP_ERROR schedule_error = DeviceLayer::PlatformMgr().ScheduleWork(
        initialize_antenna_controls, 0);
    if (schedule_error != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to schedule antenna-control initialization: %" CHIP_ERROR_FORMAT,
                 schedule_error.Format());
        abort();
    }
    schedule_error = DeviceLayer::PlatformMgr().ScheduleWork(
        initialize_energy_cluster, static_cast<intptr_t>(g_meter_endpoint_id));
    if (schedule_error != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to schedule energy-cluster initialization: %" CHIP_ERROR_FORMAT,
                 schedule_error.Format());
        abort();
    }

    error = smart_meter_start(g_meter_endpoint_id, &g_power_delegate);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start SML reader: %s", esp_err_to_name(error));
        abort();
    }

    error = smart_meter_register_console_commands();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register meter-pin console command: %s",
                 esp_err_to_name(error));
    }

    esp_console_cmd_t antenna_command = {};
    antenna_command.command = "antenna";
    antenna_command.help = "Select the internal or external antenna and restart";
    antenna_command.hint = "<internal|external>";
    antenna_command.func = antenna_console_command;
    error = esp_console_cmd_register(&antenna_command);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register antenna recovery command: %s",
                 esp_err_to_name(error));
    }

    if (xTaskCreate(factory_reset_button_task, "factory_reset", 3072, nullptr, 3,
                    nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create factory-reset button task");
        abort();
    }

    if (xTaskCreate(ota_health_task, "ota_health", 3072, nullptr, 3, nullptr) != pdPASS) {
        abort();
    }

    ESP_LOGI(TAG, "Matter-over-Thread smart meter ready on endpoint %u",
             g_meter_endpoint_id);
}
