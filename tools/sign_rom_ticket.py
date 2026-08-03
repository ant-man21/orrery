#!/usr/bin/env python3
"""
sign_rom_ticket.py — run once per firmware build, offline.

Computes the exact digest TpmVerifyBootApp / TpmProvisionApp will produce
on-device after Tpm2PolicyPCR + Tpm2PolicyGetDigest for this ROM image,
then signs it, producing the ticket file that ships alongside this ROM.

The chain (5 hash layers — see issue #15 for why it's not just
SHA256(rom)):

  inner    = SHA256(rom_bytes)
             what Tcg2->HashLogExtendEvent hashes when measuring the ROM

  pcr_ext  = SHA256(0x00*32 || inner)
             PCR15's value after that one extend, from a freshly-reset PCR
             (PCR15 chosen over PCR16 for non-resettability — see issue #27)

  pcrdig   = SHA256(pcr_ext)
             TPM2_PolicyPCR folds in PCRComputeCurrentDigest(), which is a
             hash of the concatenated selected PCR values — even with one
             PCR selected, this extra layer is real. Confirmed against
             ms-tpm-20-ref's PolicyPCR.c.

  approved = SHA256(0x00*32 || TPM_CC_PolicyPCR || pcr_selection || pcrdig)
             the session-digest chaining formula TPM2_PolicyPCR applies:
             hash(oldDigest=0 || commandCode || marshalled TPML_PCR_SELECTION
             || pcrDigest). This is exactly what Tpm2PolicyGetDigest reads
             back on-device — NOT the raw PCR15 value.

  aHash    = SHA256(approved || policyRef)
             what TPM2_PolicyAuthorize actually checks the signature
             against — approvedPolicy concatenated with policyRef (empty,
             here), then hashed once more. Confirmed against
             ms-tpm-20-ref's PolicyAuthorize.c.

ticket = RSASSA-PKCS1v1.5-SHA256(private_key, aHash)   — 256 raw bytes,
no TPM wire-format wrapping. TpmVerifyBootApp reads these bytes directly
into TPMT_SIGNATURE.signature.rsassa.sig.buffer.

pcr_selection matches this codebase's BuildPcrSelection(15, ...)
(OrreryPkg/Library/Tpm2PcrLib/Tpm2PcrLib.c) exactly: count=1,
hash=TPM_ALG_SHA256, sizeofSelect=3, pcrSelect=[0x00, 0x80, 0x00]
(PCR15 -> byte 15/8=1, bit 15%8=7).
"""

import argparse
import hashlib
import pathlib
import struct
import subprocess

REPO_ROOT   = pathlib.Path(__file__).resolve().parent.parent
PRIVATE_KEY = REPO_ROOT / "tools" / "keys" / "update_signing_key.pem"

TPM_CC_POLICY_PCR = 0x0000017F
TPM_ALG_SHA256    = 0x000B
PCR_FOR_BIOS      = 15

