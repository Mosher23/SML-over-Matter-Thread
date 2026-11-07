"""Format recognized binary electricity-meter identifiers for display."""

from __future__ import annotations

import re


def format_meter_id(value: str | None) -> str | None:
    """Render DIN 43863-5 IDs; preserve unknown or already readable values.

    The firmware sends binary octet strings as hexadecimal text. Recognize
    only the ten-byte 09/0A encoding with electricity sector 01, three ASCII
    manufacturer letters, a production block (00..FE), and a big-endian
    serial fitting the standard's eight decimal digits. The production block
    stays hexadecimal; only the serial is converted to decimal.
    """
    if value is None or re.fullmatch(r"[0-9A-Fa-f]{20}", value) is None:
        return value
    raw = bytes.fromhex(value)
    if (
        raw[0] not in (0x09, 0x0A)
        or raw[1] != 0x01
        or not all(ord("A") <= byte <= ord("Z") for byte in raw[2:5])
        or raw[5] == 0xFF
    ):
        return value
    serial = int.from_bytes(raw[6:10], "big")
    if serial > 99_999_999:
        return value
    return f"1{raw[2:5].decode('ascii')}{raw[5]:02X}{serial:08d}"
