import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import firmware


def make_image(chip=9, board="v2"):
    header = bytearray(24)
    header[0] = 0xE9
    header[1] = 1
    header[2] = 2
    header[3] = 0x40 if board == "v2" else 0x20
    header[8] = 0xEE
    header[23] = 1
    struct.pack_into("<H", header, 12, chip)
    descriptor = bytearray(256)
    struct.pack_into("<I", descriptor, 0, 0xABCD5432)
    descriptor[16:22] = b"2.0.0\0"
    project = f"esp32_div_{board}".encode() + b"\0"
    descriptor[48:48 + len(project)] = project
    descriptor[112:119] = b"v5.5.5\0"
    image = header + struct.pack("<II", 0x3FC88000, len(descriptor)) + descriptor
    checksum = 0xEF
    for byte in descriptor:
        checksum ^= byte
    image.extend(b"\0" * ((15 - len(image)) % 16))
    image.append(checksum)
    image.extend(hashlib.sha256(image).digest())
    return image


def make_table():
    entries = [(1, 2, 0x9000, 0x5000, b"nvs"), (1, 0, 0xE000, 0x2000, b"otadata"),
               (0, 0x10, 0x10000, 0x400000, b"ota_0"), (0, 0x11, 0x410000, 0x400000, b"ota_1")]
    return b"".join(struct.pack("<HBBII16sI", 0x50AA, *p, 0) for p in entries) + b"\xff" * 32


