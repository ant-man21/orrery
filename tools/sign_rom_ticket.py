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

  pcr16    = SHA256(0x00*32 || inner)
             PCR16's value after that one extend, from a freshly-reset PCR

  pcrdig   = SHA256(pcr16)
             TPM2_PolicyPCR folds in PCRComputeCurrentDigest(), which is a
             hash of the concatenated selected PCR values — even with one
             PCR selected, this extra layer is real. Confirmed against
             ms-tpm-20-ref's PolicyPCR.c.

  approved = SHA256(0x00*32 || TPM_CC_PolicyPCR || pcr_selection || pcrdig)
             the session-digest chaining formula TPM2_PolicyPCR applies:
             hash(oldDigest=0 || commandCode || marshalled TPML_PCR_SELECTION
             || pcrDigest). This is exactly what Tpm2PolicyGetDigest reads
             back on-device — NOT the raw PCR16 value.

  aHash    = SHA256(approved || policyRef)
             what TPM2_PolicyAuthorize actually checks the signature
             against — approvedPolicy concatenated with policyRef (empty,
             here), then hashed once more. Confirmed against
             ms-tpm-20-ref's PolicyAuthorize.c.

ticket = RSASSA-PKCS1v1.5-SHA256(private_key, aHash)   — 256 raw bytes,
no TPM wire-format wrapping. TpmVerifyBootApp reads these bytes directly
into TPMT_SIGNATURE.signature.rsassa.sig.buffer.

pcr_selection matches this codebase's BuildPcrSelection(16, ...)
(OrreryPkg/Library/Tpm2PcrLib/Tpm2PcrLib.c) exactly: count=1,
hash=TPM_ALG_SHA256, sizeofSelect=3, pcrSelect=[0x00, 0x00, 0x01]
(PCR16 -> byte 16/8=2, bit 16%8=0).
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
PCR_FOR_BIOS      = 16

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
    pcr16    = hashlib.sha256(b"\x00" * 32 + inner).digest()
    pcrdig   = hashlib.sha256(pcr16).digest()
    approved = hashlib.sha256(
        b"\x00" * 32
        + struct.pack(">I", TPM_CC_POLICY_PCR)
        + pcr_selection_bytes(PCR_FOR_BIOS)
        + pcrdig
    ).digest()
    a_hash = hashlib.sha256(approved).digest()   # policyRef is empty
    return inner, pcr16, pcrdig, approved, a_hash


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
        "-o", "--out", type=pathlib.Path,
        default=REPO_ROOT / "Q35Pkg" / "shared" / "data" / "rom.ticket",
        help="Output ticket path (default: Q35Pkg/shared/data/rom.ticket)",
    )
    args = parser.parse_args()

    rom_bytes = args.rom.read_bytes()
    inner, pcr16, pcrdig, approved, a_hash = compute_a_hash(rom_bytes)
    signature = sign(a_hash)
    self_check(a_hash, signature)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(signature)

    def hx(b: bytes) -> str:
        return b.hex()

    print(f"ROM             : {args.rom}  ({len(rom_bytes)} bytes)")
    print(f"inner (SHA256)  : {hx(inner)}")
    print(f"pcr16           : {hx(pcr16)}")
    print(f"pcrdig          : {hx(pcrdig)}")
    print(f"approved        : {hx(approved)}   <- compare against on-device Tpm2PolicyGetDigest")
    print(f"aHash           : {hx(a_hash)}")
    print(f"signature       : {len(signature)} bytes, self-check OK")
    print(f"ticket written  : {args.out}")
    print()
    print("NOTE: build.sh does not currently push Q35Pkg/shared/data/ onto")
    print("shared.img (it only auto-syncs shared/apps/). To get this ticket")
    print("onto the image for a boot test:")
    print(f'  mcopy -o -i Q35Pkg/shared.img "{args.out}" ::data/rom.ticket')


if __name__ == "__main__":
    main()
