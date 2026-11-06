#include "smart_meter.h"

#include "electrical_meter_delegate.h"
#include "sml.h"
#include "meter_readings.h"

#include <app/data-model/Nullable.h>
#include <data_model_provider/clusters/electrical_energy_measurement/integration.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/uart.h>
#include <esp_check.h>
#include <esp_console.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
#include <ctype.h>
#include <limits.h>
#include <new>
#include <platform/CHIPDeviceLayer.h>
#include <stdio.h>
#include <string.h>
#include <system/SystemClock.h>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;

namespace {
constexpr char TAG[] = "smart_meter";

constexpr uint8_t kMeterIdObis[] = {1, 0, 96, 1, 0, 255};
constexpr uint8_t kMeterIdFactoryAlias[] = {0, 0, 96, 1, 255, 255};
constexpr uint8_t kMeterIdEmhAlias[] = {1, 0, 0, 0, 9, 255};
constexpr const char *kIdSources[] = {"1-0:96.1.0*255", "0-0:96.1.0*255", "0-0:96.1.255*255", "1-0:0.0.9*255", "SML serverId", "none"};
constexpr uint8_t kMeterIdAlias[] = {0, 0, 96, 1, 0, 255};
constexpr uint8_t kMeterManufacturerObis[] = {129, 129, 199, 130, 3, 255};
constexpr uint8_t kMeterManufacturerAlias[] = {1, 0, 96, 50, 1, 1};
constexpr size_t kMaxMeterIdentityLength = kSmartMeterIdentityMaxLength;

struct TextReading {
    bool valid = false;
    char value[kMaxMeterIdentityLength + 1] = {};
};

struct PendingFrame : meter::Frame {
    TextReading meter_id;
    TextReading manufacturer;
    uint8_t id_rank = 5;
    bool primary_manufacturer = false;
    bool identity_conflict = false;
};

struct State {
    uint16_t endpoint_id = 0;
    SmartMeterPowerDelegate *power_delegate = nullptr;
    meter::Tracker readings;
    uint32_t valid_frames = 0, crc_errors = 0, decode_errors = 0, uart_overflows = 0;
    uint64_t last_frame_ms = 0, last_uart_ms = 0;
    bool uart_seen = false;
    uint64_t session = 0;
    bool ever_power = false;
    uint8_t id_rank = 5;
    uint64_t identity_ms = 0;
    TextReading proposed_identity;
    unsigned identity_confirmations = 0;
    TextReading meter_id;
    TextReading manufacturer;
    PendingFrame pending;
};

State g_state;

enum class OpticalRequestType : uint8_t {
    Pin,
    ShortPulse,
    LongPulse,
};

struct OpticalRequest {
    OpticalRequestType type;
    char digits[4];
};

QueueHandle_t g_optical_queue = nullptr;
SemaphoreHandle_t g_uart_mutex = nullptr;
QueueHandle_t g_uart_events = nullptr;
TaskHandle_t g_led_task_handle = nullptr;
std::atomic_bool g_optical_busy{false};
std::atomic_bool g_antenna_confirmation_pending{false};

uint64_t monotonic_ms() { return System::SystemClock().GetMonotonicTimestamp().count(); }

constexpr ledc_mode_t kLedSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kLedTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kLedChannel = LEDC_CHANNEL_0;
constexpr uint32_t kLedMaxDuty = (1U << LEDC_TIMER_10_BIT) - 1U;
constexpr uint32_t kLedBreathMinDuty = 4;
constexpr uint32_t kLedBreathMaxDuty = 180;
constexpr uint32_t kLedStepMs = 20;
constexpr uint32_t kLedBreathSteps = 200;
constexpr uint32_t kLedReceivePulseMs = 160;
constexpr uint32_t kLedAntennaBlinkMs = 80;
constexpr unsigned kLedAntennaBlinks = 3;

struct PublishSnapshot {
    uint16_t endpoint_id = 0;
    SmartMeterPowerDelegate *delegate = nullptr;
    meter::Tracker readings;
    TextReading meter_id, manufacturer;
    uint32_t valid_frames = 0, crc_errors = 0, decode_errors = 0, uart_overflows = 0;
    uint64_t now_ms = 0, last_frame_ms = 0, session = 0;
    uint8_t id_rank = 5;
    bool optical_busy = false, uart_recent = false;
    bool publish_power = false, publish_energy = false, applied_power_fresh = false, ever_power = false;
};
// One static snapshot in flight. Only the reader writes it, and only after the
// Matter callback releases it. No allocation failure, data races, or lost retry.
PublishSnapshot g_queued, g_ack;
enum Publication { Idle, Pending, Done, Failed };
std::atomic<Publication> g_publication{Idle};
bool g_have_ack = false;
meter::Reading g_power_ack, g_import_ack, g_export_ack;
uint64_t g_retry_after = 0, g_power_published_ms = 0, g_energy_published_ms = 0;
bool g_ack_power_fresh = false;

uint8_t commit_pending_frame()
{
    auto &pending = g_state.pending;
    if (pending.identity_conflict) {
        g_state.readings.reject("conflicting meter identity"); pending = {}; return 0;
    }
    const uint64_t now = monotonic_ms();
    if (pending.id_rank > g_state.id_rank && now - g_state.identity_ms < g_state.readings.energy_timeout_ms)
        pending.meter_id = {}; // Do not alternate between equivalent identifier fields.
    bool id_changed = pending.meter_id.valid && (!g_state.meter_id.valid ||
        strcmp(pending.meter_id.value, g_state.meter_id.value));
    if (id_changed && g_state.meter_id.valid) {
        if (!g_state.proposed_identity.valid || strcmp(g_state.proposed_identity.value, pending.meter_id.value)) {
            g_state.proposed_identity = pending.meter_id; g_state.identity_confirmations = 0;
        }
        if (++g_state.identity_confirmations < 3) { pending = {}; return 0; }
        g_state.readings.reset_meter(); ++g_state.session;
        g_state.manufacturer = {};
    }
    bool manufacturer_changed = pending.manufacturer.valid && (!g_state.manufacturer.valid ||
        strcmp(pending.manufacturer.value, g_state.manufacturer.value));
    if (pending.meter_id.valid) {
        g_state.meter_id = pending.meter_id; g_state.id_rank = pending.id_rank; g_state.identity_ms = now;
        g_state.proposed_identity = {}; g_state.identity_confirmations = 0;
    }
    if (pending.manufacturer.valid) g_state.manufacturer = pending.manufacturer;
    g_state.readings.commit(pending, monotonic_ms());
    g_state.ever_power = g_state.ever_power || g_state.readings.power.valid;
    pending = {};
    return (id_changed ? 1 : 0) | (manufacturer_changed ? 2 : 0);
}

bool decode_identity_value(char (&output)[kMaxMeterIdentityLength + 1], bool server = false)
{
    uint8_t raw[kMaxMeterIdentityLength];
    size_t length = 0;
    if (!(server ? smlServerId(raw, sizeof(raw), &length) : smlOBISOctetString(raw, sizeof(raw), &length))) {
        return false;
    }

    bool printable = true;
    for (size_t i = 0; i < length; ++i) {
        printable = printable && raw[i] >= 0x20 && raw[i] <= 0x7e;
    }
    if (printable) {
        memcpy(output, raw, length);
        output[length] = '\0';
    } else {
        if (length * 2 > kMaxMeterIdentityLength) {
            return false;
        }
        constexpr char kHex[] = "0123456789ABCDEF";
        for (size_t i = 0; i < length; ++i) {
            output[i * 2] = kHex[raw[i] >> 4];
            output[i * 2 + 1] = kHex[raw[i] & 0x0f];
        }
        output[length * 2] = '\0';
    }
    return true;
}

const char *reading_status(const meter::Reading &reading, uint64_t now, uint64_t timeout)
{
    return !reading.valid ? "not received" : reading.fresh(now, timeout) ? "fresh" : "stale";
}

DataModel::Nullable<ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type>
make_energy_value(bool valid, int64_t milli_watt_hours, uint64_t received_ms)
{
    if (!valid) return DataModel::NullNullable;
    ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type value = {
        .energy = milli_watt_hours,
        .startTimestamp = NullOptional, .endTimestamp = NullOptional,
        .startSystime = NullOptional, .endSystime = MakeOptional(received_ms),
    };
    return DataModel::Nullable<ElectricalEnergyMeasurement::Structs::EnergyMeasurementStruct::Type>(value);
}

void publish_snapshot(intptr_t)
{
    auto &s = g_queued;
    const auto &r = s.readings;
    const uint64_t now = monotonic_ms();
    bool success = true;
    s.applied_power_fresh = r.power.fresh(now, r.power_timeout_ms);
    if (s.publish_power) {
        const auto error = r.power.fresh(now, r.power_timeout_ms) ?
            s.delegate->SetActivePowerMilliwatts(r.power.value) : s.delegate->ClearActivePower();
        success = error == CHIP_NO_ERROR && success;
    }
    if (s.publish_energy) {
        auto imported = make_energy_value(r.imported.valid, r.imported.value, r.imported.received_ms);
        auto exported = make_energy_value(r.exported.valid, r.exported.value, r.exported.received_ms);
        success = ElectricalEnergyMeasurement::NotifyCumulativeEnergyMeasured(s.endpoint_id, imported, exported) && success;
    }
    auto update = [&](uint32_t id, esp_matter_attr_val_t value) {
        success = esp_matter::attribute::update(s.endpoint_id, kSmartMeterDiagnosticsClusterId, id, &value) == ESP_OK && success;
    };
    auto text = [&](uint32_t id, const char *value) { update(id, esp_matter_char_str(const_cast<char *>(value), strlen(value))); };
    auto number = [&](uint32_t id, uint32_t value) { update(id, esp_matter_uint32(value)); };
    text(kSmartMeterIdAttributeId, s.meter_id.value);
    text(kSmartMeterManufacturerAttributeId, s.manufacturer.value);
    // Latch is independent of freshness; an actual zero is still received data.
    update(kSmartMeterActivePowerObisSeenAttributeId, esp_matter_bool(s.ever_power));
    text(kSmartMeterPowerStatus, reading_status(r.power, now, r.power_timeout_ms));
    text(kSmartMeterImportStatus, reading_status(r.imported, now, r.energy_timeout_ms));
    text(kSmartMeterExportStatus, reading_status(r.exported, now, r.energy_timeout_ms));
    text(kSmartMeterProfile, r.profile());
    text(kSmartMeterDetection, r.detection());
    text(kSmartMeterPowerSource, meter::source_name(r.source));
    text(kSmartMeterIdSource, kIdSources[s.id_rank]);
    text(kSmartMeterImportSource, r.imported.valid ? "1-0:1.8.0*255" : "none");
    text(kSmartMeterExportSource, r.exported.valid ? "1-0:2.8.0*255" : "none");
    number(kSmartMeterPowerAge, r.power.age(now));
    number(kSmartMeterImportAge, r.imported.age(now));
    number(kSmartMeterExportAge, r.exported.age(now));
    number(kSmartMeterValidFrames, s.valid_frames);
    number(kSmartMeterCrcErrors, s.crc_errors);
    number(kSmartMeterDecodeErrors, s.decode_errors);
    number(kSmartMeterRejectedReadings, r.rejected);
    number(kSmartMeterUartOverflows, s.uart_overflows);
    number(kSmartMeterFrameAge, s.valid_frames ? static_cast<uint32_t>((now - s.last_frame_ms) / 1000) : UINT32_MAX);
    text(kSmartMeterLastError, r.last_error);
    const char *health = s.optical_busy ? "optical command active" :
        !s.uart_recent ? "no UART data" :
        !s.valid_frames || now - s.last_frame_ms >= r.power_timeout_ms ? "no valid SML frames" :
        !r.power.fresh(now, r.power_timeout_ms) ? "power missing: check INFO/PIN" : "receiving";
    text(kSmartMeterReaderHealth, health);
    g_publication.store(success ? Done : Failed, std::memory_order_release);
}

bool changed(const meter::Reading &a, const meter::Reading &b)
{
    return a.valid != b.valid || (a.valid && a.value != b.value);
}

void publish_readings_if_due()
{
    const uint64_t now = monotonic_ms();
    const auto status = g_publication.load(std::memory_order_acquire);
    if (status == Pending) return;
    if (status == Done) {
        g_ack = g_queued;
        if (g_queued.publish_power) {
            g_power_published_ms = g_queued.now_ms; g_ack_power_fresh = g_queued.applied_power_fresh;
            g_power_ack = g_queued.readings.power;
        }
        if (g_queued.publish_energy) {
            g_energy_published_ms = g_queued.now_ms;
            g_import_ack = g_queued.readings.imported; g_export_ack = g_queued.readings.exported;
        }
        g_have_ack = true;
    }
    if (status == Failed) g_retry_after = now + 1000;
    g_publication.store(Idle);
    if (now < g_retry_after) return;
    const auto &r = g_state.readings;
    const uint64_t elapsed = now - g_ack.now_ms;
    const bool power_fresh = r.power.fresh(now, r.power_timeout_ms);
    const bool ack_fresh = g_ack_power_fresh;
    const bool session_changed = g_state.session != g_ack.session;
    const bool publish_power = !g_have_ack || session_changed || power_fresh != ack_fresh ||
        ((changed(r.power, g_power_ack) && now - g_power_published_ms >= CONFIG_SMARTMETER_MIN_UPDATE_INTERVAL_SECONDS * 1000ULL) ||
         now - g_power_published_ms >= CONFIG_SMARTMETER_MAX_UPDATE_INTERVAL_SECONDS * 1000ULL);
    const bool publish_energy = !g_have_ack || session_changed ||
        ((r.imported.revision != g_import_ack.revision || r.exported.revision != g_export_ack.revision) &&
         now - g_energy_published_ms >= CONFIG_SMARTMETER_MIN_UPDATE_INTERVAL_SECONDS * 1000ULL);
    const bool state_change = g_optical_busy.load() != g_ack.optical_busy ||
        strcmp(reading_status(r.imported, now, r.energy_timeout_ms),
               reading_status(g_ack.readings.imported, g_ack.now_ms, r.energy_timeout_ms)) ||
        strcmp(reading_status(r.exported, now, r.energy_timeout_ms),
               reading_status(g_ack.readings.exported, g_ack.now_ms, r.energy_timeout_ms));
    if (!publish_power && !publish_energy && !state_change && elapsed < 5000) return;
    g_queued = {};
    g_queued.endpoint_id = g_state.endpoint_id; g_queued.delegate = g_state.power_delegate;
    g_queued.readings = r; g_queued.meter_id = g_state.meter_id; g_queued.manufacturer = g_state.manufacturer;
    g_queued.valid_frames = g_state.valid_frames; g_queued.crc_errors = g_state.crc_errors;
    g_queued.decode_errors = g_state.decode_errors; g_queued.uart_overflows = g_state.uart_overflows;
    g_queued.now_ms = now; g_queued.last_frame_ms = g_state.last_frame_ms; g_queued.session = g_state.session; g_queued.ever_power = g_state.ever_power; g_queued.id_rank = g_state.id_rank;
    g_queued.optical_busy = g_optical_busy.load();
    g_queued.uart_recent = g_state.uart_seen && now - g_state.last_uart_ms < r.power_timeout_ms;
    g_queued.publish_power = publish_power; g_queued.publish_energy = publish_energy;
    g_publication.store(Pending, std::memory_order_release);
    if (DeviceLayer::PlatformMgr().ScheduleWork(publish_snapshot, 0) != CHIP_NO_ERROR) {
        g_publication.store(Failed, std::memory_order_release);
    }
}

void handle_list_end()
{
    auto &p = g_state.pending;
    uint8_t rank = smlOBISCheck(kMeterIdObis) ? 0 : smlOBISCheck(kMeterIdAlias) ? 1 :
        smlOBISCheck(kMeterIdFactoryAlias) ? 2 : smlOBISCheck(kMeterIdEmhAlias) ? 3 : 4;
    if (rank < 4) {
        TextReading next;
        next.valid = decode_identity_value(next.value);
        if (next.valid && rank <= p.id_rank) {
            if (p.meter_id.valid && p.id_rank == rank && strcmp(p.meter_id.value, next.value)) p.identity_conflict = true;
            p.meter_id = next; p.id_rank = rank;
        }
        return;
    }
    bool primary_manufacturer = smlOBISCheck(kMeterManufacturerObis);
    if (primary_manufacturer || smlOBISCheck(kMeterManufacturerAlias)) {
        TextReading next;
        next.valid = decode_identity_value(next.value);
        // 96.50.1 is a hardware revision on some meters, not a manufacturer.
        next.valid = next.valid && strlen(next.value) == 3;
        for (unsigned i = 0; next.valid && i < 3; ++i) next.valid = next.value[i] >= 'A' && next.value[i] <= 'Z';
        if (next.valid && (primary_manufacturer || !p.primary_manufacturer)) {
            p.manufacturer = next; p.primary_manufacturer = primary_manufacturer;
        }
        return;
    }
    struct Handler { uint8_t c, d; sml_units_t unit; meter::Candidate *reading; };
    const Handler handlers[] = {
        {16, 7, SML_WATT, &p.total}, {1, 7, SML_WATT, &p.import_power}, {2, 7, SML_WATT, &p.export_power},
        {36, 7, SML_WATT, &p.phases[0]}, {56, 7, SML_WATT, &p.phases[1]}, {76, 7, SML_WATT, &p.phases[2]},
        {1, 8, SML_WATT_HOUR, &p.imported}, {2, 8, SML_WATT_HOUR, &p.exported},
    };
    for (const auto &h : handlers) {
        const uint8_t obis[] = {1, 0, h.c, h.d, 0, 255};
        if (!smlOBISCheck(obis)) continue;
        long long raw = 0; signed char scaler = 0; int64_t value, quantum;
        const auto result = smlOBISDecode(raw, scaler, h.unit);
        if (result != SML_DECODE_OK || !meter::scale(raw, scaler, value) || !meter::scale(1, scaler, quantum)) {
            if (g_state.decode_errors != UINT32_MAX) ++g_state.decode_errors;
            g_state.readings.last_error = result == SML_DECODE_UNIT ? "wrong or missing unit" : "invalid number/scaler/overflow";
            h.reading->valid = false; h.reading->conflict = true;
            continue;
        }
        h.reading->offer(value, quantum > 0 ? quantum : 1);
    }
}

void set_led_duty(uint32_t duty)
{
    if (ledc_set_duty(kLedSpeedMode, kLedChannel, duty) == ESP_OK) {
        (void) ledc_update_duty(kLedSpeedMode, kLedChannel);
    }
}

void status_led_task(void *)
{
    uint32_t breath_step = 0;
    TickType_t pulse_until = 0;

    while (true) {
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kLedStepMs)) != 0) {
            pulse_until = xTaskGetTickCount() + pdMS_TO_TICKS(kLedReceivePulseMs);
        }

        // Keep all PWM writes in this task so receive pulses and breathing
        // cannot interrupt the antenna confirmation pattern.
        if (g_antenna_confirmation_pending.load()) {
            for (unsigned blink = 0; blink < kLedAntennaBlinks; ++blink) {
                set_led_duty(kLedMaxDuty);
                vTaskDelay(pdMS_TO_TICKS(kLedAntennaBlinkMs));
                set_led_duty(0);
                vTaskDelay(pdMS_TO_TICKS(kLedAntennaBlinkMs));
            }
            pulse_until = 0;
            g_antenna_confirmation_pending.store(false);
            continue;
        }

        const TickType_t now = xTaskGetTickCount();
        if (pulse_until != 0 && static_cast<int32_t>(pulse_until - now) > 0) {
            set_led_duty(kLedMaxDuty);
            continue;
        }
        pulse_until = 0;

        // Four-second, gamma-corrected breathing cycle. Its deliberately dim
        // maximum makes the full-brightness SML receive pulse easy to see.
        const uint32_t half = kLedBreathSteps / 2;
        const uint32_t linear = breath_step < half ? breath_step : kLedBreathSteps - breath_step;
        const uint32_t curved = (linear * linear * (kLedBreathMaxDuty - kLedBreathMinDuty)) /
                                (half * half);
        set_led_duty(kLedBreathMinDuty + curved);
        breath_step = (breath_step + 1) % kLedBreathSteps;
    }
}

