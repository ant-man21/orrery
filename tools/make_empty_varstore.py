#!/usr/bin/env python3
"""
make_empty_varstore.py — patch a valid, empty NV variable store into a
flash image at a fixed offset.

Why this exists: edk2-platforms' own SbsaQemu.fdf bakes an equivalent
structure (EFI_FIRMWARE_VOLUME_HEADER + VARIABLE_STORE_HEADER, plus an
EFI_FAULT_TOLERANT_WORKING_BLOCK_HEADER) into its non-secure variable
store region at *build* time, as a literal `DATA = {...}` hex blob in the
FDF. We need the same thing for SbsaOrreryStandaloneMm's secure variable
store — but that region lives inside SBSA_FLASH0.fd, whose [FD] section
is defined entirely by edk2-platforms' upstream SbsaQemu.fdf (BL1+FIP
only), not anything we own. Forking that whole FDF just to add one DATA
region would mean duplicating its FV definitions too. Patching the built
flash image after the fact — same idea as sign_rom_ticket.py patching a
ticket in post-build — is far less invasive and doesn't touch a vendored
file at all.

Without this, VariableStandaloneMm reads raw erased-flash bytes at boot,
finds no recognizable header, and ASSERTs "Volume Corrupt"
(VariableSmm.c:1164) — StandaloneMmCore itself has already fully
initialized by that point; this is pure first-boot NV storage formatting,
nothing protocol-related.

Layout patched in, matching SbsaQemu.fdf's own non-secure store exactly
(see its [FD.SBSA_FLASH1] DATA blocks) except sized for our PCDs
(SbsaOrreryStandaloneMm.dsc: 64 KiB each for Variable / FtwWorking /
FtwSpare, vs. the reference's 256 KiB):

  offset+0x00000  EFI_FIRMWARE_VOLUME_HEADER + VARIABLE_STORE_HEADER
                  (FvLength spans all three regions combined, matching
                  the reference — FTW copies between "working" and
                  "spare" within what's logically one reserved NV FV)
  offset+var_size            EFI_FAULT_TOLERANT_WORKING_BLOCK_HEADER
  offset+var_size+ftw_size   left erased (0xFF) — matches the reference,
                             which never seeds NV_FTW_SPARE with content

SPDX-License-Identifier: BSD-2-Clause-Patent
"""
import argparse
import struct
import sys
import uuid
import zlib

# gEfiSystemNvDataFvGuid
FV_GUID = uuid.UUID("fff12b8d-7696-4c8b-a985-2747075b4f50")
# gEfiAuthenticatedVariableGuid — VARIABLE_STORE_HEADER signature.
# (Compatible with SECURE_BOOT_ENABLE == FALSE too, same as the reference.)
VARIABLE_STORE_GUID = uuid.UUID("aaf32c78-947b-439a-a180-2e144ec37792")
# gEdkiiWorkingBlockSignatureGuid
FTW_WORKING_GUID = uuid.UUID("9e58292b-7c68-497d-a0ce-6500fd9f1b95")

FVH_SIGNATURE = b"_FVH"
FVH_ATTRIBUTES = 0x0004FEFF   # matches the reference byte-for-byte
FVH_HEADER_LEN = 0x48         # EFI_FIRMWARE_VOLUME_HEADER + 1 BlockMap entry + terminator
FVH_REVISION = 0x02

FTW_WORKING_HEADER_SIZE = 32  # GUID(16) + Crc(4) + State+Reserved(4) + WriteQueueSize(8)
# WorkingBlockValid cleared (erase-polarity: 0 == "set"), everything else
# left erased — fixed, size-independent initial state used by every
# EDK2 platform's freshly-formatted FTW working block.
FTW_STATE_BYTES = bytes([0xFE, 0xFF, 0xFF, 0xFF])


def guid_bytes(g: uuid.UUID) -> bytes:
    """EFI_GUID on-disk byte order (mixed-endian per the GUID spec)."""
    b = g.bytes_le
    return b


