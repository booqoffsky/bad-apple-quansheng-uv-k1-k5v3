#!/usr/bin/env python3
"""
UV-K1/UV-K5V3 Bulk Flash Tool.

Uploads binary data into the external PY25Q16 flash chip using custom
firmware commands (0x0530, 0x0531, 0x0532). Requires a firmware build
with ENABLE_PLAYER defined.

Dependencies:
    - pyserial
    - tools.serialtool (from this repo)

Usage:
     python3 -m tools.player.flash --port /dev/ttyACM0 animation.bin

Copyright (c) 2026 booqoffsky

Licensed under the MIT License (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at the root of this repository.

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
"""
import argparse
import struct
import sys
from datetime import datetime
from time import sleep, time

from serial import Serial, SerialException

# Local imports from the repository
from tools.serialtool import msg as mm

# ---------------------------------------------------------------------------
# Protocol Constants
# ---------------------------------------------------------------------------
CMD_ERASE = 0x0530
CMD_WRITE = 0x0531
CMD_READ = 0x0532

SECTOR_SIZE = 0x1000  # 4 KiB
MAX_WRITE_CHUNK = 128  # Max payload size for write command
MAX_READ_CHUNK = 128  # Max payload size for read command
DEFAULT_OFFSET = 0x012000  # PLAYER_FLASH_BASE defined in player.c

REPLY_TIMEOUT_S = 2.0  # Standard timeout for read/write replies
ERASE_TIMEOUT_S = 5.0  # Loop interval timeout while waiting for erase
MAX_RETRIES = 5  # Maximum communication retries
BAUDRATE = 38400  # Default baudrate for the radio's serial interface


# ---------------------------------------------------------------------------
# Utility Functions
# ---------------------------------------------------------------------------
def pack_le32(val: int) -> bytes:
    """Pack an integer into a 4-byte little-endian byte string."""
    return struct.pack("<I", val)


def pack_le16(val: int) -> bytes:
    """Pack an integer into a 2-byte little-endian byte string."""
    return struct.pack("<H", val)


def send_and_recv(
    ser: Serial,
    msg: mm.Msg,
    expected_type: int,
    retries: int = MAX_RETRIES,
    timeout: float = REPLY_TIMEOUT_S,
) -> mm.Msg:
    """
    Send a message and block until the expected reply is received.

    Args:
        ser: Active serial connection.
        msg: The message object to send.
        expected_type: The message type ID to wait for.
        retries: Number of times to resend on timeout. If 0, the command
                 is sent once and the function waits indefinitely (used
                 for flash erase which takes several seconds).
        timeout: Seconds to wait before triggering a retry or timeout.

    Returns:
        The matched reply message.

    Raises:
        TimeoutError: If maximum retries are exceeded without a valid reply.
    """

    def _do_send():
        ser.reset_input_buffer()
        ser.write(mm.make_packet(msg.buf))
        ser.flush()

    buf = bytearray()
    attempts = 0
    _do_send()
    last_send = time()

    while True:
        # Read available data from serial buffer
        n = ser.in_waiting
        if n > 0:
            buf.extend(ser.read(n))
            reply = mm.fetch(buf)
            if reply and reply.get_msg_type() == expected_type:
                return reply

        # Handle timeout
        if time() - last_send > timeout:
            if retries > 0 and attempts < retries:
                attempts += 1
                print(f"\n[WARN] Timeout, retry {attempts}/{retries}…", file=sys.stderr)
                buf.clear()
                _do_send()
                last_send = time()
            elif retries == 0:
                # For long operations (e.g., erase), just reset the loop timer
                # without resending the command to avoid interrupting the chip.
                last_send = time()
            else:
                raise TimeoutError("Maximum communication retries reached.")

        sleep(0.001)  # Yield to OS, prevent 100% CPU core usage


# ---------------------------------------------------------------------------
# Flashing Stages
# ---------------------------------------------------------------------------
def handshake(ser: Serial) -> int:
    """
    Perform initial handshake with the radio.

    Sends command 0x0514 to retrieve firmware version and establish a
    session timestamp.

    Args:
        ser: Active serial connection.

    Returns:
        The established session timestamp as an integer.
    """
    ts = int(datetime.now().timestamp()) & 0xFFFFFFFF
    msg = mm.Msg(8)
    msg.set_msg_type(0x0514)
    msg.buf[4:8] = pack_le32(ts)

    reply = send_and_recv(ser, msg, 0x0515)
    end = reply.buf.find(b"\0", 4, 20)
    ver = reply.buf[4 : end if end != -1 else 20].decode("ascii", errors="replace")

    print(f"[INFO] Device version: {ver}")
    return ts


def erase_flash(ser: Serial, offset: int, sector_count: int, timestamp: int) -> None:
    """
    Erase the required sectors on the external flash.

    Args:
        ser: Active serial connection.
        offset: Starting memory address.
        sector_count: Number of 4KiB sectors to erase.
        timestamp: Session timestamp for command validation.

    Raises:
        ValueError: If the radio's reply does not match the request.
    """
    msg = mm.Msg(16)
    msg.set_msg_type(CMD_ERASE)
    msg.buf[4:8] = pack_le32(offset)
    msg.buf[8:12] = pack_le32(sector_count)
    msg.buf[12:16] = pack_le32(timestamp)

    total_bytes = sector_count * SECTOR_SIZE
    print(
        f"[INFO] Erasing {sector_count} sectors ({total_bytes} bytes) at 0x{offset:06X}…"
    )

    # retries=0 means: send once, wait as long as it takes for the chip to erase
    reply = send_and_recv(ser, msg, CMD_ERASE, retries=0, timeout=ERASE_TIMEOUT_S)

    r_addr, r_count = struct.unpack_from("<II", reply.buf, 4)
    if r_addr != offset or r_count != sector_count:
        raise ValueError(
            f"Erase reply mismatch! Expected addr=0x{offset:06X}, count={sector_count}. "
            f"Got addr=0x{r_addr:06X}, count={r_count}."
        )
    print("[INFO] Erase successful.")


