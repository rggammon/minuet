# SPDX-License-Identifier: GPL-3.0-only
"""Capture BTT HDMI7 evdev events and decoded raw HID touch reports.

Usage: sudo python3 tools/touch-capture.py [seconds] [--quiet]
"""
import glob
import os
import select
import struct
import sys
import time

EVENT = "/dev/input/by-id/usb-BIQU_BTT-HDMI7_SN0000000003-event-if00"
HIDRAW_MATCHES = [
    "/dev/" + os.path.basename(path)
    for path in glob.glob("/sys/class/hidraw/hidraw*")
    if ":04D9:8030." in os.path.realpath(path)
]
if len(HIDRAW_MATCHES) > 1:
    sys.exit(f"Expected at most one BTT hidraw node, found {HIDRAW_MATCHES}")
HIDRAW = HIDRAW_MATCHES[0] if HIDRAW_MATCHES else None
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
VERBOSE = "--quiet" not in sys.argv[2:]
FORMAT = "llHHi"
SIZE = struct.calcsize(FORMAT)
TYPES = {0: "SYN", 1: "KEY", 3: "ABS"}
CODES = {
    (1, 0x14A): "BTN_TOUCH",
    (3, 0x00): "ABS_X",
    (3, 0x01): "ABS_Y",
    (3, 0x28): "ABS_MISC",
    (3, 0x2F): "ABS_MT_SLOT",
    (3, 0x30): "ABS_MT_TOUCH_MAJOR",
    (3, 0x35): "ABS_MT_POSITION_X",
    (3, 0x36): "ABS_MT_POSITION_Y",
    (3, 0x39): "ABS_MT_TRACKING_ID",
    (0, 0): "SYN_REPORT",
    (0, 3): "SYN_DROPPED",
}

def describe(report):
    if len(report) != 44 or report[0] != 1:
        return report.hex(" ")
    contacts = []
    for index in range(5):
        tip, contact, x, y, width = struct.unpack_from("<BBHHH", report, 1 + index * 8)
        contacts.append(f"[tip={tip & 1} id={contact} x={x} y={y} w={width}]")
    scan, count = struct.unpack_from("<HB", report, 41)
    return f"count={count} scan={scan} " + " ".join(contacts)


event_fd = os.open(EVENT, os.O_RDONLY | os.O_NONBLOCK)
hid_fd = os.open(HIDRAW, os.O_RDONLY | os.O_NONBLOCK) if HIDRAW else None
sources = [event_fd] + ([hid_fd] if hid_fd is not None else [])
print(f"Capturing {EVENT} and {HIDRAW or 'no hidraw node'} for {DURATION:.0f}s.", flush=True)
counts = {"evdev": 0, "hidraw": 0}
start = time.monotonic()
end = start + DURATION
previous = None
per_second = {}
distinct = {}
try:
    while (remaining := end - time.monotonic()) > 0:
        ready, _, _ = select.select(sources, [], [], remaining)
        for fd in ready:
            data = os.read(fd, 4096)
            if fd == hid_fd:
                counts["hidraw"] += 1
                now = time.monotonic() - start
                second = int(now)
                per_second[second] = per_second.get(second, 0) + 1
                first, number = distinct.get(data, (now, 0))
                distinct[data] = (first, number + 1)
                if data != previous and VERBOSE:
                    print(f"{now:6.2f}s hidraw changed: {describe(data)}", flush=True)
                previous = data
                continue
            for offset in range(0, len(data) - SIZE + 1, SIZE):
                _, _, kind, code, value = struct.unpack_from(FORMAT, data, offset)
                counts["evdev"] += 1
                if counts["evdev"] <= 200 and VERBOSE:
                    name = CODES.get((kind, code), hex(code))
                    print(f"{time.monotonic() - start:6.2f}s evdev: "
                          f"{TYPES.get(kind, kind)} {name} {value}", flush=True)
finally:
    os.close(event_fd)
    if hid_fd is not None:
        os.close(hid_fd)
print("Totals:", counts, flush=True)
print("Reports per second:", [per_second.get(s, 0) for s in range(int(DURATION))], flush=True)
if distinct:
    print("Distinct raw HID reports (first seen, count):", flush=True)
    for report, (first, number) in distinct.items():
        print(f"  {first:6.2f}s x{number}: {describe(report)}", flush=True)
