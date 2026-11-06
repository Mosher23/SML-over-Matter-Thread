/* Bounded SML transport and TL decoder. Legacy OBIS convenience API retained.
 * The convenience API originated in https://github.com/olliiiver/sml_parser
 * Licensed under LGPL-2.1: https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html
 * Legacy helper modifications by Claus Muus, 01.08.2025.
 * Every entry is exposed only after transport CRC and the complete TL tree pass.
 */
#include "sml.h"
#include "smlCrcTable.h"
#include <stdint.h>
#include <limits.h>
#include <string.h>

namespace {
constexpr size_t kMaxFrame = 8192, kMaxEntries = 128, kMaxDepth = 16;
uint8_t wire[kMaxFrame], payload[kMaxFrame];
size_t wireSize = 0, payloadSize = 0, entryCount = 0, entryCursor = 0;
size_t entries[kMaxEntries];
size_t validatedServerOffset = 0, validatedServerSize = 0;
uint8_t startMatch = 0;
bool receiving = false;
size_t escapeRun = 0;
unsigned trailer = 0;
constexpr uint8_t start[] = {27,27,27,27,1,1,1,1};
struct Token { unsigned type; size_t count, data, next; };

bool token(size_t offset, Token &t)
{
  if (offset >= payloadSize) return false;
  const uint8_t first = payload[offset++];
  if (first == 0) { t = {0, 0, offset, offset}; return true; } // endOfSmlMsg
  t.type = (first >> 4) & 7;
  size_t length = first & 15, header = 1;
  uint8_t part = first;
  while (part & 128) {
    if (offset >= payloadSize || ++header > 4) return false;
    part = payload[offset++];
    if (part & 0x70) return false;
    length = (length << 4) | (part & 15);
  }
  if (t.type == 7) { t = {7, length, offset, offset}; return length <= kMaxFrame; }
  if (length < header || (t.type != 0 && t.type != 4 && t.type != 5 && t.type != 6)) return false;
  const size_t size = length - header;
  if (size > payloadSize - offset ||
      ((t.type == 5 || t.type == 6) && (size == 0 || size > 8)) ||
      (t.type == 4 && size != 1)) return false;
  t = {t.type, size, offset, offset + size};
  return true;
}

bool walk(size_t &offset, unsigned depth)
{
  if (depth > kMaxDepth) return false;
  Token t;
  if (!token(offset, t)) return false;
  offset = t.next;
  if (t.type != 7) return true;
  for (size_t i = 0; i < t.count; ++i) if (!walk(offset, depth + 1)) return false;
  return true;
}

// Extract only current GetList.Response values. Historical/profile responses
// may contain OBIS-looking lists, but must never refresh live measurements.
bool collect_response(const Token &message, size_t &server_offset, size_t &server_size)
{
  size_t offset = message.data;
  for (unsigned i = 0; i < 3; ++i) if (!walk(offset, 0)) return false;
  Token body, tag;
  if (!token(offset, body) || body.type != 7 || body.count != 2 || !token(body.data, tag) || tag.type != 6) return false;
  uint64_t message_type = 0;
  for (size_t i = 0; i < tag.count; ++i) message_type = (message_type << 8) | payload[tag.data + i];
  if (message_type != 0x0701) return true;
  Token response;
  if (!token(tag.next, response) || response.type != 7 || response.count != 7) return false;
  offset = response.data;
  if (!walk(offset, 0)) return false; // clientId
  Token server;
  if (!token(offset, server) || server.type != 0 || server.count == 0) return false;
  if (server_size && (server_size != server.count || memcmp(payload + server_offset, payload + server.data, server_size))) return false;
  server_offset = server.data; server_size = server.count;
  for (unsigned i = 0; i < 3; ++i) if (!walk(offset, 0)) return false; // serverId, listName, actSensorTime
  Token values;
  if (!token(offset, values) || values.type != 7) return false;
  offset = values.data;
  for (size_t i = 0; i < values.count; ++i) {
    Token entry, identifier;
    if (!token(offset, entry) || entry.type != 7 || entry.count != 7 ||
        !token(entry.data, identifier) || identifier.type != 0 || identifier.count != 6 || entryCount == kMaxEntries) return false;
    entries[entryCount++] = offset;
    if (!walk(offset, 0)) return false;
  }
  return true;
}

sml_states_t finish_frame()
{
  const size_t size = wireSize;
  receiving = false; wireSize = 0; entryCount = entryCursor = validatedServerSize = 0;
  uint16_t crc = 0xffff;
  for (size_t i = 0; i < size - 2; ++i) crc = smlCrcTable[(wire[i] ^ crc) & 255] ^ (crc >> 8);
  crc ^= 0xffff;
  if (wire[size-2] != (crc & 255) || wire[size-1] != (crc >> 8)) return SML_CHECKSUM_ERROR;
  const size_t padding = wire[size-3];
  if (padding > 3 || size < 16 + padding) return SML_UNEXPECTED;
  const size_t end = size - 8;
  for (size_t i = end - padding; i < end; ++i) if (wire[i]) return SML_UNEXPECTED;
  payloadSize = 0;
  for (size_t i = 8; i < end - padding;) {
    if (i + 4 <= end - padding && memcmp(wire + i, start, 4) == 0) {
      if (i + 8 > end - padding || memcmp(wire + i + 4, start, 4)) return SML_UNEXPECTED;
      memcpy(payload + payloadSize, start, 4); payloadSize += 4; i += 8;
    } else payload[payloadSize++] = wire[i++];
  }
  size_t offset = 0, server_offset = 0, server_size = 0;
  while (offset < payloadSize) {
    Token message;
    if (!token(offset, message) || message.type != 7 || message.count != 6) {
      entryCount = 0; return SML_UNEXPECTED;
    }
    if (!walk(offset, 0) || !collect_response(message, server_offset, server_size)) { entryCount = 0; return SML_UNEXPECTED; }
    // SML messages end with the special 0x00 endOfSmlMsg marker.
    if (payload[offset - 1] != 0) { entryCount = 0; return SML_UNEXPECTED; }
  }
  if (payloadSize == 0) return SML_UNEXPECTED;
  validatedServerOffset = server_offset; validatedServerSize = server_size;
  return SML_FINAL;
}
} // namespace

