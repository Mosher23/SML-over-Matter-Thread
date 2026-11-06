#!/usr/bin/env python3
"""Package prebuilt images; use the pinned SDK's official Matter OTA tool.

No ZIPs. No network writes. Refuse version/configuration inconsistencies.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("meter_ota", ROOT / "custom_components/smartmeter/ota.py")
ota = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = ota
spec.loader.exec_module(ota)


def package(source: Path, output: Path, *, ota_only=False, ota_build: Path | None = None):
    config = (source / "main/MatterProjConfig.h").read_text()
    version = int(re.search(r"#define CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION (\d+)", config)[1])
    string = re.search(r'#define CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION_STRING "([^"]+)"', config)[1]
    if version < ota.BOOTSTRAP_VERSION:
        raise ValueError("This packaging pipeline requires bootstrap v1.7 or later")
    if ota_only and version == ota.BOOTSTRAP_VERSION:
        raise ValueError("A USB-only firmware cannot be packaged as universal OTA")
    cmake = (source / "CMakeLists.txt").read_text()
    defaults = (source / "sdkconfig.defaults").read_text()
    if f'set(PROJECT_VER "{string}")' not in cmake or f"set(PROJECT_VER_NUMBER {version})" not in cmake or f"CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER={version}\n" not in defaults:
        raise ValueError("CMake, Matter identity and SDK version numbers disagree")
    if output.exists() and any(output.iterdir()):
        raise ValueError("Output folder must be new or empty (do not replace an existing release)")
    output.mkdir(parents=True, exist_ok=True)
    variants = ("ota",) if ota_only else ("internal", "external", "ota")
    for variant in variants:
        build = ota_build if variant == "ota" and ota_build else source / f"build-{variant}"
        sdk = (build / "config/sdkconfig.h").read_text()
        expected = ["#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1",
                    f"#define CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER {version}"]
        if variant == "ota":
            expected.append("#define CONFIG_SMARTMETER_REQUIRE_BOARD_CONFIG 1")
        else:
            expected.append(f"#define CONFIG_SMARTMETER_{variant.upper()}_ANTENNA 1")
            if "#define CONFIG_SMARTMETER_REQUIRE_BOARD_CONFIG 1" in sdk:
                raise ValueError("USB bootstrap must allow initial board configuration")
        if any(value not in sdk for value in expected):
            raise ValueError(f"Incorrect {variant} build flags")
        # The board provisioning tool relies on the SDK's legacy providers
        # reading chip-factory before their compile-time test defaults.
        provisioning_flags = (
            '#define CONFIG_CHIP_FACTORY_NAMESPACE_PARTITION_LABEL "nvs"',
            '#define CONFIG_EXAMPLE_COMMISSIONABLE_DATA_PROVIDER 1',
            '#define CONFIG_EXAMPLE_DEVICE_INSTANCE_INFO_PROVIDER 1',
        )
        if any(flag not in sdk for flag in provisioning_flags) or "#define CONFIG_NVS_ENCRYPTION 1" in sdk:
            raise ValueError(f"{variant} changed the provisioning provider; update the board flasher schema")
        description = json.loads((build / "project_description.json").read_text())
        if description["project_version"] != string:
            raise ValueError(f"Stale {variant} build")
        binary = build / "IR_Meter_Thread_Matter.bin"
        if binary.stat().st_size > ota.MAX_PAYLOAD:
            raise ValueError("Application exceeds OTA slot")
        if variant != "ota":
            shutil.copyfile(binary, output / f"IR_Power_Meter_{variant}.bin")
        if variant == "external":
            for relative, name in (("bootloader/bootloader.bin", "bootloader.bin"),
                                   ("partition_table/partition-table.bin", "partition-table.bin"),
                                   ("ota_data_initial.bin", "ota_data_initial.bin")):
                shutil.copyfile(build / relative, output / name)
        elif variant == "internal":
            # Compare flash layout and initial OTA selection. Bootloader .bin
            # embeds build timestamps, so two correct builds differ in bytes.
            for relative in ("partition_table/partition-table.bin", "ota_data_initial.bin"):
                if (build / relative).read_bytes() != (source / "build-external" / relative).read_bytes():
                    raise ValueError(f"Antenna variants disagree on {relative}")
    tool = Path(os.environ["ESP_MATTER_PATH"]) / "connectedhomeip/connectedhomeip/src/app/ota_image_tool.py"
    tag = f"v{string}"
    notes = f"https://github.com/{ota.REPOSITORY}/releases/tag/{tag}"
    if version == ota.BOOTSTRAP_VERSION:
        # This migration changes the bootloader and partition table: USB only.
        manifest = {"schema": 1, "delivery": "usb", "vendor_id": ota.VID,
                    "product_id": ota.PID, "hardware_version": 1,
                    "software_version": version, "version_string": string}
        ota.parse_manifest(manifest, tag, set())
        finish(output, manifest)
        return
    image = output / ota.OTA_FILENAME
    maximum = version - 1
    subprocess.run([sys.executable, str(tool), "create", "-v", str(ota.VID), "-p", str(ota.PID),
                    "-vn", str(version), "-vs", string, "-da", "sha256",
                    "-mi", str(ota.BOOTSTRAP_VERSION), "-ma", str(maximum), "-rn", notes,
                    str((ota_build or source / "build-ota") / "IR_Meter_Thread_Matter.bin"), str(image)], check=True)
    data = image.read_bytes()
    manifest = {"schema": 1, "vendor_id": ota.VID, "product_id": ota.PID,
                "hardware_version": 1, "software_version": version, "version_string": string,
                "min_version": ota.BOOTSTRAP_VERSION, "max_version": maximum,
                "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                "url": f"https://github.com/{ota.REPOSITORY}/releases/download/{tag}/{image.name}"}
    release = ota.parse_manifest(manifest, tag, {manifest["url"]})
    ota.validate_image(data, release)
    finish(output, manifest, include_usb=not ota_only)


def finish(output, manifest, *, include_usb=True):
    (output / "ota-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    provisioning = {"schema": 1, "chip": "esp32c6", "partition": "nvs",
                    "namespace": "chip-factory", "provider": "esp32-legacy",
                    "version_string": manifest["version_string"]}
    (output / "provisioning.json").write_text(json.dumps(provisioning, indent=2) + "\n")
    if include_usb:
        shutil.copyfile(ROOT / "scripts/flash.py", output / "flash.py")
    checksums = [f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}"
                 for path in sorted(output.iterdir()) if path.is_file()]
    (output / "SHA256SUMS").write_text("\n".join(checksums) + "\n")
    print(f"Validated release folder: {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=ROOT / "firmware")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ota-only", action="store_true",
                        help="Package only the universal OTA build; no USB builds are required")
    parser.add_argument("--ota-build", type=Path, help="Use an existing universal build directory")
    args = parser.parse_args()
    package(args.source.resolve(), args.output.resolve(), ota_only=args.ota_only,
            ota_build=args.ota_build.resolve() if args.ota_build else None)
