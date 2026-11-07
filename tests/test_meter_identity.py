"""Regression tests for the companion's conservative meter-ID presentation."""

import importlib.util
from pathlib import Path
import unittest

PATH = Path(__file__).resolve().parents[1] / "custom_components/smartmeter/meter_identity.py"
spec = importlib.util.spec_from_file_location("meter_identity", PATH)
identity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(identity)


class MeterIdentityTests(unittest.TestCase):
    def test_reported_lgz_id_matches_printed_identifier(self):
        self.assertEqual(identity.format_meter_id("0A014C475A0004D70208"), "1LGZ0081199624")
        self.assertEqual(identity.format_meter_id("0a014c475a0004d70208"), "1LGZ0081199624")

    def test_other_manufacturers_legacy_encoding_and_leading_zeroes(self):
        for raw, expected in (
            ("0A01454D480000B416CB", "1EMH0011802315"),
            ("09014553591100BC614E", "1ESY1112345678"),
            ("0A01484C590200000001", "1HLY0200000001"),
            ("0A01414243FE05F5E0FF", "1ABCFE99999999"),
            ("0A014142430000000000", "1ABC0000000000"),
        ):
            with self.subTest(raw=raw):
                self.assertEqual(identity.format_meter_id(raw), expected)

    def test_unknown_malformed_and_non_electricity_ids_are_preserved(self):
        for raw in (
            None, "", "1LGZ0081199624", "LGZ", "SIM", "12345678",
            "0A014C475A0004D702", "0A014C475A0004D7020800",
            "0A014C475A0004D7020G", "0A 01 4C 47 5A 00 04 D7 02 08",
            "0A014C475A0004D70208\n", "0A014C475A0004D70208 ",
            "0B014C475A0004D70208", "0A024C475A0004D70208",
            "0A014C675A0004D70208", "0A01FF475A0004D70208",
            "0A014C475AFF04D70208", "0A014C475A0005F5E100",
            "0A014C475A00FFFFFFFF",
        ):
            with self.subTest(raw=raw):
                self.assertEqual(identity.format_meter_id(raw), raw)
