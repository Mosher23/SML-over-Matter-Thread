"""Exercise the actual UART-to-Matter publisher with a delayed/failing work queue."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]

class PublicationTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('c++'), 'C++ compiler required')
    def test_coalescing_retries_timestamps_and_stale_recovery(self):
        source = (ROOT / 'firmware/main/smart_meter.cpp').read_text()
        constants = '\n'.join(re.findall(r'^constexpr (?:uint8_t kMeter|const char \*kIdSources).*;', source, re.M)) + '\n' + '\n'.join(re.findall(r'^constexpr .*;', (ROOT / 'firmware/main/smart_meter.h').read_text(), re.M))
        state = source[source.index('constexpr size_t kMaxMeterIdentityLength'):source.index('enum class OpticalRequestType')]
        snapshot = source[source.index('struct PublishSnapshot'):source.index('uint8_t commit_pending_frame')]
        validation = source[source.index('uint8_t commit_pending_frame'):source.index('const char *reading_status')]
        handler = source[source.index('void handle_list_end'):source.index('void set_led_duty')]
        builders = (ROOT / 'firmware/tests/test_sml_reliability.cpp').read_text().split('using Bytes =',1)[1].split('void numeric_tests()',1)[0]
        builders = 'using Bytes =' + builders
        publisher = source[source.index('const char *reading_status'):source.index('void handle_list_end')]
        harness = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include "meter_readings.h"
#include "sml.cpp"
#include <vector>
using TickType_t = uint32_t;
constexpr int CHIP_NO_ERROR = 0, ESP_OK = 0;
#define CONFIG_SMARTMETER_MIN_UPDATE_INTERVAL_SECONDS 5
#define CONFIG_SMARTMETER_MAX_UPDATE_INTERVAL_SECONDS 3600
uint64_t clock_ms = 0;
uint64_t monotonic_ms() { return clock_ms; }
struct SmartMeterPowerDelegate {
    std::optional<int64_t> value;
    unsigned calls = 0;
    int SetActivePowerMilliwatts(int64_t v) { value = v; ++calls; return 0; }
    int ClearActivePower() { value = std::nullopt; ++calls; return 0; }
};
__CONSTANTS__
__STATE__
std::atomic_bool g_optical_busy{false};
__SNAPSHOT__
const std::nullopt_t NullOptional = std::nullopt;
template<class T> auto MakeOptional(T v) { return std::optional<T>(v); }
namespace DataModel {
constexpr std::nullopt_t NullNullable = std::nullopt;
template<class T> using Nullable = std::optional<T>;
}
namespace ElectricalEnergyMeasurement {
namespace Structs { struct EnergyMeasurementStruct { struct Type {
    int64_t energy; std::optional<uint64_t> startTimestamp, endTimestamp, startSystime, endSystime;
}; }; }
DataModel::Nullable<Structs::EnergyMeasurementStruct::Type> imported, exported;
bool NotifyCumulativeEnergyMeasured(uint16_t, decltype(imported) a, decltype(exported) b) {
    imported = a; exported = b; return true;
}
}
struct esp_matter_attr_val_t { uint32_t integer = 0; const char *text = nullptr; };
esp_matter_attr_val_t esp_matter_char_str(char *s, size_t) { return {0,s}; }
esp_matter_attr_val_t esp_matter_uint32(uint32_t v) { return {v,nullptr}; }
esp_matter_attr_val_t esp_matter_bool(bool v) { return {v,nullptr}; }
namespace esp_matter { namespace attribute {
bool fail = false;
std::map<unsigned,std::string> text;
int update(uint16_t, uint32_t, uint32_t id, esp_matter_attr_val_t *v) {
    if(v->text) text[id] = v->text;
    return fail ? -1 : 0;
}
} }
namespace DeviceLayer {
struct Platform {
    bool fail = false;
    void (*work)(intptr_t) = nullptr;
    int ScheduleWork(void (*fn)(intptr_t), intptr_t) { if(fail) return -1; assert(!work); work = fn; return 0; }
    void run() { auto fn = work; assert(fn); work = nullptr; fn(0); }
} platform;
Platform &PlatformMgr() { return platform; }
}
__VALIDATION__
__PUBLISHER__
__HANDLER__
__BUILDERS__
void receive(Bytes body) {
    g_state.pending = {};
    assert(feed(frame(body)) == SML_FINAL);
    while(smlNextEntry()) handle_list_end();
    if(!g_state.pending.meter_id.valid && decode_identity_value(g_state.pending.meter_id.value,true)) {
        g_state.pending.meter_id.valid = true; g_state.pending.id_rank = 4;
    }
    commit_pending_frame();
}
void pump() { publish_readings_if_due(); if(DeviceLayer::platform.work) DeviceLayer::platform.run(); publish_readings_if_due(); }
int main() {
    SmartMeterPowerDelegate delegate;
    g_state.endpoint_id = 1; g_state.power_delegate = &delegate;
    pump(); assert(!delegate.value && g_have_ack);
    meter::Frame f; f.total.offer(450000); f.imported.offer(1000000);
    clock_ms = 1; g_state.readings.commit(f,clock_ms); pump(); assert(delegate.value == 450000);
    clock_ms = 100; f.total = {}; f.total.offer(600000); g_state.readings.commit(f,clock_ms);
    g_optical_busy = true; pump(); // diagnostic update cannot acknowledge unpublished power
    assert(delegate.value == 450000 && g_power_ack.value == 450000);
    clock_ms = 5001; pump(); assert(delegate.value == 600000);
    assert(ElectricalEnergyMeasurement::imported->endSystime == 100);
    // Scheduling and attribute failures leave work dirty, then retry.
    clock_ms = 10002; f.total = {}; f.total.offer(700000); g_state.readings.commit(f,clock_ms);
    DeviceLayer::platform.fail = true; publish_readings_if_due(); assert(g_publication == Failed);
    publish_readings_if_due(); assert(g_power_ack.value == 600000);
    DeviceLayer::platform.fail = false;
    clock_ms = 11003; esp_matter::attribute::fail = true; pump(); assert(g_power_ack.value == 600000);
    esp_matter::attribute::fail = false; clock_ms = 12004; pump(); assert(g_power_ack.value == 700000);
    clock_ms = 40002; g_optical_busy = false; pump(); assert(!delegate.value);
    assert(esp_matter::attribute::text[kSmartMeterPowerStatus] == "stale");
    // Same numerical reading recovers immediately after null.
    clock_ms = 40003; g_state.readings.commit(f,clock_ms); pump(); assert(delegate.value == 700000);
    // Delayed Matter work must not resurrect a measurement that expired in queue.
    clock_ms = 45004; f.total = {}; f.total.offer(800000); g_state.readings.commit(f,clock_ms);
    publish_readings_if_due(); assert(DeviceLayer::platform.work);
    clock_ms = 80005; DeviceLayer::platform.run(); publish_readings_if_due(); assert(!delegate.value);
    assert(!g_ack_power_fresh);
    auto energy = make_energy_value(true,123456,42);
    assert(energy->energy == 123456 && energy->endSystime == 42);
    // Run real decoded entries through the actual firmware handlers/commit.
    g_state = {}; clock_ms = 1;
    receive(message(entry(16,7,{0x63,1,194}))); // 450 W
    assert(g_state.readings.power.value == 450000 && g_state.ever_power);
    assert(!strcmp(g_state.meter_id.value,"SIM") && g_state.id_rank == 4);
    clock_ms = 2; receive(message(entry(16,7,{0x62,1},30)));
    assert(g_state.decode_errors == 1 && g_state.readings.power.received_ms == 1);
    clock_ms = 3; receive(message(entry(16,7,{0x52,255})));
    assert(g_state.readings.power.value == -1000);
    auto replacement = entry(96,1,{5,'M','T','R','1'});
    append(replacement,entry(16,7,{0x62,2}));
    receive(message(replacement,2)); receive(message(replacement,2));
    assert(!strcmp(g_state.meter_id.value,"SIM"));
    receive(message(replacement,2));
    assert(!strcmp(g_state.meter_id.value,"MTR1") && g_state.session == 1 && g_state.readings.power.value == 2000);
    // Lower-priority serverId cannot oscillate a recently preferred identity.
    receive(message(entry(16,7,{0x62,3})));
    assert(!strcmp(g_state.meter_id.value,"MTR1"));
    auto duplicate = entry(16,7,{0x62,4}); append(duplicate,entry(16,7,{0x62,5}));
    receive(message(duplicate,2));
    assert(g_state.readings.power.value == 3000 && g_state.readings.rejected > 0);
}
'''
        harness = harness.replace('__CONSTANTS__', constants).replace('__STATE__', state).replace('__SNAPSHOT__', snapshot).replace('__PUBLISHER__', publisher).replace('__VALIDATION__', validation).replace('__HANDLER__', handler).replace('__BUILDERS__', builders)
        with tempfile.TemporaryDirectory() as folder:
            src = Path(folder) / 'publication.cpp'; src.write_text(harness)
            binary = Path(folder) / 'publication'
            result = subprocess.run([shutil.which('c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-Wno-unused-const-variable', '-I', str(ROOT / 'firmware/main'), str(src), '-o', str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(binary)],check=True)