# TPML_PCR_SELECTION wire bytes for {count=1, hash=SHA256, sizeofSelect=3,
# pcrSelect=[selecting PCR_FOR_BIOS]} — must match BuildPcrSelection().
def pcr_selection_bytes(pcr_index: int) -> bytes:
    pcr_select = bytearray(3)
    pcr_select[pcr_index // 8] = 1 << (pcr_index % 8)
    return (
        struct.pack(">I", 1)                 # count = 1
        + struct.pack(">H", TPM_ALG_SHA256)  # hash
        + bytes([3])                          # sizeofSelect
        + bytes(pcr_select)
    )


def compute_a_hash(rom_bytes: bytes) -> tuple[bytes, bytes, bytes, bytes, bytes]:
    inner    = hashlib.sha256(rom_bytes).digest()
    pcr_ext  = hashlib.sha256(b"\x00" * 32 + inner).digest()
    pcrdig   = hashlib.sha256(pcr_ext).digest()
    approved = hashlib.sha256(
        b"\x00" * 32
        + struct.pack(">I", TPM_CC_POLICY_PCR)
        + pcr_selection_bytes(PCR_FOR_BIOS)
        + pcrdig
    ).digest()
    a_hash = hashlib.sha256(approved).digest()   # policyRef is empty
    return inner, pcr_ext, pcrdig, approved, a_hash


def sign(a_hash: bytes) -> bytes:
    if not PRIVATE_KEY.exists():
        raise SystemExit(
            f"{PRIVATE_KEY} not found — run tools/generate_signing_key.py first."
        )
    result = subprocess.run(
        [
            "openssl", "pkeyutl", "-sign",
            "-inkey", str(PRIVATE_KEY),
            "-pkeyopt", "digest:sha256",
        ],
        input=a_hash,
        capture_output=True,
        check=True,
    )
    return result.stdout


def self_check(a_hash: bytes, signature: bytes) -> None:
    """Verify offline before ever letting this ticket near a device."""
    pubkey = subprocess.run(
        ["openssl", "rsa", "-in", str(PRIVATE_KEY), "-pubout"],
        capture_output=True, check=True,
    ).stdout
    # openssl pkeyutl needs three separate file inputs (digest, pubkey,
    # sigfile) — can't multiplex them all through stdin. Use a temp dir.
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        (tmp / "pub.pem").write_bytes(pubkey)
        (tmp / "digest.bin").write_bytes(a_hash)
        (tmp / "sig.bin").write_bytes(signature)
        result = subprocess.run(
            [
                "openssl", "pkeyutl", "-verify",
                "-pubin", "-inkey", str(tmp / "pub.pem"),
                "-pkeyopt", "digest:sha256",
                "-in", str(tmp / "digest.bin"),
                "-sigfile", str(tmp / "sig.bin"),
            ],
            capture_output=True, text=True,
        )
    if "Signature Verified Successfully" not in result.stdout:
        raise SystemExit(
            f"Self-check FAILED — refusing to write a ticket that doesn't "
            f"even verify against its own key.\n{result.stdout}\n{result.stderr}"
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=pathlib.Path, help="Path to the ROM image to sign")
    parser.add_argument(
        "-o", "--out", type=pathlib.Path, required=True,
        help="Output ticket path — e.g. <chip>/shared/data/rom.ticket. No "
             "default: every platform's build.sh must pass its own path "
             "explicitly, this script has no notion of which chip it's "
             "signing for.",
    )
    parser.add_argument(
        "--offset", type=lambda x: int(x, 0), default=0,
        help="Byte offset into ROM to start hashing from (default: 0, whole file). "
             "Must match exactly what PlatformRomInfoLib measures on-device — e.g. "
             "ArmVirtQemu's QEMU_EFI.fd has a 0x1000-byte SEC/reset-vector region "
             "before FVMAIN_COMPACT that isn't part of the measured region.",
    )
    parser.add_argument(
        "--length", type=lambda x: int(x, 0), default=None,
        help="Number of bytes to hash from --offset (default: rest of file).",
    )
    args = parser.parse_args()

    full_bytes = args.rom.read_bytes()
    end = None if args.length is None else args.offset + args.length
    rom_bytes = full_bytes[args.offset : end]
    if args.offset != 0 or args.length is not None:
        print(
            f"Slicing ROM: offset=0x{args.offset:x}, length={len(rom_bytes)} "
            f"(of {len(full_bytes)} total bytes in {args.rom})"
        )

    inner, pcr_ext, pcrdig, approved, a_hash = compute_a_hash(rom_bytes)
    signature = sign(a_hash)
    self_check(a_hash, signature)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(signature)

    def hx(b: bytes) -> str:
        return b.hex()

    print(f"ROM             : {args.rom}  ({len(rom_bytes)} bytes)")
    print(f"inner (SHA256)  : {hx(inner)}")
    print(f"pcr_ext         : {hx(pcr_ext)}")
    print(f"pcrdig          : {hx(pcrdig)}")
    print(f"approved        : {hx(approved)}   <- compare against on-device Tpm2PolicyGetDigest")
    print(f"aHash           : {hx(a_hash)}")
    print(f"signature       : {len(signature)} bytes, self-check OK")
    print(f"ticket written  : {args.out}")
    print()
    print("Q35Pkg/build.sh and ArmVirtOrreryPkg/build.sh both run this")
    print("automatically after each build and push the ticket onto shared.img")
    print("(fs1:\\data\\rom.ticket) alongside the synced apps — no manual step")
    print("needed unless you're signing a ROM outside the normal build flow.")


if __name__ == "__main__":
    main()
