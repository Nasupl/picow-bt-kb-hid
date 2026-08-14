#!/usr/bin/env python3
"""Capture Pico W CDC and Linux input events for the Bluetooth HID smoke test."""

from __future__ import annotations

import argparse
import datetime as dt
import errno
import fcntl
import glob
import json
import os
import re
import selectors
import signal
import struct
import sys
import termios
import time
from pathlib import Path


INPUT_EVENT = struct.Struct("@llHHI")
EV_KEY = 0x01
EVIOCGNAME_256 = 0x81004506
KEY_VALUES = {0: "up", 1: "down", 2: "repeat"}
REPORT_RE = re.compile(
    r"\[(HID|USB)\] (?:RX id=\d+ |TX )mod=([0-9a-fA-F]{2}) "
    r"keys=((?:[0-9a-fA-F]{2},){5}[0-9a-fA-F]{2})"
)
CONNECTION_FAILURE_RE = re.compile(r"Connection failed status=0x([0-9a-fA-F]{2})")


def iso_now() -> str:
    return dt.datetime.now().astimezone().isoformat(timespec="milliseconds")


def input_name(path: str) -> str:
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    try:
        name = bytearray(256)
        fcntl.ioctl(fd, EVIOCGNAME_256, name, True)
        return name.split(b"\0", 1)[0].decode(errors="replace")
    finally:
        os.close(fd)


def sysfs_input_name(path: str) -> str | None:
    event = os.path.basename(os.path.realpath(path))
    name_path = Path("/sys/class/input") / event / "device/name"
    try:
        return name_path.read_text(encoding="utf-8").strip()
    except OSError:
        return None


def list_devices() -> int:
    serials = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*"))
    inputs = sorted(glob.glob("/dev/input/event*"))
    print("Serial devices:")
    for path in serials:
        print(f"  {path}")
    if not serials:
        print("  (none)")
    print("Input devices:")
    for path in inputs:
        try:
            name = input_name(path)
        except OSError as exc:
            sysfs_name = sysfs_input_name(path)
            name = f"{sysfs_name} (device unavailable: {exc})" if sysfs_name else f"unavailable: {exc}"
        print(f"  {path}: {name}")
    if not inputs:
        print("  (none)")
    return 0


def configure_serial(fd: int) -> None:
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CLOCAL | termios.CREAD
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    # TinyUSB's tud_cdc_connected() follows the host DTR state. Assert it so
    # opening this collector also enables firmware logging and discovery.
    if hasattr(termios, "TIOCMBIS") and hasattr(termios, "TIOCM_DTR"):
        fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack("I", termios.TIOCM_DTR))


def open_readonly(path: str, serial: bool = False) -> int:
    try:
        access = os.O_RDWR if serial else os.O_RDONLY
        fd = os.open(path, access | os.O_NONBLOCK | os.O_NOCTTY)
        if serial:
            configure_serial(fd)
        return fd
    except OSError as exc:
        raise SystemExit(f"cannot open {path}: {exc}") from exc


def write_json_line(file, value: dict) -> None:
    file.write(json.dumps(value, ensure_ascii=False) + "\n")
    file.flush()


def make_summary(stats: dict, started: str, ended: str, interrupted: bool,
                 required_connections: int = 2) -> dict:
    stats["unmatched_bt_report_count"] = len(stats.pop("pending_bt_reports"))
    observed_connections = min(stats["ready_count"], required_connections)
    per_connection_reports = stats["matched_reports_by_connection"][:observed_connections]
    per_connection_downs = stats["key_downs_by_connection"][:observed_connections]
    per_connection_ups = stats["key_ups_by_connection"][:observed_connections]
    every_connection_has_reports = (
        observed_connections == required_connections
        and all(count >= 1 for count in per_connection_reports)
    )
    every_connection_has_input = (
        observed_connections == required_connections
        and all(count >= 1 for count in per_connection_downs)
        and all(count >= 1 for count in per_connection_ups)
    )
    passed = (
        stats["ready_count"] >= required_connections
        and stats["disconnect_count"] >= required_connections - 1
        and stats["reconnected"]
        and stats["key_down_count"] >= 1
        and stats["key_up_count"] >= 1
        and stats["post_reconnect_key_down_count"] >= 1
        and stats["post_reconnect_key_up_count"] >= 1
        and stats["key_repeat_count"] <= 100
        and stats["matched_report_count"] >= 1
        and stats["post_reconnect_matched_report_count"] >= 1
        and stats["report_mismatch_count"] == 0
        and stats["unmatched_bt_report_count"] == 0
        and stats["unexpected_usb_report_count"] == 0
        and stats["queue_overflow_count"] == 0
        and every_connection_has_reports
        and every_connection_has_input
    )
    return {
        "started_at": started,
        "ended_at": ended,
        "interrupted": interrupted,
        "required_connections": required_connections,
        **stats,
        "result": "PASS" if passed else "INCOMPLETE",
        "criteria": {
            "initial_hid_ready": stats["ready_count"] >= 1,
            "key_down_observed": stats["key_down_count"] >= 1,
            "key_up_observed": stats["key_up_count"] >= 1,
            "disconnect_observed": stats["disconnect_count"] >= 1,
            "hid_ready_after_disconnect": stats["reconnected"],
            "key_down_after_reconnect": stats["post_reconnect_key_down_count"] >= 1,
            "key_up_after_reconnect": stats["post_reconnect_key_up_count"] >= 1,
            "no_excessive_key_repeats": stats["key_repeat_count"] <= 100,
            "bt_reports_forwarded_to_usb": stats["matched_report_count"] >= 1,
            "bt_reports_forwarded_after_reconnect":
                stats["post_reconnect_matched_report_count"] >= 1,
            "no_report_mismatches": stats["report_mismatch_count"] == 0,
            "no_unmatched_bt_reports": stats["unmatched_bt_report_count"] == 0,
            "no_unexpected_usb_reports": stats["unexpected_usb_report_count"] == 0,
            "no_queue_overflow": stats["queue_overflow_count"] == 0,
            "required_connection_count_reached":
                stats["ready_count"] >= required_connections,
            "input_observed_in_every_connection": every_connection_has_input,
            "reports_forwarded_in_every_connection": every_connection_has_reports,
        },
    }


