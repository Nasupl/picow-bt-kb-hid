# Hardware smoke test

`smoke_test.py` captures the Pico W USB CDC log and Linux input events into one
timestamped test session. It uses only the Python standard library.

## Preparation

Build and flash the firmware, connect the Pico W to a Linux host, and identify
its CDC and keyboard event devices:

```bash
python3 tools/smoke_test.py --list-devices
```

The user must have read access to both devices. Depending on the distribution,
membership in groups such as `dialout` and `input`, or an appropriate udev rule,
may be required. Do not run the script as root unless that is already part of
the local development policy.

For a one-session test, grant only the selected Pico keyboard event device to
the current user with an ACL (replace the event number if needed):

```bash
sudo setfacl -m "u:$USER:r" /dev/input/event8
```

This ACL normally disappears when the USB device is unplugged. For repeated
testing, add the user to the distribution's `input` group and log in again, or
install a narrowly matched udev rule for this USB product instead of making all
input devices world-readable.

## Capture

Start capture with the devices reported above:

```bash
python3 tools/smoke_test.py \
  --serial /dev/ttyACM0 \
  --input-event /dev/input/event12
```

Then perform this sequence:

1. Put the Bluetooth keyboard into pairing/connection mode. If requested, type
   PIN `0000` on the Bluetooth keyboard and press Enter, then wait for
   `HID_READY` in the console.
2. Press and release ordinary, modified, and simultaneous keys.
3. While holding a key, turn the Bluetooth keyboard off and then release it.
4. Turn the keyboard on again and wait for the second ready indication.
5. Press and release another key, then press Ctrl-C to finish capture.

Use `--duration 180` to stop automatically after three minutes. Every run
creates `smoke-logs/smoke-YYYYMMDD-HHMMSS/` containing:

- `metadata.json` — devices and session start information
- `serial.log` — timestamped CDC output
- `input-events.jsonl` — timestamped Linux input events
- `summary.json` — observed counts, individual criteria, and PASS/INCOMPLETE

PASS requires two HID-ready indications with a disconnect between them, USB
key-down/up events, another key-down/up pair after reconnection, and no more
than 100 repeat events (a guard against stuck keys). It also compares the
firmware's `[HID] RX` and `[USB] TX` reports in order and requires at least one
matching report before and after reconnection, no unmatched or unexpected
reports, and no HID/CDC queue overflow.

For a repeated stability run, require input in ten separate connections:

```bash
python3 tools/smoke_test.py \
  --serial /dev/ttyACM0 \
  --input-event /dev/input/event8 \
  --required-connections 10 \
  --duration 900
```

Disconnect and reconnect the keyboard nine times, typing at least one key in
every connection. The per-connection input and forwarding counts are saved in
the summary, and PASS requires activity in all ten sessions.

The summary also records connection attempts and failure status codes, ignored
stale events, and rejected boot reports. Connection failures and rejected
non-keyboard packets are diagnostic counts: they do not fail the test if the
complete connection/reconnection cycle and report-forwarding criteria pass.
