"""Build, validate, package and flash the board-specific ESP-IDF images."""
from __future__ import annotations

import argparse
import hashlib
from importlib.metadata import version as package_version
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
ESPTOOL_REQUIREMENT = "esptool>=4.12,<6"
BOARDS = {
    "v2": {"chip": "esp32s3", "chip_id": 9, "flash": 16 * 1024 * 1024, "family": "ESP32-S3"},
    "v1": {"chip": "esp32", "chip_id": 0, "flash": 4 * 1024 * 1024, "family": "ESP32"},
    "cyd": {"chip": "esp32", "chip_id": 0, "flash": 4 * 1024 * 1024, "family": "ESP32"},
}


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_image_file(chip: str, path: Path):
    try:
        from esptool.bin_image import LoadFirmwareImage
    except ImportError as exc:
        raise ValueError("Install tools/requirements.txt or activate the ESP-IDF environment") from exc
    image = LoadFirmwareImage(chip, str(path))
    if image.calculate_checksum() != image.checksum:
        raise ValueError(f"Invalid ESP image checksum: {path.name}")
    if image.append_digest and image.calc_digest != image.stored_digest:
        raise ValueError(f"Invalid ESP image digest: {path.name}")


def image_identity(data: bytes, application: bool = False) -> dict:
    if len(data) < 24 or data[0] != 0xE9:
        raise ValueError("Not an ESP image")
    result = {"chip_id": struct.unpack_from("<H", data, 12)[0]}
    if application:
        if len(data) < 288 or struct.unpack_from("<I", data, 32)[0] != 0xABCD5432:
            raise ValueError("Missing application descriptor (use the application image for OTA)")
        result["project"] = data[80:112].split(b"\0", 1)[0].decode("ascii")
        result["version"] = data[48:80].split(b"\0", 1)[0].decode("ascii")
        result["idf"] = data[144:176].split(b"\0", 1)[0].decode("ascii")
    return result


def partitions(data: bytes) -> list[dict]:
    entries = []
    for offset in range(0, len(data) - 31, 32):
        magic, kind, subtype, start, size, label, flags = struct.unpack_from("<HBBII16sI", data, offset)
        if magic in (0xFFFF, 0xEBEB):
            break
        if magic != 0x50AA or not size:
            raise ValueError("Invalid partition table")
        entries.append({"type": kind, "subtype": subtype, "offset": start, "size": size,
                        "label": label.split(b"\0", 1)[0].decode("ascii"), "flags": flags})
    if not entries:
        raise ValueError("Empty partition table")
    return entries


def checked_path(directory: Path, relative: str) -> Path:
    result = (directory / relative).resolve()
    if not result.is_relative_to(directory.resolve()):
        raise ValueError(f"Path escapes package: {relative}")
    return result


def validate_layout(board: str, directory: Path, args: dict) -> dict:
    spec = BOARDS[board]
    if args["extra_esptool_args"]["chip"] != spec["chip"]:
        raise ValueError("Build target and board disagree")
    expected_mb = f"{spec['flash'] // (1024 * 1024)}MB"
    if args["flash_settings"]["flash_size"] != expected_mb:
        raise ValueError(f"{board} requires {expected_mb} flash configuration")
    boot_path = checked_path(directory, args["bootloader"]["file"])
    boot_data = boot_path.read_bytes()
    boot = image_identity(boot_data)
    # Web Serial writes the merged bytes without esptool's header patching.
    expected_flash_header = 0x40 if board == "v2" else 0x20
    if boot_data[2:4] != bytes((2, expected_flash_header)):
        raise ValueError("Bootloader must encode DIO, 40 MHz and the board flash capacity")
    app_path = checked_path(directory, args["app"]["file"])
    app = image_identity(app_path.read_bytes(), True)
    if boot["chip_id"] != spec["chip_id"] or app["chip_id"] != spec["chip_id"]:
        raise ValueError("Bootloader/application chip mismatch")
    if app["project"] != f"esp32_div_{board}":
        raise ValueError("Application was built for a different board")
    validate_image_file(spec["chip"], boot_path)
    validate_image_file(spec["chip"], app_path)
    table = partitions(checked_path(directory, args["partition-table"]["file"]).read_bytes())
    previous_end = 0x9000
    for entry in sorted(table, key=lambda e: e["offset"]):
        end = entry["offset"] + entry["size"]
        if entry["offset"] < previous_end or end > spec["flash"]:
            raise ValueError("Partition overlap or flash overflow")
        previous_end = end
    slots = [p for p in table if p["type"] == 0 and p["subtype"] in (0x10, 0x11)]
    if len(slots) != 2 or {p["subtype"] for p in slots} != {0x10, 0x11}:
        raise ValueError("Both OTA slots are required")
    if any(app_path.stat().st_size > p["size"] for p in slots):
        raise ValueError("Application does not fit both OTA partitions")
    if int(args["app"]["offset"], 0) != next(p["offset"] for p in slots if p["subtype"] == 0x10):
        raise ValueError("Application flash offset disagrees with ota_0")
    previous_end = 0
    for offset, name in sorted(args["flash_files"].items(), key=lambda e: int(e[0], 0)):
        start = int(offset, 0)
        end = start + checked_path(directory, name).stat().st_size
        if start < previous_end or end > spec["flash"]:
            raise ValueError("Flash images overlap or exceed flash capacity")
        previous_end = end
    return {**app, "application_bytes": app_path.stat().st_size, "partitions": table,
            "ota_free_bytes": min(p["size"] for p in slots) - app_path.stat().st_size}


