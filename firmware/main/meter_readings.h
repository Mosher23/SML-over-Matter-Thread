#pragma once
// Pure, allocation-free validation and mapping shared by firmware and host tests.
#include <stdint.h>
#include <limits.h>

namespace meter {
enum Source : uint8_t { None, Total16, Directional, ThreePhase };
inline const char *source_name(Source s) {
    switch (s) {
    case Total16: return "1-0:16.7.0*255";
    case Directional: return "1.7.0 minus 2.7.0";
    case ThreePhase: return "36.7.0+56.7.0+76.7.0";
    default: return "none";
    }
}
inline bool scale(int64_t raw, int8_t scaler, int64_t &out) {
    out = raw;
    int exponent = int(scaler) + 3;
    while (exponent > 0) {
        if (out > INT64_MAX / 10 || out < INT64_MIN / 10) return false;
        out *= 10; --exponent;
    }
    while (exponent++ < 0) out /= 10;
    return true;
}
inline bool add(int64_t a, int64_t b, int64_t &out) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return false;
    out = a + b; return true;
}
struct Candidate {
    bool valid = false, conflict = false;
    int64_t value = 0, quantum = 1;
    void offer(int64_t next, int64_t resolution = 1) {
        if (valid && next != value) conflict = true;
        value = next; quantum = resolution; valid = !conflict;
    }
};
struct Frame {
    Candidate total, import_power, export_power, phases[3], imported, exported;
    Source power(int64_t &value) const {
        // A malformed/conflicting preferred source is not silently replaced.
        if (total.conflict) return None;
        if (total.valid) { value = total.value; return Total16; }
        if (import_power.valid && export_power.valid && import_power.value >= 0 && export_power.value >= 0) {
            value = import_power.value - export_power.value; return Directional;
        }
        int64_t partial;
        if (phases[0].valid && phases[1].valid && phases[2].valid &&
            add(phases[0].value, phases[1].value, partial) && add(partial, phases[2].value, value)) return ThreePhase;
        return None;
    }
};
struct Reading {
    bool valid = false;
    int64_t value = 0, quantum = 1;
    uint64_t received_ms = 0;
    uint32_t revision = 0;
    bool fresh(uint64_t now, uint64_t timeout) const { return valid && now - received_ms < timeout; }
    uint32_t age(uint64_t now) const {
        if (!valid) return UINT32_MAX;
        uint64_t seconds = (now - received_ms) / 1000;
        return seconds < UINT32_MAX ? seconds : UINT32_MAX - 1;
    }
    void receive(const Candidate &c, uint64_t now) {
        value = c.value; quantum = c.quantum; received_ms = now; valid = true; ++revision;
    }
};
struct Tracker {
    Reading power, imported, exported;
    Source source = None, proposed = None;
    unsigned confirmations = 0;
    bool source_confirmed = false;
    uint32_t rejected = 0;
    const char *last_error = "none";
    uint64_t power_timeout_ms = 30000, energy_timeout_ms = 120000;
    int64_t max_power_mw = 1000000000LL; // 1 MW, configurable for the installation.

    void reset_meter() {
        // Never carry a previous meter's counter into a newly identified meter.
        power = {}; imported = {}; exported = {}; source = proposed = None; confirmations = 0; source_confirmed = false;
    }
    bool energy(const Candidate &c, Reading &r, uint64_t now) {
        if (!c.valid) { if (c.conflict) reject("conflicting duplicate OBIS"); return false; }
        if (c.value < 0) { reject("negative cumulative energy"); return false; }
        if (r.valid) {
            if (c.value < r.value) { reject("energy counter decreased"); return false; }
            // Bound the increase by installation capacity plus both register
            // resolutions; coarse whole-kWh registers must remain usable.
            uint64_t elapsed = now - r.received_ms;
            uint64_t allowance = static_cast<uint64_t>(max_power_mw) / 3600 * (elapsed / 1000 + 1);
            uint64_t quantization = static_cast<uint64_t>(r.quantum) + c.quantum;
            const uint64_t limit = allowance > UINT64_MAX - quantization ? UINT64_MAX : allowance + quantization;
            if (static_cast<uint64_t>(c.value - r.value) > limit) {
                reject("implausible energy increase"); return false;
            }
        }
        r.receive(c, now); return true;
    }
    void reject(const char *reason) { if (rejected != UINT32_MAX) ++rejected; last_error = reason; }
    void commit(const Frame &frame, uint64_t now) {
        int64_t value = 0;
        Source next = frame.power(value);
        if (next != None && (value > max_power_mw || value < -max_power_mw)) {
            reject("power exceeds configured capacity"); next = None;
        }
        if (frame.total.conflict) reject("conflicting duplicate OBIS");
        if (next != None) {
            if (next == proposed) { if (confirmations < 3) ++confirmations; }
            else { proposed = next; confirmations = 1; }
            // Preserve a fresh preferred source across partial frames. A new
            // fallback must repeat coherently in three valid complete frames.
            if (source == None || next == source || (confirmations >= 3 &&
                (next < source || !power.fresh(now, power_timeout_ms)))) {
                if (next != source) source_confirmed = false;
                source = next; source_confirmed = source_confirmed || confirmations >= 3; Candidate c; c.offer(value); power.receive(c, now);
            }
        } else { proposed = None; confirmations = 0; }
        energy(frame.imported, imported, now); energy(frame.exported, exported, now);
    }
    const char *profile() const {
        switch (source) {
        case Total16: return "SML signed total";
        case Directional: return "SML directional power pair";
        case ThreePhase: return "SML three-phase signed sum";
        default: return "generic SML / automatic OBIS";
        }
    }
    const char *detection() const {
        if (source == None) return "awaiting supported power OBIS";
        return source_confirmed ? "mapping confirmed" : "mapping provisional";
    }
};
} // namespace meter
