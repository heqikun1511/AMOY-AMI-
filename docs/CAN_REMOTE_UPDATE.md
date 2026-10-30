# CAN remote firmware update

The dashboard uses classic CAN at 500 kbit/s with 11-bit identifiers and DLC 8.
The normal dashboard receives update commands on `0x6E0`; both the dashboard
and Recovery reply on `0x6E1`. Recovery can write only the fixed `main`/`ota_0`
partition at `0x190000`. The bootloader, partition table, NVS and Recovery
partition are never writable through this protocol.

The importable network description is [`ami_can_ota.dbc`](ami_can_ota.dbc).

## Partition layout

| Partition | Offset | Size | Purpose |
|---|---:|---:|---|
| NVS | `0x9000` | 16 KiB | persistent update request |
| OTA data | `0xD000` | 8 KiB | selected image and rollback state |
| PHY init | `0xF000` | 4 KiB | ESP-IDF PHY data |
| can_recovery/factory | `0x10000` | 1.5 MiB | CAN Recovery application |
| main/ota_0 | `0x190000` | 12 MiB | dashboard application |

## VCU to dashboard/Recovery (`0x6E0`)

All multi-byte values are little-endian.

| Byte 0 | Bytes | Meaning |
|---:|---|---|
| `01` | `01 session 42 4F 4F 54 21 A5` | authenticated request to leave dashboard and enter Recovery |
| `10` | `10 session 01 00 size32` | start; target `01` is the fixed main partition, flags must be zero |
| `80..FF` | `(80 | sequence7) data[7]` | firmware data; sequence rolls over modulo 128 |
| `11` | `11 session crc32 version16` | finish and verify |
| `12` | `12 session A5 5A C3 3C delay 00` | activate; delay is in 100 ms units |
| `13` | `13 session 00 00 00 00 00 00` | abort current transfer |
| `14` | `14 session 00 00 00 00 00 00` | query progress |

CRC is standard reflected CRC-32/ISO-HDLC, the value produced by
`zlib.crc32(image)`. The final data frame is padded to DLC 8, but padding bytes
are neither written nor included in CRC.

## Dashboard/Recovery to VCU (`0x6E1`)

| Byte 0 | Bytes | Meaning |
|---:|---|---|
| `81` | `81 session status ...` | dashboard accepted enter-Recovery request |
| `90` | `90 00 status protocol partition_size32` | Recovery ready |
| `91` | `91 session status window partition_size32` | start accepted; current window is 32 frames |
| `92` | `92 session status next_seq received32` | flow-control acknowledgement |
| `93` | `93 session status state received32` | CRC and ESP image validation passed |
| `94` | `94 session state next_seq received32` | query response |
| `95` | `95 session status delay received32` | activation accepted |
| `96` | `96 session status ...` | abort acknowledged |
| `FF` | `FF session error command received32` | error response |

Error values are: `01` command, `02` session, `03` length, `04` sequence,
`05` flash, `06` CRC, `07` invalid ESP image, `09` invalid state, `0A`
timeout, and `0B` busy.

## Build and first installation

The new partition table must be installed once over USB/JTAG. Do not use the
normal `idf.py flash` target after this conversion because ESP-IDF selects the
factory offset for the large dashboard binary. Use the fixed-address helper:

```sh
tools/flash_can_ota_initial.sh /dev/ttyACM0
```

This writes the Recovery bootloader and partition table, Recovery at
`0x10000`, and the dashboard at `0x190000`. Recovery validates the existing
dashboard and selects it on the first boot.

## Update over SocketCAN

```sh
sudo ip link set can0 up type can bitrate 500000
python3 tools/can_flash.py can0 build/lvgl_transplant.bin --version 1
```

The sender waits for an acknowledgement every 32 data frames and can recover
from a lost acknowledgement by querying the committed byte offset. If power is
lost after `ota_0` has been erased, the immutable Recovery application remains
bootable and waits for the transfer to be repeated.

The dashboard rejects the enter-Recovery command with status `09` if a fresh
motor frame reports more than 50 rpm or a fresh control-status frame reports
the shutdown circuit closed. With no fresh vehicle-status traffic, workshop
recovery remains available.

For production vehicles, enable ESP Secure Boot/app signing after provisioning
the signing process. The current magic sequence prevents accidental entry but
is not cryptographic authentication.