// Normalised fields for the compatibility helpers (length, type, data).
static unsigned char listBuffer[512];
static size_t listPos = 0;
static signed char sc;

void smlReset()
{
  wireSize = payloadSize = entryCount = entryCursor = listPos = startMatch = 0;
  receiving = false; escapeRun = 0; trailer = 0; validatedServerSize = 0;
}

sml_states_t smlState(unsigned char &byte)
{
  // Escaped payloads contain eight 0x1b bytes, never a genuine start delimiter.
  // A fresh header therefore also resynchronises a truncated transmission.
  if (byte == start[startMatch]) ++startMatch;
  else startMatch = byte == 27 ? (startMatch < 4 ? startMatch + 1 : 4) : 0;
  if (startMatch == 8) {
    // Ignore a start-like sequence inside an escaped run of eight 0x1b bytes.
    size_t run = 0;
    if (receiving && wireSize >= 3) {
      size_t i = wireSize - 3;
      while (i && wire[i - 1] == 27) { ++run; --i; }
    }
    const bool escaped = receiving && run >= 8 && run % 8 == 0;
    startMatch = 0;
    if (!escaped) {
      memcpy(wire, start, 8); wireSize = 8; receiving = true; escapeRun = trailer = 0;
      entryCount = entryCursor = listPos = validatedServerSize = 0;
      return SML_VERSION;
    }
  }
  if (!receiving) return SML_NEXT;
  if (wireSize == kMaxFrame) { smlReset(); return SML_UNEXPECTED; }
  wire[wireSize++] = byte;
  if (trailer) {
    if (--trailer == 0) return finish_frame();
  } else if (byte == 0x1a && escapeRun % 8 == 4) trailer = 3;
  if (byte == 27) ++escapeRun; else escapeRun = 0;
  return SML_NEXT;
}