def capture(args: argparse.Namespace) -> int:
    started = iso_now()
    stamp = dt.datetime.now().astimezone().strftime("%Y%m%d-%H%M%S")
    session_dir = Path(args.output_dir) / f"smoke-{stamp}"
    session_dir.mkdir(parents=True, exist_ok=False)

    serial_fd = open_readonly(args.serial, serial=True)
    input_fd = open_readonly(args.input_event)
    try:
        device_name = input_name(args.input_event)
    except OSError:
        device_name = "unknown"

    metadata = {
        "started_at": started,
        "serial_device": os.path.realpath(args.serial),
        "input_event_device": os.path.realpath(args.input_event),
        "input_event_name": device_name,
        "duration_seconds": args.duration,
    }
    (session_dir / "metadata.json").write_text(
        json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )

    selector = selectors.DefaultSelector()
    selector.register(serial_fd, selectors.EVENT_READ, "serial")
    selector.register(input_fd, selectors.EVENT_READ, "input")
    stopping = False

    def stop(_signum, _frame):
        nonlocal stopping
        stopping = True

    old_int = signal.signal(signal.SIGINT, stop)
    old_term = signal.signal(signal.SIGTERM, stop)
    deadline = time.monotonic() + args.duration if args.duration else None
    stats = {
        "ready_count": 0,
        "disconnect_count": 0,
        "reconnected": False,
        "key_down_count": 0,
        "key_up_count": 0,
        "key_repeat_count": 0,
        "post_reconnect_key_down_count": 0,
        "post_reconnect_key_up_count": 0,
        "connection_attempt_count": 0,
        "connection_failure_count": 0,
        "connection_failure_statuses": {},
        "stale_event_count": 0,
        "queue_overflow_count": 0,
        "invalid_boot_report_count": 0,
        "matched_report_count": 0,
        "post_reconnect_matched_report_count": 0,
        "report_mismatch_count": 0,
        "unexpected_usb_report_count": 0,
        "pending_bt_reports": [],
        "matched_reports_by_connection": [],
        "key_downs_by_connection": [],
        "key_ups_by_connection": [],
    }
    serial_buffer = b""
    seen_disconnect = False
    print(f"Capturing to {session_dir}")
    print("Perform: connect -> type keys -> disconnect -> reconnect -> type keys")
    print("Press Ctrl-C to finish.")

    try:
        with (session_dir / "serial.log").open("w", encoding="utf-8") as serial_log, \
             (session_dir / "input-events.jsonl").open("w", encoding="utf-8") as input_log:
            while not stopping and (deadline is None or time.monotonic() < deadline):
                for key, _mask in selector.select(timeout=0.25):
                    try:
                        data = os.read(key.fd, 4096)
                    except OSError as exc:
                        if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                            continue
                        raise
                    if not data:
                        continue
                    if key.data == "serial":
                        serial_buffer += data
                        while b"\n" in serial_buffer:
                            raw, serial_buffer = serial_buffer.split(b"\n", 1)
                            line = raw.rstrip(b"\r").decode(errors="replace")
                            timestamp = iso_now()
                            serial_log.write(f"{timestamp} {line}\n")
                            serial_log.flush()
                            print(f"CDC {line}")
                            ready = "[BT] HID_READY" in line
                            disconnected = "[BT] DISCONNECTED" in line
                            if ready:
                                stats["ready_count"] += 1
                                stats["matched_reports_by_connection"].append(0)
                                stats["key_downs_by_connection"].append(0)
                                stats["key_ups_by_connection"].append(0)
                                if seen_disconnect:
                                    stats["reconnected"] = True
                            if disconnected:
                                stats["disconnect_count"] += 1
                                seen_disconnect = True
                                # Disconnect intentionally injects RELEASE_ALL;
                                # reports queued before teardown are no longer
                                # expected to reach the USB host.
                                stats["pending_bt_reports"].clear()
                            if "[USB] RELEASE_ALL" in line:
                                stats["pending_bt_reports"].clear()
                            if "[BT] Connecting to " in line:
                                stats["connection_attempt_count"] += 1
                            failure = CONNECTION_FAILURE_RE.search(line)
                            if failure:
                                status = f"0x{failure.group(1).lower()}"
                                stats["connection_failure_count"] += 1
                                statuses = stats["connection_failure_statuses"]
                                statuses[status] = statuses.get(status, 0) + 1
                            if "Stale " in line and " ignored" in line:
                                stats["stale_event_count"] += 1
                            if "queue overflow" in line.lower():
                                stats["queue_overflow_count"] += 1
                            if "[HID] Invalid boot report" in line:
                                stats["invalid_boot_report_count"] += 1

                            report_match = REPORT_RE.search(line)
                            if report_match:
                                report = (
                                    report_match.group(2).lower(),
                                    report_match.group(3).lower(),
                                )
                                if report_match.group(1) == "HID":
                                    stats["pending_bt_reports"].append(report)
                                elif stats["pending_bt_reports"]:
                                    expected = stats["pending_bt_reports"].pop(0)
                                    if report == expected:
                                        stats["matched_report_count"] += 1
                                        if stats["matched_reports_by_connection"]:
                                            stats["matched_reports_by_connection"][-1] += 1
                                        if stats["reconnected"]:
                                            stats["post_reconnect_matched_report_count"] += 1
                                    else:
                                        stats["report_mismatch_count"] += 1
                                elif report != ("00", "00,00,00,00,00,00"):
                                    stats["unexpected_usb_report_count"] += 1
                    else:
                        usable = len(data) - (len(data) % INPUT_EVENT.size)
                        for offset in range(0, usable, INPUT_EVENT.size):
                            sec, usec, event_type, code, value = INPUT_EVENT.unpack_from(data, offset)
                            event = {
                                "captured_at": iso_now(),
                                "kernel_time": f"{sec}.{usec:06d}",
                                "type": event_type,
                                "code": code,
                                "value": value,
                            }
                            if event_type == EV_KEY:
                                event["key_state"] = KEY_VALUES.get(value, "unknown")
                                if value == 0:
                                    stats["key_up_count"] += 1
                                    if stats["key_ups_by_connection"]:
                                        stats["key_ups_by_connection"][-1] += 1
                                    if stats["reconnected"]:
                                        stats["post_reconnect_key_up_count"] += 1
                                elif value == 1:
                                    stats["key_down_count"] += 1
                                    if stats["key_downs_by_connection"]:
                                        stats["key_downs_by_connection"][-1] += 1
                                    if stats["reconnected"]:
                                        stats["post_reconnect_key_down_count"] += 1
                                elif value == 2:
                                    stats["key_repeat_count"] += 1
                                print(f"HID code={code} {event['key_state']}")
                            write_json_line(input_log, event)
            if serial_buffer:
                serial_log.write(f"{iso_now()} {serial_buffer.decode(errors='replace')}\n")
    finally:
        selector.close()
        os.close(serial_fd)
        os.close(input_fd)
        signal.signal(signal.SIGINT, old_int)
        signal.signal(signal.SIGTERM, old_term)

    summary = make_summary(
        stats, started, iso_now(), stopping, args.required_connections
    )
    (session_dir / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(f"Result: {summary['result']}")
    print(f"Summary: {session_dir / 'summary.json'}")
    return 0 if summary["result"] == "PASS" else 2


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list-devices", action="store_true", help="list candidate devices and exit")
    parser.add_argument("--serial", help="Pico W CDC device, e.g. /dev/ttyACM0")
    parser.add_argument("--input-event", help="Pico W keyboard event device, e.g. /dev/input/event12")
    parser.add_argument("--output-dir", default="smoke-logs", help="parent directory for timestamped logs")
    parser.add_argument("--duration", type=float, default=0, help="stop after N seconds; 0 waits for Ctrl-C")
    parser.add_argument(
        "--required-connections", type=int, default=2,
        help="number of connected input sessions required for PASS (default: 2)",
    )
    args = parser.parse_args()
    if not args.list_devices and (not args.serial or not args.input_event):
        parser.error("--serial and --input-event are required")
    if args.duration < 0:
        parser.error("--duration must be zero or positive")
    if args.required_connections < 2:
        parser.error("--required-connections must be at least 2")
    return args


def main() -> int:
    args = parse_args()
    return list_devices() if args.list_devices else capture(args)


if __name__ == "__main__":
    raise SystemExit(main())