esp_err_t start_status_led()
{
    ledc_timer_config_t timer = {};
    timer.speed_mode = kLedSpeedMode;
    timer.duty_resolution = LEDC_TIMER_10_BIT;
    timer.timer_num = kLedTimer;
    timer.freq_hz = 5000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "Failed to configure status LED timer");

    ledc_channel_config_t channel = {};
    channel.gpio_num = CONFIG_SMARTMETER_LED_GPIO;
    channel.speed_mode = kLedSpeedMode;
    channel.channel = kLedChannel;
    channel.intr_type = LEDC_INTR_DISABLE;
    channel.timer_sel = kLedTimer;
    channel.duty = 0;
    channel.hpoint = 0;
#if CONFIG_SMARTMETER_LED_ACTIVE_LOW
    channel.flags.output_invert = 1;
#endif
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG,
                        "Failed to configure status LED channel");

    const BaseType_t created = xTaskCreate(status_led_task, "status_led", 2048, nullptr, 2,
                                           &g_led_task_handle);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "Failed to create status LED task");
    return ESP_OK;
}

void notify_sml_frame_received()
{
    if (g_led_task_handle != nullptr) {
        xTaskNotifyGive(g_led_task_handle);
    }
}

void smart_meter_task(void *)
{
    uint8_t buffer[256];
    uint64_t last_byte_ms = 0;
    bool was_busy = false;
    while (true) {
        // Run even on silence, partial frames, or a lengthy optical PIN sequence.
        publish_readings_if_due();
        const bool busy = g_optical_busy.load();
        if (busy || was_busy || (last_byte_ms && monotonic_ms() - last_byte_ms > 2000)) {
            smlReset(); g_state.pending = {}; last_byte_ms = 0;
        }
        was_busy = busy;
        if (busy) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        if (xSemaphoreTake(g_uart_mutex, pdMS_TO_TICKS(50)) != pdTRUE) continue;
        if (g_optical_busy.load()) { xSemaphoreGive(g_uart_mutex); continue; }
        uart_event_t event;
        bool overflow = false;
        while (xQueueReceive(g_uart_events, &event, 0) == pdTRUE) {
            if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
                event.type == UART_PARITY_ERR || event.type == UART_FRAME_ERR) overflow = true;
        }
        if (overflow) {
            if (g_state.uart_overflows != UINT32_MAX) ++g_state.uart_overflows;
            g_state.readings.last_error = "UART overflow or framing error";
            uart_flush_input(static_cast<uart_port_t>(CONFIG_SMARTMETER_UART_NUM));
            smlReset(); g_state.pending = {};
        }
        const int length = uart_read_bytes(static_cast<uart_port_t>(CONFIG_SMARTMETER_UART_NUM), buffer,
                                           sizeof(buffer), pdMS_TO_TICKS(250));
        xSemaphoreGive(g_uart_mutex);
        if (length <= 0) continue;
        g_state.last_uart_ms = last_byte_ms = monotonic_ms(); g_state.uart_seen = true;
        for (int i = 0; i < length; ++i) {
            unsigned char byte = buffer[i];
            const auto state = smlState(byte);
            if (state == SML_VERSION) g_state.pending = {};
            else if (state == SML_FINAL) {
                while (smlNextEntry()) handle_list_end();
                if (!g_state.pending.meter_id.valid && decode_identity_value(g_state.pending.meter_id.value, true)) {
                    g_state.pending.meter_id.valid = true; g_state.pending.id_rank = 4;
                }
                if (g_state.valid_frames != UINT32_MAX) ++g_state.valid_frames;
                g_state.last_frame_ms = monotonic_ms();
                commit_pending_frame();
                notify_sml_frame_received();
                publish_readings_if_due();
            } else if (state == SML_CHECKSUM_ERROR || state == SML_UNEXPECTED) {
                g_state.pending = {};
                auto &counter = state == SML_CHECKSUM_ERROR ? g_state.crc_errors : g_state.decode_errors;
                if (counter != UINT32_MAX) ++counter;
                g_state.readings.last_error = state == SML_CHECKSUM_ERROR ? "transport CRC failed" : "malformed or oversized SML frame";
            }
        }
    }
}