def fv_checksum16(header: bytes) -> int:
    """EFI_FIRMWARE_VOLUME_HEADER.Checksum: 16-bit words summed to zero."""
    assert len(header) % 2 == 0
    total = 0
    for i in range(0, len(header), 2):
        total = (total + struct.unpack_from("<H", header, i)[0]) & 0xFFFF
    return (0x10000 - total) & 0xFFFF


def build_variable_store(region_size: int, var_size: int) -> bytes:
    """EFI_FIRMWARE_VOLUME_HEADER + VARIABLE_STORE_HEADER, FvLength ==
    region_size (Variable + FtwWorking + FtwSpare combined, matching the
    reference), single BlockMap entry covering the whole region."""
    # Named byte offsets instead of manual slicing — the Checksum field
    # sits at *decimal* offset 50 (0x32 hex), a number easy to mistype as
    # decimal 32 by confusing the two bases (did exactly that on the
    # first pass here). Building the header field-by-field into a
    # preallocated buffer via struct.pack_into avoids that whole class of
    # mistake: every field is addressed by name, not by a recomputed byte
    # range.
    OFF_ZERO_VECTOR = 0x00        # 16 bytes
    OFF_FILESYSTEM_GUID = 0x10    # 16 bytes
    OFF_FV_LENGTH = 0x20          # 8 bytes
    OFF_SIGNATURE = 0x28          # 4 bytes
    OFF_ATTRIBUTES = 0x2C         # 4 bytes
    OFF_HEADER_LENGTH = 0x30      # 2 bytes
    OFF_CHECKSUM = 0x32           # 2 bytes
    OFF_EXT_HEADER_OFFSET = 0x34  # 2 bytes
    OFF_RESERVED = 0x36           # 1 byte
    OFF_REVISION = 0x37           # 1 byte
    OFF_BLOCK_MAP = 0x38          # 2x (NumBlocks u32, Length u32); ends at 0x48

    assert OFF_BLOCK_MAP + 16 == FVH_HEADER_LEN, "offsets don't add up to FVH_HEADER_LEN"

    header = bytearray(FVH_HEADER_LEN)
    # ZeroVector left as zero.
    header[OFF_FILESYSTEM_GUID:OFF_FILESYSTEM_GUID + 16] = guid_bytes(FV_GUID)
    struct.pack_into("<Q", header, OFF_FV_LENGTH, region_size)
    header[OFF_SIGNATURE:OFF_SIGNATURE + 4] = FVH_SIGNATURE
    struct.pack_into("<I", header, OFF_ATTRIBUTES, FVH_ATTRIBUTES)
    struct.pack_into("<H", header, OFF_HEADER_LENGTH, FVH_HEADER_LEN)
    struct.pack_into("<H", header, OFF_CHECKSUM, 0)  # placeholder
    struct.pack_into("<H", header, OFF_EXT_HEADER_OFFSET, 0)
    header[OFF_RESERVED] = 0
    header[OFF_REVISION] = FVH_REVISION
    struct.pack_into("<II", header, OFF_BLOCK_MAP, 1, region_size)      # one block...
    struct.pack_into("<II", header, OFF_BLOCK_MAP + 8, 0, 0)            # ...then terminator

    checksum = fv_checksum16(bytes(header))
    struct.pack_into("<H", header, OFF_CHECKSUM, checksum)
    header = bytes(header)

    var_store_hdr_size = 16 + 4 + 1 + 1 + 2 + 4  # GUID + Size + Format + State + Reserved + Reserved1
    # NorFlashFvb.c's ValidateFvHeader() checks:
    #   VariableStoreHeader->Size == PcdFlashNvStorageVariableSize - FwVolHeader->HeaderLength
    # i.e. Size covers everything after the FVH header (VARIABLE_STORE_HEADER
    # itself included) — it is NOT the size of the variable *data* area
    # alone. Subtracting var_store_hdr_size a second time here (as an
    # earlier version of this script did) undercounts Size by 28 bytes and
    # fails that check with "Variable Store Length does not match".
    var_store_size = var_size - FVH_HEADER_LEN
    var_store_header = (
        guid_bytes(VARIABLE_STORE_GUID)
        + struct.pack("<I", var_store_size)        # Size (header + variable data area)
        + bytes([0x5A, 0xFE])                      # FORMATTED, HEALTHY
        + struct.pack("<H", 0)                     # Reserved
        + struct.pack("<I", 0)                     # Reserved1
    )

    blob = header + var_store_header
    assert len(blob) == FVH_HEADER_LEN + var_store_hdr_size
    return blob


