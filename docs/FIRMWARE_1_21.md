# Firmware 1.21: SML reliability

Firmware **1.21**, Matter software version **25**, is one universal build for
both saved antenna settings. The original endpoint 1 Electrical Meter, native
power/energy clusters, antenna control, BOOT gestures, LED confirmation,
commissioning identity, and partition layout are preserved. No second outlet
or energy endpoint is introduced. Existing pairings survive OTA.

## Validated decoding

A bounded 8 KiB SML file buffer verifies transport CRC-16/X25, padding, escaped
bytes, TL lengths, complete message trees, nesting and entry limits before any
measurement can be committed. Only current GetList.Response values are read;
historical/profile responses cannot refresh measurements. Mixed server IDs
within one file are rejected. A partial telegram cannot update a reading.
UART overflow/framing errors discard the partial file. Reception also resets
a partial decoder after two seconds of silence or an optical transmission.

Numeric fields require the correct unsigned unit, signed one-byte scaler and
signed/unsigned value of 1–8 bytes. Nulls, malformed fields, unsigned values
outside int64, scaling overflow and conflicting duplicate fields are rejected.
Negative power and a measured zero are valid. The old `-1` decoding error
sentinel is no longer used for measurements.

Power is limited to the configured installation capacity (default **1 MW** in
either direction). Energy must be nonnegative and nondecreasing. Counter
increases are bounded by that capacity, elapsed time and register resolution.
A decrease is rejected; it is not silently treated as a rollover or reset.
A changed meter ID must repeat in three valid frames before replacing the
identity and resetting measurement state. The initial counter has no earlier
reference and is accepted after numeric validation.

## Automatic mapping profiles

These profiles describe observable OBIS layouts, not an inferred manufacturer
model. The first supported layout is provisional; three consecutive complete
frames confirm its mapping. A fresh preferred source is retained when a
partial frame omits it. A fallback must repeat in three frames and the previous
source must be stale before a lower-priority mapping replaces it.

| Priority | Power profile | Requirements |
| --- | --- | --- |
| 1 | SML signed total | `1-0:16.7.0*255`, unit W and its transmitted scaler |
| 2 | SML directional power pair | `1-0:1.7.0*255` minus `1-0:2.7.0*255`; both nonnegative and in the same file |
| 3 | SML three-phase signed sum | All of `1-0:36.7.0*255`, `1-0:56.7.0*255`, `1-0:76.7.0*255`, in the same file |

Cumulative import uses **`1-0:1.8.0*255`** and export uses
**`1-0:2.8.0*255`**, unit Wh and the meter's scaler. Totals are never added to
tariff registers. A missing direction is unknown, not zero.

Meter ID prefers `1-0:96.1.0*255`, then `0-0:96.1.0*255`,
`0-0:96.1.255*255`, and EMH's `1-0:0.0.9*255`. If all are absent, the validated SML
GetList.Response `serverId` is used as a clearly identified fallback. Lower-priority aliases cannot
replace a recently received preferred identity. Printable octet strings retain
their text; binary IDs retain their uppercase hex representation. The selected
identifier is not guaranteed to equal the printed billing serial.

Manufacturer accepts a three-letter identifier from `129-129:199.130.3*255`
or, as fallback, `1-0:96.50.1*1`. Nonmanufacturer-looking revision values in the
latter are ignored. Manufacturer alone does not establish an exact meter model.

**Deliberately unsupported:** unsigned absolute power `15.7.0`, historical or
tariff-specific power, a single phase presented as whole-home power, tariff
sums without a verified profile, encrypted SML, and D0/IEC 62056-21 ASCII or
DLMS protocols. Those need separate documented profiles/protocol support;
firmware never guesses missing directions or phases.

## Freshness and recovery

Each measurement has its own 64-bit monotonic receive time. Only its own valid
field in a CRC-valid file refreshes that time, including an unchanged value.
Other readings and damaged files cannot keep it fresh. Timeouts are checked on
every reader loop, including silence and optical PIN entry.

| Reading | Default timeout | On expiry |
| --- | --- | --- |
| Power | 30 seconds | Standard Matter ActivePower becomes null/unknown |
| Imported energy | 120 seconds | Last counter retained; diagnostic status becomes stale |
| Exported energy | 120 seconds | Last counter retained; diagnostic status becomes stale |
| Meter identity | Static | Retained until a validated identity replacement or restart |