esp_err_t transmit_ir_flash(uint32_t duration_ms)
{
    // A UART 0x00 byte keeps TX active for the start bit and all eight data
    // bits, with only the stop bit inactive. Repeating it therefore produces
    // a stable 90%-duty optical burst while preserving normal UART idle state.
    const uart_port_t port = static_cast<uart_port_t>(CONFIG_SMARTMETER_UART_NUM);
    const uint64_t numerator =
        static_cast<uint64_t>(duration_ms) * CONFIG_SMARTMETER_UART_BAUD;
    size_t remaining = static_cast<size_t>((numerator + 9'999) / 10'000);
    if (remaining == 0) {
        remaining = 1;
    }

    uint8_t active_bytes[64] = {};
    while (remaining != 0) {
        const size_t chunk = remaining < sizeof(active_bytes) ? remaining : sizeof(active_bytes);
        const int written = uart_write_bytes(port, active_bytes, chunk);
        if (written != static_cast<int>(chunk)) {
            return ESP_FAIL;
        }
        remaining -= chunk;
    }

    return uart_wait_tx_done(port, pdMS_TO_TICKS(duration_ms + 1000));
}

esp_err_t send_short_flash()
{
    ESP_RETURN_ON_ERROR(transmit_ir_flash(CONFIG_SMARTMETER_PIN_SHORT_FLASH_MS), TAG,
                        "IR flash failed");
    vTaskDelay(pdMS_TO_TICKS(CONFIG_SMARTMETER_PIN_FLASH_GAP_MS));
    return ESP_OK;
}

esp_err_t transmit_pin(const OpticalRequest &request)
{
    // Netze Duisburg specifies two short optical activations before the meter
    // presents the first zero of its PIN entry display.
    for (unsigned activation = 0; activation < 2; ++activation) {
        ESP_RETURN_ON_ERROR(send_short_flash(), TAG, "PIN-entry activation failed");
    }
    vTaskDelay(pdMS_TO_TICKS(CONFIG_SMARTMETER_PIN_WAKE_SETTLE_MS));

    for (char digit_char : request.digits) {
        const unsigned digit = static_cast<unsigned>(digit_char - '0');
        for (unsigned count = 0; count < digit; ++count) {
            ESP_RETURN_ON_ERROR(send_short_flash(), TAG, "PIN digit transmission failed");
        }
        // The meter advances to the next digit after at least three seconds
        // without light. A zero is entered by sending no flash in this period.
        vTaskDelay(pdMS_TO_TICKS(CONFIG_SMARTMETER_PIN_DIGIT_WAIT_MS));
    }
    return ESP_OK;
}

void optical_transmitter_task(void *)
{
    OpticalRequest request = {};
    while (true) {
        if (xQueueReceive(g_optical_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const bool pin = request.type == OpticalRequestType::Pin;
        ESP_LOGI(TAG, "Starting optical %s", pin ? "meter PIN entry" :
                 request.type == OpticalRequestType::LongPulse ? "long pulse" : "short pulse");
        xSemaphoreTake(g_uart_mutex, portMAX_DELAY);
        ESP_LOGI(TAG, "Optical transmitter acquired UART");
        uart_flush_input(static_cast<uart_port_t>(CONFIG_SMARTMETER_UART_NUM));
        const esp_err_t error = pin ? transmit_pin(request) : transmit_ir_flash(
            request.type == OpticalRequestType::LongPulse ?
                CONFIG_SMARTMETER_LONG_FLASH_MS : CONFIG_SMARTMETER_PIN_SHORT_FLASH_MS);
        uart_flush_input(static_cast<uart_port_t>(CONFIG_SMARTMETER_UART_NUM));
        xSemaphoreGive(g_uart_mutex);

        memset(&request, 0, sizeof(request));
        g_optical_busy.store(false);
        if (error == ESP_OK) {
            ESP_LOGI(TAG, "Optical transmission complete");
        } else {
            ESP_LOGE(TAG, "Optical transmission failed: %s", esp_err_to_name(error));
        }
    }
}

int meter_pin_console_command(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: meter-pin <4 digits>\n");
        return 1;
    }

    const esp_err_t error = smart_meter_submit_pin(argv[1]);
    if (error != ESP_OK) {
        printf("PIN request rejected: %s\n", esp_err_to_name(error));
        return 1;
    }
    printf("PIN request accepted; watch the meter display.\n");
    return 0;
}
} // namespace

esp_err_t smart_meter_confirm_antenna_toggle()
{
    if (g_led_task_handle == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    g_antenna_confirmation_pending.store(true);
    xTaskNotifyGive(g_led_task_handle);
    const TickType_t started = xTaskGetTickCount();
    // The pattern takes 480 ms. Bound the wait so a failed LED task cannot
    // prevent antenna recovery from restarting the board.
    while (g_antenna_confirmation_pending.load()) {
        if (xTaskGetTickCount() - started >= pdMS_TO_TICKS(1000)) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(kLedStepMs));
    }
    return ESP_OK;
}

esp_err_t smart_meter_start(uint16_t matter_endpoint_id, SmartMeterPowerDelegate *power_delegate)
{
    ESP_RETURN_ON_FALSE(power_delegate != nullptr, ESP_ERR_INVALID_ARG, TAG, "Power delegate is null");

    g_state.endpoint_id = matter_endpoint_id;
    g_state.power_delegate = power_delegate;

    ESP_RETURN_ON_ERROR(start_status_led(), TAG, "Failed to start status LED");

    uart_config_t uart_config = {};
    uart_config.baud_rate = CONFIG_SMARTMETER_UART_BAUD;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    const uart_port_t port = static_cast<uart_port_t>(CONFIG_SMARTMETER_UART_NUM);
    ESP_RETURN_ON_ERROR(uart_driver_install(port, 8192, 0, 32, &g_uart_events, 0), TAG,
                        "Failed to install UART driver");
    ESP_RETURN_ON_ERROR(uart_param_config(port, &uart_config), TAG, "Failed to configure UART");
    ESP_RETURN_ON_ERROR(uart_set_pin(port, CONFIG_SMARTMETER_UART_TX_GPIO,
                                     CONFIG_SMARTMETER_UART_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "Failed to assign UART pins");
#if CONFIG_SMARTMETER_IR_TX_INVERTED
    ESP_RETURN_ON_ERROR(uart_set_line_inverse(port, UART_SIGNAL_TXD_INV), TAG,
                        "Failed to invert UART TX");
#endif

    g_uart_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(g_uart_mutex != nullptr, ESP_ERR_NO_MEM, TAG,
                        "Failed to create UART mutex");
    g_optical_queue = xQueueCreate(1, sizeof(OpticalRequest));
    ESP_RETURN_ON_FALSE(g_optical_queue != nullptr, ESP_ERR_NO_MEM, TAG,
                        "Failed to create optical queue");

    g_state.readings.power_timeout_ms = CONFIG_SMARTMETER_POWER_FRESHNESS_SECONDS * 1000ULL;
    g_state.readings.energy_timeout_ms = CONFIG_SMARTMETER_ENERGY_FRESHNESS_SECONDS * 1000ULL;
    g_state.readings.max_power_mw = CONFIG_SMARTMETER_MAX_POWER_W * 1000LL;
    BaseType_t created = xTaskCreate(smart_meter_task, "sml_reader", 6144, nullptr, 5, nullptr);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "Failed to create SML task");
    created = xTaskCreate(optical_transmitter_task, "optical_tx", 4096, nullptr, 5, nullptr);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "Failed to create PIN transmitter task");
    return ESP_OK;
}