bool smlServerId(unsigned char *output, size_t capacity, size_t *length)
{
  if (!output || !length || !validatedServerSize || validatedServerSize > capacity) return false;
  memcpy(output, payload + validatedServerOffset, validatedServerSize);
  *length = validatedServerSize;
  return true;
}

bool smlNextEntry()
{
  while (entryCursor < entryCount) {
    listPos = 0;
    Token root; token(entries[entryCursor++], root);
    size_t offset = root.data, remaining = root.count;
    bool fits = true;
    while (remaining) {
      Token t;
      if (!token(offset, t) || t.count > 255) { fits = false; break; }
      --remaining; offset = t.next;
      const auto type = t.type == 7 ? SML_LISTSTART : t.type == 5 ? SML_DATA_SIGNED_INT :
          t.type == 6 ? SML_DATA_UNSIGNED_INT : t.type == 0 && t.count ? SML_DATA_OCTET_STRING : SML_NEXT;
      const size_t bytes = t.type == 7 ? 0 : t.count;
      if (listPos + 2 + bytes > sizeof(listBuffer)) { fits = false; break; }
      listBuffer[listPos++] = t.count; listBuffer[listPos++] = type;
      memcpy(listBuffer + listPos, payload + t.data, bytes); listPos += bytes;
      if (t.type == 7) remaining += t.count;
    }
    if (fits) return true;
  }
  listPos = 0;
  return false;
}

bool smlOBISCheck(const unsigned char *obis)
{
  return listPos >= 8 && listBuffer[0] == 6 &&
         listBuffer[1] == SML_DATA_OCTET_STRING &&
         memcmp(obis, &listBuffer[2], 6) == 0;
}

bool smlOBISOctetString(unsigned char *output, size_t capacity, size_t *length)
{
  if (output == nullptr || length == nullptr) {
    return false;
  }

  size_t offset = 0;
  unsigned position = 0;
  while (offset + 2 <= listPos) {
    const size_t size = listBuffer[offset++];
    const sml_states_t type = static_cast<sml_states_t>(listBuffer[offset++]);
    ++position;

    if (type == SML_LISTSTART) {
      size_t remaining = size;
      while (remaining) {
        if (offset + 2 > listPos) return false;
        size_t child_size = listBuffer[offset++];
        auto child_type = static_cast<sml_states_t>(listBuffer[offset++]);
        --remaining;
        if (child_type == SML_LISTSTART) remaining += child_size;
        else {
          if (child_size > listPos - offset) return false;
          offset += child_size;
        }
      }
      continue;
    }

    if (size > listPos - offset) {
      return false;
    }
    if (position == 6) {
      if (type != SML_DATA_OCTET_STRING || size == 0 || size > capacity) {
        return false;
      }
      memcpy(output, &listBuffer[offset], size);
      *length = size;
      return true;
    }
    offset += size;
  }
  return false;
}

void smlOBISManufacturer(unsigned char *str, int maxSize)
{
  if (!str || maxSize <= 0) return;
  size_t length = 0;
  if (!smlOBISOctetString(str, static_cast<size_t>(maxSize - 1), &length)) length = 0;
  str[length] = 0;
}

void smlPow(double &val, signed char &scaler)
{
  if (scaler < 0) {
    while (scaler++) {
      val /= 10;
    }
  }
  else {
    while (scaler--) {
      val *= 10;
    }
  }
}