def write_flash(ser: Serial, data: bytes, offset: int, timestamp: int) -> None:
    """
    Write binary data to the external flash.

    Args:
        ser: Active serial connection.
        data: The binary data to write.
        offset: Starting memory address.
        timestamp: Session timestamp for command validation.

    Raises:
        ValueError: If the radio acknowledges a different address or chunk size.
    """
    pos = 0
    total = len(data)

    while pos < total:
        chunk = data[pos : pos + MAX_WRITE_CHUNK]
        msg = mm.Msg(16 + len(chunk))
        msg.set_msg_type(CMD_WRITE)
        msg.buf[4:8] = pack_le32(offset + pos)
        msg.buf[8:10] = pack_le16(len(chunk))
        msg.buf[12:16] = pack_le32(timestamp)
        msg.buf[16 : 16 + len(chunk)] = chunk

        reply = send_and_recv(ser, msg, CMD_WRITE)
        r_addr, r_size = struct.unpack_from("<IH", reply.buf, 4)

        if r_addr != offset + pos or r_size != len(chunk):
            raise ValueError(f"Write acknowledgement mismatch at 0x{offset + pos:06X}.")

        pos += len(chunk)
        pct = pos * 100 // total
        print(f"\r[INFO] Writing... {pct}% (0x{offset + pos:06X})", end="", flush=True)

    print()  # New line after progress bar


def verify_flash(ser: Serial, data: bytes, offset: int, timestamp: int) -> None:
    """
    Read back data from the flash and verify integrity.

    Args:
        ser: Active serial connection.
        data: The expected binary data.
        offset: Starting memory address.
        timestamp: Session timestamp for command validation.

    Raises:
        ValueError: If the read data does not match the expected data.
    """
    pos = 0
    total = len(data)

    while pos < total:
        size = min(MAX_READ_CHUNK, total - pos)
        msg = mm.Msg(16)
        msg.set_msg_type(CMD_READ)
        msg.buf[4:8] = pack_le32(offset + pos)
        msg.buf[8:10] = pack_le16(size)
        msg.buf[12:16] = pack_le32(timestamp)

        reply = send_and_recv(ser, msg, CMD_READ)
        r_addr, r_size = struct.unpack_from("<IH", reply.buf, 4)

        if r_addr != offset + pos or r_size != size:
            raise ValueError(f"Verify read mismatch at 0x{offset + pos:06X}.")

        read_data = bytes(reply.buf[12 : 12 + r_size])
        expected_data = data[pos : pos + r_size]

        if read_data != expected_data:
            raise ValueError(
                f"Verification failed! Data mismatch at offset 0x{offset + pos:06X}."
            )

        pos += r_size
        pct = pos * 100 // total
        print(
            f"\r[INFO] Verifying... {pct}% (0x{offset + pos:06X})", end="", flush=True
        )

    print(" OK.")


# ---------------------------------------------------------------------------
# Main Entry Point
# ---------------------------------------------------------------------------
def main() -> None:
    """Parse arguments and execute the flashing sequence."""
    parser = argparse.ArgumentParser(
        description="Bulk-flash tool for UV-K1/UV-K5V3 external flash (PY25Q16).",
        epilog="Example: python flash_tool.py -p /dev/ttyUSB0 --verify animation.bin",
    )
    parser.add_argument(
        "--port",
        "-p",
        required=True,
        help="Serial port (e.g., /dev/ttyUSB0 or /dev/ttyACM0)",
    )
    parser.add_argument(
        "--offset",
        "-o",
        type=lambda x: int(x, 0),
        default=DEFAULT_OFFSET,
        help=f"Flash base address (default: 0x{DEFAULT_OFFSET:X})",
    )
    parser.add_argument(
        "--no-erase",
        action="store_true",
        help="Skip sector erase (use only if flash area is already blank)",
    )
    parser.add_argument(
        "--verify",
        "-v",
        action="store_true",
        help="Perform read-back verification after writing",
    )
    parser.add_argument("file", help="Path to the binary file (.bin) to flash")

    args = parser.parse_args()

    # Load binary file
    try:
        with open(args.file, "rb") as f:
            data = f.read()
    except FileNotFoundError:
        print(f"[ERROR] File '{args.file}' not found.", file=sys.stderr)
        sys.exit(1)
    except IOError as e:
        print(f"[ERROR] Cannot read file '{args.file}': {e}", file=sys.stderr)
        sys.exit(1)

    print(f"[INFO] Loaded {args.file}: {len(data)} bytes")

    # Execute flashing routine
    try:
        with Serial(
            port=args.port, baudrate=BAUDRATE, timeout=0.1, write_timeout=1
        ) as ser:
            ts = handshake(ser)

            if not args.no_erase:
                sector_count = (len(data) + SECTOR_SIZE - 1) // SECTOR_SIZE
                erase_flash(ser, args.offset, sector_count, ts)

            write_flash(ser, data, args.offset, ts)

            if args.verify:
                verify_flash(ser, data, args.offset, ts)

            print("[INFO] Done.")

    except SerialException as e:
        print(f"[ERROR] Serial port error: {e}", file=sys.stderr)
        sys.exit(1)
    except TimeoutError as e:
        print(f"\n[ERROR] Communication failed: {e}", file=sys.stderr)
        sys.exit(1)
    except ValueError as e:
        print(f"\n[ERROR] Protocol error: {e}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\n[INFO] Aborted by user.")
        sys.exit(0)


if __name__ == "__main__":
    main()