esp_err_t smart_meter_submit_pin(const char *pin)
{
    ESP_RETURN_ON_FALSE(pin != nullptr, ESP_ERR_INVALID_ARG, TAG, "PIN is null");
    ESP_RETURN_ON_FALSE(strlen(pin) == 4, ESP_ERR_INVALID_ARG, TAG,
                        "PIN must contain four digits");

    bool any_nonzero = false;
    for (size_t index = 0; index < 4; ++index) {
        ESP_RETURN_ON_FALSE(isdigit(static_cast<unsigned char>(pin[index])),
                            ESP_ERR_INVALID_ARG, TAG, "PIN must contain only digits");
        any_nonzero = any_nonzero || pin[index] != '0';
    }
    ESP_RETURN_ON_FALSE(any_nonzero, ESP_ERR_INVALID_ARG, TAG,
                        "Meter PIN 0000 is not valid");
    ESP_RETURN_ON_FALSE(g_optical_queue != nullptr, ESP_ERR_INVALID_STATE, TAG,
                        "Optical transmitter is not initialized");

    bool expected = false;
    ESP_RETURN_ON_FALSE(g_optical_busy.compare_exchange_strong(expected, true),
                        ESP_ERR_INVALID_STATE, TAG, "Optical transmitter is busy");

    OpticalRequest request = {};
    request.type = OpticalRequestType::Pin;
    memcpy(request.digits, pin, sizeof(request.digits));
    if (xQueueSend(g_optical_queue, &request, 0) != pdTRUE) {
        g_optical_busy.store(false);
        memset(&request, 0, sizeof(request));
        return ESP_ERR_TIMEOUT;
    }

    memset(&request, 0, sizeof(request));
    return ESP_OK;
}

esp_err_t smart_meter_submit_pulse(bool long_pulse)
{
    ESP_RETURN_ON_FALSE(g_optical_queue != nullptr, ESP_ERR_INVALID_STATE, TAG,
                        "Optical transmitter is not initialized");
    bool expected = false;
    ESP_RETURN_ON_FALSE(g_optical_busy.compare_exchange_strong(expected, true),
                        ESP_ERR_INVALID_STATE, TAG, "Optical transmitter is busy");

    OpticalRequest request = {};
    request.type = long_pulse ? OpticalRequestType::LongPulse : OpticalRequestType::ShortPulse;
    if (xQueueSend(g_optical_queue, &request, 0) != pdTRUE) {
        g_optical_busy.store(false);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t smart_meter_register_console_commands()
{
    esp_console_cmd_t command = {};
    command.command = "meter-pin";
    command.help = "Enter a four-digit meter PIN through the IR head";
    command.hint = "<4 digits>";
    command.func = meter_pin_console_command;
    return esp_console_cmd_register(&command);
}