class FirmwareTests(unittest.TestCase):
    def setUp(self):
        work = firmware.ROOT / ".work"
        work.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix="firmware-test-", dir=work)
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name).resolve()
        self.assertTrue(self.directory.is_relative_to(work.resolve()))
        self.args = {
            "extra_esptool_args": {"chip": "esp32s3"},
            "flash_settings": {"flash_size": "16MB", "flash_mode": "dio", "flash_freq": "40m"},
            "write_flash_args": ["--flash-mode", "dio", "--flash-size", "16MB", "--flash-freq", "40m"],
            "bootloader": {"file": "bootloader.bin", "offset": "0x0"},
            "partition-table": {"file": "partitions.bin", "offset": "0x8000"},
            "otadata": {"file": "ota_data_initial.bin", "offset": "0xe000"},
            "app": {"file": "app.bin", "offset": "0x10000"},
            "flash_files": {"0x0": "bootloader.bin", "0x8000": "partitions.bin", "0xe000": "ota_data_initial.bin", "0x10000": "app.bin"},
        }
        (self.directory / "bootloader.bin").write_bytes(make_image())
        (self.directory / "app.bin").write_bytes(make_image())
        (self.directory / "partitions.bin").write_bytes(make_table())
        (self.directory / "ota_data_initial.bin").write_bytes(b"\xff" * 8192)
        firmware.write_json(self.directory / "flasher_args.json", self.args)

    def test_valid_build(self):
        info = firmware.validate_layout("v2", self.directory, self.args)
        self.assertEqual(info["project"], "esp32_div_v2")
        self.assertEqual(info["idf"], "v5.5.5")

    def test_esptool_v4_flash_arguments(self):
        command = firmware.flash_command("v2", self.args, "COM7", 460800, 4)
        self.assertIn("write_flash", command)
        self.assertIn("--flash_mode", command)
        self.assertNotIn("--flash-mode", command)
        self.assertEqual(command[-8:], ["0x0", "bootloader.bin", "0x8000", "partitions.bin",
                                       "0xe000", "ota_data_initial.bin", "0x10000", "app.bin"])

    def test_esptool_v5_flash_arguments(self):
        self.args["write_flash_args"] = ["--flash_mode", "dio", "--flash_size", "16MB", "--flash_freq", "40m"]
        command = firmware.flash_command("v2", self.args, "COM7", 460800, 5)
        self.assertIn("write-flash", command)
        self.assertIn("--flash-mode", command)
        self.assertNotIn("--flash_mode", command)

    def test_unsupported_esptool_version(self):
        with self.assertRaisesRegex(ValueError, "Unsupported esptool"):
            firmware.flash_command("v2", self.args, "COM7", 460800, 3)

    def test_chip_mismatch(self):
        (self.directory / "app.bin").write_bytes(make_image(chip=0))
        with self.assertRaisesRegex(ValueError, "chip mismatch"):
            firmware.validate_layout("v2", self.directory, self.args)

    def test_board_mismatch(self):
        (self.directory / "app.bin").write_bytes(make_image(board="v1"))
        with self.assertRaisesRegex(ValueError, "different board"):
            firmware.validate_layout("v2", self.directory, self.args)

    def test_wrong_flash_size(self):
        self.args["flash_settings"]["flash_size"] = "4MB"
        with self.assertRaisesRegex(ValueError, "16MB"):
            firmware.validate_layout("v2", self.directory, self.args)

    def test_corrupt_image_data(self):
        data = make_image()
        data[200] ^= 1
        (self.directory / "app.bin").write_bytes(data)
        with self.assertRaisesRegex(ValueError, "checksum"):
            firmware.validate_layout("v2", self.directory, self.args)

    def test_corrupt_digest(self):
        data = make_image()
        data[-1] ^= 1
        (self.directory / "app.bin").write_bytes(data)
        with self.assertRaisesRegex(ValueError, "digest"):
            firmware.validate_layout("v2", self.directory, self.args)

    def test_wrong_bootloader_flash_header(self):
        data = make_image()
        data[3] = 0x20
        (self.directory / self.args["bootloader"]["file"]).write_bytes(data)
        with self.assertRaisesRegex(ValueError, "Bootloader must encode"):
            firmware.validate_layout("v2", self.directory, self.args)

    def test_web_manifest_wrong_offset(self):
        output = self.directory / "package"
        with contextlib.redirect_stdout(io.StringIO()):
            firmware.package("v2", self.directory, output)
        manifest = firmware.read_json(output / "manifest.json")
        manifest["builds"][0]["parts"][0]["offset"] = 0x1000
        firmware.write_json(output / "manifest.json", manifest)
        checksums = firmware.read_json(output / "checksums.json")
        checksums["manifest.json"] = firmware.digest(output / "manifest.json")
        firmware.write_json(output / "checksums.json", checksums)
        with self.assertRaisesRegex(ValueError, "Web manifest"):
            firmware.verify("v2", output)

    def test_partition_overlap(self):
        table = bytearray(make_table())
        struct.pack_into("<I", table, 3 * 32 + 4, 0x400000)
        (self.directory / "partitions.bin").write_bytes(table)
        with self.assertRaisesRegex(ValueError, "overlap"):
            firmware.validate_layout("v2", self.directory, self.args)

    def test_ota_slot_too_small(self):
        table = bytearray(make_table())
        struct.pack_into("<I", table, 3 * 32 + 8, 256)
        (self.directory / "partitions.bin").write_bytes(table)
        with self.assertRaisesRegex(ValueError, "both OTA"):
            firmware.validate_layout("v2", self.directory, self.args)

    def test_missing_ota_slot(self):
        (self.directory / "partitions.bin").write_bytes(make_table()[:3 * 32] + b"\xff" * 32)
        with self.assertRaisesRegex(ValueError, "Both OTA"):
            firmware.validate_layout("v2", self.directory, self.args)

    def test_bad_app_offset(self):
        self.args["app"]["offset"] = "0x20000"
        with self.assertRaisesRegex(ValueError, "offset"):
            firmware.validate_layout("v2", self.directory, self.args)

    def test_path_escape(self):
        with self.assertRaisesRegex(ValueError, "escapes"):
            firmware.checked_path(self.directory, "../outside.bin")

    def test_truncated_header(self):
        with self.assertRaises(ValueError):
            firmware.image_identity(make_image()[:287], True)

    def test_package_round_trip_and_tampering(self):
        output = self.directory / "package"
        with contextlib.redirect_stdout(io.StringIO()):
            firmware.package("v2", self.directory, output)
        firmware.verify("v2", output)
        with (output / "app.bin").open("ab") as file:
            file.write(b"corruption")
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            firmware.verify("v2", output)


if __name__ == "__main__":
    unittest.main()