def package(board: str, build: Path, destination: Path) -> Path:
    args = read_json(build / "flasher_args.json")
    info = validate_layout(board, build, args)
    destination.mkdir(parents=True, exist_ok=True)
    copied = {}
    for offset, source in args["flash_files"].items():
        name = Path(source).name
        if name in copied.values():
            raise ValueError("Ambiguous image names")
        shutil.copy2(checked_path(build, source), destination / name)
        copied[offset] = name
    args["flash_files"] = copied
    for name in ("bootloader", "partition-table", "otadata", "app"):
        args[name]["file"] = Path(args[name]["file"]).name
    write_json(destination / "flasher_args.json", args)
    # Fill gaps with erased flash bytes; offset zero works for both ESP32 variants.
    merged = bytearray(b"\xff" * max(int(o, 0) + (destination / n).stat().st_size for o, n in copied.items()))
    for offset, name in copied.items():
        data = (destination / name).read_bytes()
        start = int(offset, 0)
        merged[start:start + len(data)] = data
    merged_name = f"esp32-div-{board}-merged.bin"
    (destination / merged_name).write_bytes(merged)
    write_json(destination / "manifest.json", {
        "name": f"ESP32-DIV {board.upper()}", "version": info["version"],
        "new_install_prompt_erase": True, "new_install_improv_wait_time": 0,
        "builds": [{"chipFamily": BOARDS[board]["family"], "parts": [{"path": merged_name, "offset": 0}]}],
    })
    write_json(destination / "build-info.json", {"board": board, **info})
    shutil.copy2(Path(__file__), destination / "flash_tool.py")
    (destination / "requirements.txt").write_text(ESPTOOL_REQUIREMENT + "\n", encoding="utf-8")
    (destination / "FLASH.txt").write_text(
        f"ESP32-DIV {board.upper()} {info['version']} / {BOARDS[board]['chip']}\n\n"
        "Install Python 3.10+ and run these commands in this extracted directory:\n"
        "  python -m pip install -r requirements.txt\n"
        f"  python flash_tool.py verify --board {board} --directory .\n"
        f"  python flash_tool.py flash --board {board} --directory . --port COM7\n\n"
        "Replace COM7 with the actual serial port (Linux: /dev/ttyUSB0).\n"
        "If automatic download mode fails, hold BOOT, tap RESET, release BOOT and retry.\n"
        f"Merged image: {merged_name}; flash offset 0x0.\n"
        f"SD / HTTP OTA application image: {args['app']['file']}.\n"
        "Use the board matching the physical PCB. V1 and CYD are not interchangeable.\n"
        "This package has passed build/image validation; hardware acceptance is separate.\n",
        encoding="utf-8")
    files = list(copied.values()) + [merged_name, "flasher_args.json", "manifest.json",
                                   "build-info.json", "flash_tool.py", "requirements.txt", "FLASH.txt"]
    write_json(destination / "checksums.json", {name: digest(destination / name) for name in files})
    verify(board, destination)
    archive = destination.parent / f"esp32-div-{board}-{info['version']}.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as output:
        for name in files + ["checksums.json"]:
            output.write(destination / name, name)
    print(f"{board}: {info['application_bytes']:,} bytes; OTA free {info['ota_free_bytes']:,}; {destination}")
    return destination