def build_ftw_working_header(ftw_working_size: int) -> bytes:
    write_queue_size = ftw_working_size - FTW_WORKING_HEADER_SIZE
    assert write_queue_size >= 0, "FtwWorkingSize too small for the header alone"

    header = (
        guid_bytes(FTW_WORKING_GUID)
        + struct.pack("<I", 0)          # Crc placeholder
        + FTW_STATE_BYTES
        + struct.pack("<Q", write_queue_size)
    )
    crc = zlib.crc32(header[0:16] + b"\x00\x00\x00\x00" + header[20:]) & 0xFFFFFFFF
    header = header[0:16] + struct.pack("<I", crc) + header[20:]
    assert len(header) == FTW_WORKING_HEADER_SIZE
    return header


def has_valid_header(flash_image: str, offset: int, region_size: int) -> bool:
    """Best-effort check for an existing, already-formatted store at
    offset — same checks NorFlashFvb.c's ValidateFvHeader() makes for
    Signature/FileSystemGuid/FvLength. Used to make patching idempotent:
    a rebuild that overwrites SBSA_FLASH0.fd's BL1/FIP content shouldn't
    also stomp on variables a previous boot persisted into this region."""
    with open(flash_image, "rb") as f:
        f.seek(offset)
        header = f.read(FVH_HEADER_LEN)

    if len(header) < FVH_HEADER_LEN:
        return False

    signature = header[0x28:0x2C]
    fv_length = struct.unpack_from("<Q", header, 0x20)[0]
    fs_guid = uuid.UUID(bytes_le=header[0x10:0x20])

    return (signature == FVH_SIGNATURE) and (fv_length == region_size) and (fs_guid == FV_GUID)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("flash_image", help="Flash image file to patch in place")
    ap.add_argument("--offset", type=lambda x: int(x, 0), required=True,
                     help="File offset of the Variable region (hex OK, e.g. 0x01000000)")
    ap.add_argument("--var-size", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--ftw-working-size", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--ftw-spare-size", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--force", action="store_true",
                     help="Overwrite even if a valid store already exists at --offset "
                          "(default: skip, to preserve variables persisted by a previous boot)")
    args = ap.parse_args()

    region_size = args.var_size + args.ftw_working_size + args.ftw_spare_size

    if not args.force and has_valid_header(args.flash_image, args.offset, region_size):
        print(f"✓ Valid variable store already present at {hex(args.offset)} in "
              f"{args.flash_image} — leaving it alone (pass --force to reformat)")
        return 0

    var_blob = build_variable_store(region_size, args.var_size)
    if len(var_blob) > args.var_size:
        print(f"ERROR: variable store header ({len(var_blob)} bytes) doesn't fit "
              f"in --var-size {args.var_size}", file=sys.stderr)
        return 1

    ftw_blob = build_ftw_working_header(args.ftw_working_size)

    with open(args.flash_image, "r+b") as f:
        f.seek(args.offset)
        f.write(var_blob)

        f.seek(args.offset + args.var_size)
        f.write(ftw_blob)
        # Rest of FtwWorking, and all of FtwSpare, stay whatever the file
        # already had there (expected: erased/0xFF, same as the reference
        # leaves NV_FTW_SPARE with no DATA block at all).

    print(f"✓ Patched empty variable store at offset {hex(args.offset)} "
          f"in {args.flash_image} (region size {hex(region_size)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