// Return a typed result: -1 is a legitimate signed power measurement.
sml_decode_result_t smlOBISDecode(long long &value, signed char &scaler, sml_units_t unit)
{
  struct Field { size_t offset, size; sml_states_t type; } fields[6] = {};
  size_t offset = 0;
  for (unsigned position = 0; position < 6; ++position) {
    if (offset + 2 > listPos) return SML_DECODE_MALFORMED;
    auto &field = fields[position];
    field.size = listBuffer[offset++];
    field.type = static_cast<sml_states_t>(listBuffer[offset++]);
    field.offset = offset;
    if (field.type == SML_LISTSTART) {
      // The optional valueTime may contain nested lists.
      size_t remaining = field.size;
      while (remaining) {
        if (offset + 2 > listPos) return SML_DECODE_MALFORMED;
        size_t size = listBuffer[offset++];
        auto type = static_cast<sml_states_t>(listBuffer[offset++]);
        --remaining;
        if (type == SML_LISTSTART) remaining += size;
        else {
          if (size > listPos - offset) return SML_DECODE_MALFORMED;
          offset += size;
        }
      }
    } else {
      if (field.size > listPos - offset) return SML_DECODE_MALFORMED;
      offset += field.size;
    }
  }
  const auto &u = fields[3]; const auto &s = fields[4]; const auto &v = fields[5];
  if (u.type != SML_DATA_UNSIGNED_INT || u.size != 1 ||
      (unit != SML_IGNORE && listBuffer[u.offset] != unit)) return SML_DECODE_UNIT;
  if (s.type != SML_DATA_SIGNED_INT || s.size != 1) return SML_DECODE_SCALER;
  if ((v.type != SML_DATA_SIGNED_INT && v.type != SML_DATA_UNSIGNED_INT) ||
      v.size == 0 || v.size > 8) return SML_DECODE_VALUE;
  uint64_t bits = 0;
  for (size_t i = 0; i < v.size; ++i) bits = (bits << 8) | listBuffer[v.offset + i];
  const bool negative = v.type == SML_DATA_SIGNED_INT && (listBuffer[v.offset] & 0x80);
  if (!negative && bits > INT64_MAX) return SML_DECODE_OVERFLOW;
  if (negative && v.size < 8) bits |= UINT64_MAX << (v.size * 8);
  // memcpy preserves the two's-complement bit pattern without signed shifts.
  static_assert(sizeof(value) == sizeof(bits), "64-bit SML values required");
  memcpy(&value, &bits, sizeof(value));
  const uint8_t raw_scaler = listBuffer[s.offset];
  scaler = raw_scaler < 128 ? raw_scaler : static_cast<int>(raw_scaler) - 256;
  return SML_DECODE_OK;
}

void smlOBISByUnit(long long &value, signed char &scaler, sml_units_t unit)
{
  if (smlOBISDecode(value, scaler, unit) != SML_DECODE_OK) { value = -1; scaler = 0; }
}

void smlOBISValue(long long int &value)
{
  long long int val;
  smlOBISByUnit(val, sc, SML_IGNORE);
  double dval = val;
  smlPow(dval, sc);
  value = (long long int)dval;
}

void smlOBISWh(double &wh)
{
  long long int val;
  smlOBISByUnit(val, sc, SML_WATT_HOUR);
  wh = val;
  smlPow(wh, sc);
}

void smlOBISW(double &w)
{
  long long int val;
  smlOBISByUnit(val, sc, SML_WATT);
  w = val;
  smlPow(w, sc);
}

void smlOBISVolt(double &v)
{
  long long int val;
  smlOBISByUnit(val, sc, SML_VOLT);
  v = val;
  smlPow(v, sc);
}

void smlOBISAmpere(double &a)
{
  long long int val;
  smlOBISByUnit(val, sc, SML_AMPERE);
  a = val;
  smlPow(a, sc);
}

void smlOBISHertz(double &h)
{
  long long int val;
  smlOBISByUnit(val, sc, SML_HERTZ);
  h = val;
  smlPow(h, sc);
}

void smlOBISDegree(double &d)
{
  long long int val;
  smlOBISByUnit(val, sc, SML_DEGREE);
  d = val;
  smlPow(d, sc);
}