def verify(board: str, directory: Path) -> dict:
    checksums = read_json(directory / "checksums.json")
    args = read_json(directory / "flasher_args.json")
    required = set(args["flash_files"].values()) | {"flasher_args.json", "manifest.json", f"esp32-div-{board}-merged.bin"}
    if not required.issubset(checksums):
        raise ValueError("Missing image checksums")
    for name, expected in checksums.items():
        if digest(checked_path(directory, name)) != expected:
            raise ValueError(f"SHA-256 mismatch: {name}")
    info = validate_layout(board, directory, args)
    manifest = read_json(directory / "manifest.json")
    expected_builds = [{"chipFamily": BOARDS[board]["family"], "parts": [
        {"path": f"esp32-div-{board}-merged.bin", "offset": 0}]}]
    if manifest.get("builds") != expected_builds:
        raise ValueError("Web manifest disagrees with the validated flash image")
    merged = (directory / f"esp32-div-{board}-merged.bin").read_bytes()
    for offset, name in args["flash_files"].items():
        data = checked_path(directory, name).read_bytes()
        start = int(offset, 0)
        if merged[start:start + len(data)] != data:
            raise ValueError("Merged image differs from component images")
    return info


def build_board(board: str) -> Path:
    idf = os.environ.get("IDF_PATH")
    if not idf:
        raise ValueError("Activate the ESP-IDF environment first, or use tools/build.ps1")
    build = ROOT / f"build-{board}"
    command = [sys.executable, str(Path(idf) / "tools/idf.py"), "-B", str(build),
               "-D", f"DIV_BOARD={board}", "-D", f"IDF_TARGET={BOARDS[board]['chip']}",
               "-D", f"SDKCONFIG={build / 'sdkconfig'}"]
    # Use IDF's tested Ninja when present, including on machines with another Ninja on PATH.
    tool_root = Path(os.environ.get("IDF_TOOLS_PATH", Path.home() / ".espressif"))
    ninjas = sorted((tool_root / "tools/ninja").glob("*/ninja.exe"))
    environment = os.environ.copy()
    if ninjas:
        environment["PATH"] = str(ninjas[-1].parent) + os.pathsep + environment["PATH"]
        command += ["-D", f"CMAKE_MAKE_PROGRAM={ninjas[-1]}"]
    subprocess.run(command + ["build"], cwd=ROOT, env=environment, check=True)
    return build


def flash_command(board: str, args: dict, port: str, baud: int,
                  esptool_major: int | None = None) -> list[str]:
    """Use the command spelling of the installed esptool without changing images."""
    major = esptool_major if esptool_major is not None else int(package_version("esptool").split(".")[0])
    if major not in (4, 5):
        raise ValueError(f"Unsupported esptool version {major}; install {ESPTOOL_REQUIREMENT}")
    command = "write_flash" if major == 4 else "write-flash"
    spellings = {"--flash-mode", "--flash-size", "--flash-freq"}
    write_args = []
    for argument in args["write_flash_args"]:
        normalized = argument.replace("_", "-") if argument.startswith("--") else argument
        if normalized in spellings:
            argument = "--" + normalized[2:].replace("-", "_") if major == 4 else normalized
        write_args.append(argument)
    images = [item for pair in args["flash_files"].items() for item in pair]
    return [sys.executable, "-m", "esptool", "--chip", BOARDS[board]["chip"],
            "--port", port, "--baud", str(baud), command, *write_args, *images]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("build", "package", "verify", "flash"))
    parser.add_argument("--board", choices=(*BOARDS, "all"), default="v2")
    parser.add_argument("--directory", type=Path, help="Package directory; default dist/<board>")
    parser.add_argument("--port", help="Serial port, required for flash")
    parser.add_argument("--baud", type=int, default=460800)
    options = parser.parse_args()
    if options.board == "all" and (options.directory or options.action == "flash"):
        parser.error("Select one board for --directory or flash")
    for board in BOARDS if options.board == "all" else (options.board,):
        directory = (options.directory or ROOT / "dist" / board).resolve()
        if options.action in ("build", "package"):
            build = build_board(board) if options.action == "build" else ROOT / f"build-{board}"
            package(board, build, directory)
        else:
            info = verify(board, directory)
            print(f"Verified {board}, version {info['version']}, {info['application_bytes']:,} bytes")
            if options.action == "flash":
                if not options.port:
                    parser.error("--port is required; no device is selected automatically")
                args = read_json(directory / "flasher_args.json")
                subprocess.run(flash_command(board, args, options.port, options.baud),
                               cwd=directory, check=True)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        sys.exit(str(exc))
