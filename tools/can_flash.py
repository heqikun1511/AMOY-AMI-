#!/usr/bin/env python3
"""Send an ESP-IDF application image to the AMI CAN Recovery program.

Uses Linux SocketCAN and only Python's standard library. Example:
    sudo ip link set can0 up type can bitrate 500000
    python3 tools/can_flash.py can0 build/lvgl_transplant.bin
"""

from __future__ import annotations

import argparse
import os
import random
import socket
import struct
import sys
import time
import zlib

RX_ID = 0x6E0
TX_ID = 0x6E1
CAN_FRAME = struct.Struct("=IB3x8s")


class ProtocolError(RuntimeError):
    pass


def frame_payload(opcode: int, session: int, tail: bytes = b"") -> bytes:
    data = bytes((opcode, session)) + tail
    if len(data) > 8:
        raise ValueError("CAN payload is longer than 8 bytes")
    return data.ljust(8, b"\0")


class CanLink:
    def __init__(self, interface: str, timeout: float):
        self.sock = socket.socket(socket.PF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
        can_filter = struct.pack("=II", TX_ID, 0x7FF)
        self.sock.setsockopt(socket.SOL_CAN_RAW, 1, can_filter)
        self.sock.bind((interface,))
        self.sock.settimeout(timeout)

    def send(self, payload: bytes) -> None:
        self.sock.send(CAN_FRAME.pack(RX_ID, 8, payload))

    def receive(self, wanted: set[int], timeout: float | None = None) -> bytes:
        previous_timeout = self.sock.gettimeout()
        if timeout is not None:
            self.sock.settimeout(timeout)
        try:
            while True:
                raw = self.sock.recv(CAN_FRAME.size)
                can_id, dlc, payload = CAN_FRAME.unpack(raw)
                if (can_id & 0x7FF) != TX_ID or dlc != 8:
                    continue
                if payload[0] == 0xFF:
                    offset = int.from_bytes(payload[4:8], "little")
                    raise ProtocolError(
                        f"device error 0x{payload[2]:02X}, command "
                        f"0x{payload[3]:02X}, offset {offset}"
                    )
                if payload[0] in wanted:
                    return payload
        except socket.timeout as exc:
            names = ", ".join(f"0x{x:02X}" for x in sorted(wanted))
            raise ProtocolError(f"timeout waiting for {names}") from exc
        finally:
            self.sock.settimeout(previous_timeout)


def enter_recovery(link: CanLink, session: int, boot_timeout: float) -> None:
    link.send(frame_payload(0x01, session, b"BOOT!\xA5"))
    ack = link.receive({0x81})
    if ack[2] != 0:
        raise ProtocolError(f"enter-Recovery rejected, status 0x{ack[2]:02X}")
    print("Dashboard accepted update request; waiting for Recovery...")
    ready = link.receive({0x90}, timeout=boot_timeout)
    if ready[2] != 0:
        raise ProtocolError(f"Recovery status 0x{ready[2]:02X}")
    size = int.from_bytes(ready[4:8], "little")
    print(f"Recovery ready, protocol {ready[3]}, main partition {size} bytes")


def query_progress(link: CanLink, session: int) -> tuple[int, int, int]:
    link.send(frame_payload(0x14, session))
    reply = link.receive({0x94})
    return reply[2], reply[3], int.from_bytes(reply[4:8], "little")


def send_image(link: CanLink, image: bytes, session: int, version: int) -> None:
    start = frame_payload(
        0x10,
        session,
        bytes((1, 0)) + len(image).to_bytes(4, "little"),
    )
    link.send(start)
    reply = link.receive({0x91})
    if reply[2] != 0:
        raise ProtocolError(f"START rejected, status 0x{reply[2]:02X}")
    window = reply[3]
    if not 1 <= window <= 64:
        raise ProtocolError(f"invalid flow-control window {window}")

    offset = 0
    frame_index = 0
    started = time.monotonic()
    while offset < len(image):
        frames_sent = 0
        while frames_sent < window and offset < len(image):
            chunk = image[offset : offset + 7]
            sequence = frame_index & 0x7F
            link.send(bytes((0x80 | sequence,)) + chunk.ljust(7, b"\0"))
            offset += len(chunk)
            frame_index += 1
            frames_sent += 1

        try:
            ack = link.receive({0x92})
            device_offset = int.from_bytes(ack[4:8], "little")
            if device_offset != offset:
                raise ProtocolError(
                    f"offset mismatch: host {offset}, device {device_offset}"
                )
        except ProtocolError as exc:
            if "timeout" not in str(exc):
                raise
            state, next_sequence, device_offset = query_progress(link, session)
            if state != 1 or device_offset > len(image):
                raise ProtocolError(
                    f"cannot resume: state {state}, offset {device_offset}"
                ) from exc
            offset = device_offset
            frame_index = (device_offset + 6) // 7
            if (frame_index & 0x7F) != next_sequence:
                raise ProtocolError("device sequence does not match reported offset")

        elapsed = max(time.monotonic() - started, 0.001)
        percent = offset * 100.0 / len(image)
        rate = offset / elapsed / 1024.0
        print(f"\r{percent:6.2f}%  {offset}/{len(image)} bytes  {rate:7.1f} KiB/s", end="", flush=True)

    print()
    crc = zlib.crc32(image) & 0xFFFFFFFF
    end = frame_payload(
        0x11,
        session,
        crc.to_bytes(4, "little") + (version & 0xFFFF).to_bytes(2, "little"),
    )
    link.send(end)
    verified = link.receive({0x93}, timeout=15.0)
    if verified[2] != 0:
        raise ProtocolError(f"verification failed, status 0x{verified[2]:02X}")
    print(f"Image verified, CRC32 0x{crc:08X}")

    activate = frame_payload(0x12, session, b"\xA5\x5A\xC3\x3C\x02\x00")
    link.send(activate)
    activated = link.receive({0x95})
    if activated[2] != 0:
        raise ProtocolError(f"activation failed, status 0x{activated[2]:02X}")
    print("ota_0 activated; dashboard is rebooting")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Flash the AMI dashboard over classic CAN")
    parser.add_argument("interface", help="SocketCAN interface, for example can0")
    parser.add_argument("image", help="ESP-IDF application .bin file")
    parser.add_argument("--session", type=lambda x: int(x, 0), default=None)
    parser.add_argument("--version", type=lambda x: int(x, 0), default=0)
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--boot-timeout", type=float, default=12.0)
    parser.add_argument(
        "--skip-enter",
        action="store_true",
        help="Recovery is already running; do not send the dashboard reboot request",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        image_size = os.path.getsize(args.image)
        if image_size < 1024:
            raise ProtocolError("input does not look like an ESP application image")
        with open(args.image, "rb") as image_file:
            image = image_file.read()
        session = args.session if args.session is not None else random.randint(1, 255)
        if not 0 <= session <= 255:
            raise ProtocolError("session must fit in one byte")

        link = CanLink(args.interface, args.timeout)
        if args.skip_enter:
            state, _, offset = query_progress(link, session)
            print(f"Recovery reachable, state {state}, offset {offset}")
        else:
            enter_recovery(link, session, args.boot_timeout)
        send_image(link, image, session, args.version)
        return 0
    except (OSError, ProtocolError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
