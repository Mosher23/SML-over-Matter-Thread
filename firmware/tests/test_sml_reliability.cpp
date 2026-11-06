#include <cstdio>
#include <cassert>
#include <cstdint>
#include <vector>
#include <random>
#include "../main/sml.cpp"
#include "../main/meter_readings.h"
using Bytes = std::vector<uint8_t>;
void append(Bytes &a, const Bytes &b) { a.insert(a.end(), b.begin(), b.end()); }
Bytes entry(uint8_t c, uint8_t d, Bytes value, uint8_t unit = 27, int8_t scaler = 0) {
    Bytes b{0x77, 7, 1, 0, c, d, 0, 255, 1, 1, 0x62, unit, 0x52, static_cast<uint8_t>(scaler)};
    append(b, value); b.push_back(1); return b;
}
Bytes message(const Bytes &list, uint8_t entries = 1) {
    Bytes b{0x76, 2, 1, 0x62, 0, 0x62, 0, 0x72, 0x63, 7, 1,
            0x77, 1, 4, 'S', 'I', 'M', 1, 1, static_cast<uint8_t>(0x70 | entries)};
    append(b, list); append(b, {1, 1, 0x63, 0, 0, 0}); return b;
}
Bytes frame(Bytes body) {
    // SML file escaping, padding, and CRC-16/X25, independently generated.
    Bytes raw{27,27,27,27,1,1,1,1};
    unsigned run = 0;
    for (auto b : body) {
        raw.push_back(b);
        run = b == 27 ? run + 1 : 0;
        if (run == 4) { append(raw, {27,27,27,27}); run = 0; }
    }
    unsigned pad = (4 - body.size() % 4) % 4;
    for (unsigned i = 0; i < pad; ++i) raw.push_back(0);
    append(raw, {27,27,27,27,26,static_cast<uint8_t>(pad)});
    uint16_t crc = 0xffff;
    for (auto b : raw) {
        crc ^= b;
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
    }
    crc ^= 0xffff; raw.push_back(crc & 255); raw.push_back(crc >> 8); return raw;
}
sml_states_t feed(Bytes b, bool reset = true) {
    if (reset) smlReset();
    sml_states_t state = SML_NEXT;
    for (auto byte : b) state = smlState(byte);
    return state;
}
void numeric_tests() {
    auto f = frame(message(entry(16,7,{0x53,0xff,0xff})));
    assert(feed(f) == SML_FINAL && smlNextEntry());
    long long value = 0; signed char scaler = 0;
    assert(smlOBISDecode(value, scaler, SML_WATT) == SML_DECODE_OK && value == -1);
    assert(!smlNextEntry());
    unsigned char server[8]; size_t server_length = 0;
    assert(smlServerId(server, sizeof(server), &server_length) && server_length == 3 && !memcmp(server, "SIM", 3));
    assert(!smlServerId(server, 1, &server_length));
    assert(feed(frame(message(entry(16,7,{0x53,0x80,0})))) == SML_FINAL && smlNextEntry());
    assert(smlOBISDecode(value, scaler, SML_WATT) == SML_DECODE_OK && value == -32768);
    for (auto bytes : {Bytes{0x62,0}, Bytes{0x69,0x7f,255,255,255,255,255,255,255},
                       Bytes{0x59,0x80,0,0,0,0,0,0,0}}) {
        assert(feed(frame(message(entry(16,7,bytes)))) == SML_FINAL && smlNextEntry());
        assert(smlOBISDecode(value, scaler, SML_WATT) == SML_DECODE_OK);
    }
    assert(feed(frame(message(entry(16,7,{0x69,0x80,0,0,0,0,0,0,0})))) == SML_FINAL && smlNextEntry());
    assert(smlOBISDecode(value, scaler, SML_WATT) == SML_DECODE_OVERFLOW);
    assert(feed(frame(message(entry(16,7,{0x62,42},30)))) == SML_FINAL && smlNextEntry());
    assert(smlOBISDecode(value, scaler, SML_WATT) == SML_DECODE_UNIT);
    assert(feed(frame(message(entry(16,7,{1})))) == SML_FINAL && smlNextEntry());
    assert(smlOBISDecode(value, scaler, SML_WATT) == SML_DECODE_VALUE);
    // Optional nested valueTime must not shift unit/scaler/value positions.
    auto nested = entry(16,7,{0x62,42}); nested.erase(nested.begin()+9);
    nested.insert(nested.begin()+9, {0x72,0x62,1,0x65,0,0,0,7});
    assert(feed(frame(message(nested))) == SML_FINAL && smlNextEntry());
    assert(smlOBISDecode(value, scaler, SML_WATT) == SML_DECODE_OK && value == 42);
    assert(feed(frame(message(entry(16,7,{0x62,42},27,-2)))) == SML_FINAL && smlNextEntry());
    assert(smlOBISDecode(value, scaler, SML_WATT) == SML_DECODE_OK && scaler == -2);
    int64_t out;
    assert(meter::scale(-1234,-1,out) && out == -123400);
    assert(meter::scale(0,0,out) && out == 0);
    assert(!meter::scale(INT64_MAX,0,out));
    assert(meter::scale(INT64_MIN,-3,out) && out == INT64_MIN);
}
void transport_tests() {
    auto good = frame(message(entry(16,7,{0x62,42})));
    auto historical = message(entry(16,7,{0x62,42}));
    historical[10] = 3; // GetProfileList instead of GetList.Response
    assert(feed(frame(historical)) == SML_FINAL && !smlNextEntry());
    auto second = message(entry(16,7,{0x62,43})); second[16] = 'X';
    auto mixed = message(entry(16,7,{0x62,42})); append(mixed,second);
    assert(feed(frame(mixed)) == SML_UNEXPECTED && !smlNextEntry());
    auto bad = good; bad[20] ^= 1;
    assert(feed(bad) == SML_CHECKSUM_ERROR && !smlNextEntry());
    assert(feed(good,false) == SML_FINAL && smlNextEntry());
    assert(feed(frame({0x7f,1})) == SML_UNEXPECTED && !smlNextEntry());
    Bytes deep(20,0x71); deep.push_back(1);
    assert(feed(frame(deep)) == SML_UNEXPECTED);
    // Embedded escaped 1b*4 followed by start/end-like bytes is data.
    auto escaped = entry(96,1,{0x0d,27,27,27,27,1,1,1,1,27,27,27,27});
    assert(feed(frame(message(escaped))) == SML_FINAL && smlNextEntry());
    uint8_t identity[32]; size_t length;
    assert(smlOBISOctetString(identity,sizeof(identity),&length) && length == 12);
    auto escaped_end = entry(96,1,{0x0a,27,27,27,27,26,0,0,0,0});
    assert(feed(frame(message(escaped_end))) == SML_FINAL && smlNextEntry());
    // A fresh header replaces an interrupted frame; noise is harmless.
    Bytes interrupted(good.begin(),good.begin()+20);
    feed(interrupted); assert(feed(good,false) == SML_FINAL);
    feed(Bytes(9000,42)); assert(feed(good,false) == SML_FINAL);
    auto huge = good; huge.resize(9000,42); feed(huge);
    assert(feed(good,false) == SML_FINAL);
    // Every prefix, truncation, random byte mutation and arbitrary split is safe.
    for (size_t i = 0; i < good.size(); ++i) {
        feed(Bytes(good.begin(),good.begin()+i)); assert(!smlNextEntry());
        auto recovered = feed(good,false);
        // A truncation inside an escape run is indistinguishable until the
        // next delimiter. Recovery is bounded to the following complete frame.
        if (recovered != SML_FINAL) assert(feed(good,false) == SML_FINAL);
        auto changed = good; changed[i] ^= 0x80; feed(changed);
    }
    std::mt19937 rng(21);
    for (unsigned n=0; n<2000; ++n) {
        Bytes noise(rng()%300);
        for (auto &b: noise) b = rng();
        feed(frame(noise));
        assert(feed(good,false) == SML_FINAL);
    }
}
void mapping_tests() {
    meter::Tracker t;
    assert(!t.power.valid && t.power.age(0) == UINT32_MAX);
    meter::Frame f;
    f.total.offer(450000); f.imported.offer(10000000); f.exported.offer(2000000);
    t.commit(f,0); assert(t.power.fresh(29999,30000) && !t.power.fresh(30000,30000));
    assert(t.imported.fresh(119999,120000) && !t.imported.fresh(120000,120000));
    // Identical measurements refresh only their own clocks.
    meter::Frame energy; energy.imported.offer(10000000);
    t.commit(energy,25000); assert(!t.power.fresh(30000,30000) && t.imported.fresh(30000,120000));
    t.commit(f,30001); assert(t.power.fresh(30001,30000) && t.power.value == 450000);
    t.commit(f,31000); t.commit(f,32000); assert(t.confirmations == 3);
    t.commit(energy,33000); assert(t.power.received_ms == 32000);
    // Correctly signed export and a real zero remain valid.
    meter::Frame zero; zero.total.offer(0); t.commit(zero,34000); assert(t.power.value == 0);
    meter::Frame exported; exported.total.offer(-200000); t.commit(exported,35000); assert(t.power.value == -200000);
    // Missing export/phase data never means zero or a complete house.
    meter::Tracker pair;
    meter::Frame one; one.import_power.offer(100000); pair.commit(one,1); assert(!pair.power.valid);
    one.export_power.offer(200000); pair.commit(one,2); assert(pair.power.value == -100000 && pair.source == meter::Directional);
    meter::Frame phases; phases.phases[0].offer(100000); phases.phases[1].offer(200000);
    meter::Tracker three; three.commit(phases,1); assert(!three.power.valid);
    phases.phases[2].offer(-50000); three.commit(phases,2); assert(three.power.value == 250000);
    phases.total.offer(999000); three.commit(phases,3); three.commit(phases,4); three.commit(phases,5);
    assert(three.source == meter::Total16 && three.power.value == 999000);
    // A lower priority source cannot displace fresh total data.
    phases.total = {}; three.commit(phases,6); three.commit(phases,7); three.commit(phases,8);
    assert(three.source == meter::Total16);
    three.commit(phases,30006); assert(three.source == meter::ThreePhase);
    meter::Frame conflict; conflict.total.offer(1); conflict.total.offer(2); t.commit(conflict,36000);
    assert(t.power.value == -200000 && t.rejected > 0);
    meter::Frame wrong; wrong.imported.offer(-1); wrong.exported.offer(1999999); t.commit(wrong,40000);
    assert(t.imported.value == 10000000 && t.exported.value == 2000000);
    meter::Frame jump; jump.imported.offer(INT64_MAX); t.commit(jump,41000); assert(t.imported.value == 10000000);
    meter::Frame coarse; coarse.imported.offer(1000000,1000000);
    meter::Tracker quantized; quantized.commit(coarse,0); coarse.imported = {}; coarse.imported.offer(2000000,1000000);
    quantized.commit(coarse,1000); assert(quantized.imported.value == 2000000);
    t.reset_meter(); assert(!t.imported.valid && !t.power.valid && t.source == meter::None);
    wrong = {}; wrong.imported.offer(1); t.commit(wrong,42000); assert(t.imported.value == 1);
}
int main() { numeric_tests(); transport_tests(); mapping_tests(); }
