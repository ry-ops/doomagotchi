#!/usr/bin/env python3
"""Push a file to the doom-node's SD card over the serial console.

The doom-node firmware (recv_wad.c) enters "WAD receive mode" at boot when
/sdcard/freedoom1.wad is missing. This is the one-time host side.

    python tools/send-wad.py /dev/cu.usbmodem1101 assets/freedoom1.wad

Needs pyserial (`pip install pyserial`). Runs at 921600 (the firmware's
console baud). ~27 MiB takes a few minutes.

Wire protocol:
    dev  -> "WADRECV READY"
    host -> "WADBIN <size> <crc32_hex>\\n"
    host -> <size> raw bytes
    dev  -> "WADRESULT OK ..."  |  "WADRESULT FAIL <reason>"
"""

import sys
import time
import zlib

BAUD = 921600
CHUNK = 8192


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    port, path = sys.argv[1], sys.argv[2]

    try:
        import serial
    except ImportError:
        print("needs pyserial:  pip install pyserial", file=sys.stderr)
        return 2

    with open(path, "rb") as f:
        data = f.read()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    print(f"{path}: {len(data)} bytes, crc32={crc:08x}")

    ser = serial.Serial(port, BAUD, timeout=1)
    ser.reset_input_buffer()

    # Nudge a reset so we catch the boot + "WADRECV READY". If the board is
    # already sitting in receive mode this is harmless.
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.15)
    ser.setRTS(False)

    print("waiting for 'WADRECV READY' (reset the board if it doesn't appear)...")
    deadline = time.time() + 40
    line = b""
    while time.time() < deadline:
        b = ser.read(1)
        if not b:
            continue
        if b == b"\n":
            text = line.decode("utf-8", "replace").strip()
            if text:
                print("  dev:", text)
            if "WADRECV READY" in text:
                break
            line = b""
        else:
            line += b
    else:
        print("timed out waiting for READY", file=sys.stderr)
        return 1

    ser.write(f"WADBIN {len(data)} {crc:08x}\n".encode())
    ser.flush()

    t0 = time.time()
    sent = 0
    for i in range(0, len(data), CHUNK):
        ser.write(data[i:i + CHUNK])
        sent += min(CHUNK, len(data) - i)
        if sent % (1 << 20) < CHUNK or sent == len(data):
            el = time.time() - t0
            rate = sent / el / 1024 if el else 0
            pct = 100 * sent / len(data)
            print(f"\r  sent {sent}/{len(data)} ({pct:.0f}%)  {rate:.0f} KiB/s", end="", flush=True)
    ser.flush()
    print()

    # wait for the verdict
    deadline = time.time() + 60
    line = b""
    while time.time() < deadline:
        b = ser.read(1)
        if not b:
            continue
        if b == b"\n":
            text = line.decode("utf-8", "replace").strip()
            if text:
                print("  dev:", text)
            if "WADRESULT" in text:
                ser.close()
                return 0 if "WADRESULT OK" in text else 1
            line = b""
        else:
            line += b

    print("no WADRESULT from device", file=sys.stderr)
    ser.close()
    return 1


if __name__ == "__main__":
    sys.exit(main())