Recovery publishes power immediately, even if its numerical value equals the
last value before expiry. Energy reports retain their actual measurement time,
rather than acquiring a new timestamp when another counter is reported.
One coalesced Matter work item prevents queue growth. Failed scheduling or
attribute updates retry without acknowledging unpublished measurements.

`menuconfig → Smart-meter reader` exposes the two timeouts and installation
maximum power. The universal build uses the defaults above. These are build
settings, not remotely writable limits.

## Home Assistant companion 2.1.7

Install companion **2.1.7** for **Power/Imported energy/Exported energy
status and reading age**, **Meter profile**, **Profile detection**, individual
**OBIS sources**, **Reader health**, **Valid SML frame age**, frame/error
counters, and **Last validation error**. Diagnostic attributes 5–24 are
additive in cluster `0xFFF1FC01`; existing attributes 0–4 retain their IDs.
Companion 2.1.7 enables seven basic diagnostics by default. Enable the additional
status, age, profile and error entities in HA's entity settings for testing.

`OBIS Received` remains an ever-received latch for power during this boot,
not a freshness flag. Use **Power status** for freshness. Error counters and
the last error are since boot; an old error remains visible after recovery.
Reader health distinguishes silence, UART traffic without valid files, missing
power, active optical commands, and normal receiving. “Check INFO/PIN” is a
suggestion, not automatic proof that the meter requires a PIN.

Diagnostic ages are sampled at least every five seconds and polled by the
companion every thirty seconds; they are not a continuously ticking HA clock.
Older firmware keeps its existing companion functions and returns unknown for
new diagnostics. Native Matter power/energy do not depend on the companion.
Apple Home's exact display of a null power value is controller-dependent.

## Verification and remaining hardware test

Host tests exercise the real parser and publisher under AddressSanitizer and
UndefinedBehaviorSanitizer: CRC rejection, malformed lengths, deep nesting,
escaping, interrupted streams, deterministic fuzz input, signedness, units,
scalers, overflow, zero/export power, mapping priority, incomplete phase/pair
rejection, independent virtual-clock timeouts, same-value recovery, counter
validation, delayed publication and failed-queue retries. Existing antenna,
BOOT/LED and OTA/companion tests remain in the suite.

Public telegrams from the [libsml-testing capture collection](https://github.com/devZer0/libsml-testing)
were replayed locally for DZG DVS-7420 negative power, EMH ED300L import/export,
EMH eHZ361L5R, EasyMeter Q3A A1064V1009, Holley DTZ541-BDBA, Iskra MT631-D2A51
and Itron OpenWay-3.HZ. This checks parsing of those captures; it does not claim
physical testing of those meters or support for every register in them.
Some captures contain ambiguous legacy power mappings; these remain unknown.
The EasyMeter capture also contains three CRC-invalid files, which are rejected.
External captures are not bundled in this repository.

Mapping references include [Tasmota's meter descriptors](https://tasmota.github.io/docs/Smart-Meter-Interface/),
the [EMH ED300L manufacturer manual](https://www.celle-uelzennetz.de/downloads/cun/bedienungsanleitung_ed300l.pdf)
and [DZG WS76 manual](https://www.dzg.de/fileadmin/dzg/content/downloads/produkte-zaehler/ws76/DZG_WS76_Handbuch_250714.pdf).
They document mappings and limits; a model-specific rule requires its own
validated telegram before being added.

After installing on a test board, keep the IR reader attached and verify native
power, import/export counters and profile. Disconnect only the IR data/head for
35 seconds: power must become unknown, without publishing a synthetic zero.
After 125 seconds, energy status must be stale while the last counters remain.
Reconnect: values and statuses should recover without a reset or re-pairing.
On 6 November 2026, HA MCP history verified reception on the user's EMH meter:
190–205 W power, a continuously increasing import counter, a confirmed signed-total
mapping, and 140 valid frames with zero reported decoding, CRC, UART or rejected
reading errors. After IR data stopped, native power became unknown and imported
energy became stale while retaining its counter. A subsequent restart correctly
returned unreceived readings to unknown. Reconnection recovery, export readings
and long-term stability remain unverified on this hardware.
