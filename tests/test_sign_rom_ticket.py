"""Unit tests for tools/sign_rom_ticket.py (issue #24).

No TPM, no QEMU, no EDK2 build — this is all pure, deterministic host-side
logic, and it's the tier that should have caught the arm-virt ticket-signing
bug in seconds instead of via a real boot failure on real swtpm.
"""

import hashlib
import struct

import pytest

# Golden vectors for the input below, derived independently from the TPM spec
# formulas rather than by running the implementation and recording whatever it
# happened to print. A self-consistency check (recomputing with the same code
# under test) would pass even if the whole chain were wrong; these constants
# fail if anyone reorders, drops, or re-parameterises a layer.
GOLDEN_ROM = b"orrery-unit-test-rom"
GOLDEN = {
    "inner":    "145554d2ad093590f4c0a74d938f7ccb4d952c83ef943d055e0134e8df1b08db",
    "pcr16":    "328218c9d7bfac7bf3d50dc5bd28c90db566f7a2e5b6934b65a42bbf7e2b901d",
    "pcrdig":   "e7a4a541c0ca9a15769679b3d0455e0afb49b64fd0067497cf6ae38e076b1d7b",
    "approved": "990e5ff1d02a136de836e631442615d725a9bdc777bb8dea15eb1c42d165147f",
    "a_hash":   "7c9d6df3b4dfdc53e0ee7fec6e06c005cbffcc67088f964370237a69ef59a037",
}


# ── TPML_PCR_SELECTION wire format ────────────────────────────────────────────
# Must match OrreryPkg/Library/Tpm2PcrLib's BuildPcrSelection() exactly. If
# these drift apart the offline `approved` digest stops matching what the device
# computes, and every ticket silently fails verification on real hardware.

def test_pcr_selection_bytes_for_pcr16(sign_rom_ticket):
    got = sign_rom_ticket.pcr_selection_bytes(16)
    assert got == bytes.fromhex("00000001000b03000001")
    count, alg, size_of_select = struct.unpack(">IHB", got[:7])
    assert (count, alg, size_of_select) == (1, 0x000B, 3)  # 1 bank, SHA256, 3 bytes


@pytest.mark.parametrize(
    "pcr_index,expected_select",
    [
        (0,  "010000"),   # byte 0, bit 0
        (7,  "800000"),   # byte 0, bit 7  — last bit of the first byte
        (8,  "000100"),   # byte 1, bit 0  — first bit of the second byte
        (16, "000001"),   # byte 2, bit 0  — the one this project actually uses
        (23, "000080"),   # byte 2, bit 7  — highest PCR a 3-byte select can name
    ],
)
def test_pcr_selection_bit_placement(sign_rom_ticket, pcr_index, expected_select):
    """pcrSelect is little-endian by byte, LSB-first within each byte."""
    got = sign_rom_ticket.pcr_selection_bytes(pcr_index)
    assert got[7:].hex() == expected_select
    assert len(got) == 10


# ── the 5-layer hash chain ────────────────────────────────────────────────────

def test_compute_a_hash_matches_golden_vectors(sign_rom_ticket):
    inner, pcr16, pcrdig, approved, a_hash = sign_rom_ticket.compute_a_hash(GOLDEN_ROM)
    assert inner.hex()    == GOLDEN["inner"]
    assert pcr16.hex()    == GOLDEN["pcr16"]
    assert pcrdig.hex()   == GOLDEN["pcrdig"]
    assert approved.hex() == GOLDEN["approved"]
    assert a_hash.hex()   == GOLDEN["a_hash"]


def test_pcr16_layer_is_a_real_pcr_extend(sign_rom_ticket):
    """pcr16 must be extend(0x00*32, inner), i.e. what one TPM2_PCR_Extend
    against a freshly-reset PCR produces — not SHA256(rom) directly."""
    inner, pcr16, *_ = sign_rom_ticket.compute_a_hash(GOLDEN_ROM)
    assert pcr16 == hashlib.sha256(b"\x00" * 32 + inner).digest()
    assert pcr16 != inner


def test_a_hash_folds_in_an_empty_policy_ref(sign_rom_ticket):
    """TPM2_PolicyAuthorize checks SHA256(approvedPolicy || policyRef);
    policyRef is empty here, so aHash is a plain single hash of approved."""
    *_, approved, a_hash = sign_rom_ticket.compute_a_hash(GOLDEN_ROM)
    assert a_hash == hashlib.sha256(approved).digest()


def test_chain_is_sensitive_to_every_input_byte(sign_rom_ticket):
    a = sign_rom_ticket.compute_a_hash(b"\x00" * 64)[-1]
    b = sign_rom_ticket.compute_a_hash(b"\x00" * 63 + b"\x01")[-1]
    assert a != b


# ── --offset / --length slicing (the arm-virt regression) ─────────────────────
# ArmVirtQemu's QEMU_EFI.fd carries a 0x1000-byte SEC/reset-vector region ahead
# of FVMAIN_COMPACT that PlatformRomInfoLibArmVirt never measures. The signer
# hashing the whole file meant it hashed 4096 bytes the device doesn't see —
# guaranteeing a mismatch regardless of tampering.

def _write_multi_region_rom(path):
    """A file whose regions are individually identifiable, so a wrong slice
    produces a different digest instead of accidentally matching."""
    head = b"\xAA" * 0x1000        # SEC / reset vector — NOT measured on arm-virt
    body = bytes(range(256)) * 32  # 0x2000 bytes of measured firmware
    tail = b"\xBB" * 0x800         # padding past the measured region
    path.write_bytes(head + body + tail)
    return head, body, tail


def test_offset_and_length_hash_exactly_the_named_slice(sign_rom_ticket, tmp_path):
    rom = tmp_path / "QEMU_EFI.fd"
    head, body, tail = _write_multi_region_rom(rom)

    sliced = sign_rom_ticket.compute_a_hash(
        rom.read_bytes()[len(head) : len(head) + len(body)]
    )
    assert sliced == sign_rom_ticket.compute_a_hash(body)
    # ...and is genuinely different from hashing the whole file, which is
    # exactly the bug that shipped.
    assert sliced != sign_rom_ticket.compute_a_hash(head + body + tail)


def test_cli_offset_length_produces_same_ticket_as_presliced_file(
    sign_rom_ticket, signing_key, tmp_path, openssl_required
):
    """End-to-end through main(): signing file[0x1000:0x1000+len] must equal
    signing a file that contains only those bytes.

    RSASSA-PKCS1v1.5 is deterministic, so identical inputs give byte-identical
    signatures — comparing the two tickets directly proves which bytes actually
    got hashed, without reaching into the implementation.
    """
    key, _ = signing_key
    rom = tmp_path / "QEMU_EFI.fd"
    head, body, _ = _write_multi_region_rom(rom)

    presliced = tmp_path / "body-only.fd"
    presliced.write_bytes(body)

    sliced_ticket = tmp_path / "sliced.ticket"
    whole_ticket = tmp_path / "whole.ticket"

    sign_rom_ticket.main([
        str(rom), "--key", str(key), "--out", str(sliced_ticket),
        "--offset", hex(len(head)), "--length", hex(len(body)),
    ])
    sign_rom_ticket.main([str(presliced), "--key", str(key), "--out", str(whole_ticket)])

    assert sliced_ticket.read_bytes() == whole_ticket.read_bytes()
    assert len(sliced_ticket.read_bytes()) == 256  # RSA-2048


def test_cli_default_offset_hashes_the_whole_file(
    sign_rom_ticket, signing_key, tmp_path, openssl_required
):
    """Q35 passes no --offset/--length; that path must stay whole-file."""
    key, _ = signing_key
    rom = tmp_path / "OVMF_CODE.fd"
    rom.write_bytes(b"\xCD" * 4096)

    default_ticket = tmp_path / "default.ticket"
    explicit_ticket = tmp_path / "explicit.ticket"

    sign_rom_ticket.main([str(rom), "--key", str(key), "--out", str(default_ticket)])
    sign_rom_ticket.main([
        str(rom), "--key", str(key), "--out", str(explicit_ticket),
        "--offset", "0", "--length", "4096",
    ])
    assert default_ticket.read_bytes() == explicit_ticket.read_bytes()


def test_length_past_end_of_file_is_clamped_not_padded(sign_rom_ticket, tmp_path):
    """Python slicing clamps; assert that rather than silently zero-padding,
    since a padded hash would differ from what the device measures."""
    rom = tmp_path / "short.fd"
    rom.write_bytes(b"\x11" * 100)
    data = rom.read_bytes()[0:9999]
    assert len(data) == 100
    assert sign_rom_ticket.compute_a_hash(data) == sign_rom_ticket.compute_a_hash(b"\x11" * 100)


# ── CLI contract ──────────────────────────────────────────────────────────────

def test_out_is_required(sign_rom_ticket, tmp_path):
    """--out deliberately has no default: the script is chip-agnostic and must
    not guess a path for whichever platform happens to be calling it."""
    rom = tmp_path / "rom.fd"
    rom.write_bytes(b"\x00" * 16)
    with pytest.raises(SystemExit) as exc:
        sign_rom_ticket.main([str(rom)])
    assert exc.value.code != 0


def test_missing_key_is_a_clean_error_not_a_traceback(sign_rom_ticket, tmp_path):
    rom = tmp_path / "rom.fd"
    rom.write_bytes(b"\x00" * 16)
    with pytest.raises(SystemExit) as exc:
        sign_rom_ticket.main([
            str(rom), "--key", str(tmp_path / "nope.pem"),
            "--out", str(tmp_path / "t.ticket"),
        ])
    assert "generate_signing_key" in str(exc.value)


def test_ticket_is_written_and_verifies_against_its_own_key(
    sign_rom_ticket, signing_key, tmp_path, openssl_required
):
    key, _ = signing_key
    rom = tmp_path / "rom.fd"
    rom.write_bytes(GOLDEN_ROM)
    out = tmp_path / "nested" / "dir" / "rom.ticket"

    sign_rom_ticket.main([str(rom), "--key", str(key), "--out", str(out)])

    assert out.exists(), "should create parent directories"
    # self_check() runs inside main() and raises SystemExit on mismatch, so
    # reaching here already proves it verified — re-run it explicitly anyway so
    # a future refactor that drops the self-check gets caught here.
    a_hash = sign_rom_ticket.compute_a_hash(GOLDEN_ROM)[-1]
    sign_rom_ticket.self_check(a_hash, out.read_bytes(), key)


def test_self_check_rejects_a_corrupted_signature(
    sign_rom_ticket, signing_key, openssl_required
):
    """The guard that stops a bad ticket from ever reaching a device."""
    key, _ = signing_key
    a_hash = sign_rom_ticket.compute_a_hash(GOLDEN_ROM)[-1]
    good = sign_rom_ticket.sign(a_hash, key)

    corrupted = bytearray(good)
    corrupted[0] ^= 0xFF
    with pytest.raises(SystemExit):
        sign_rom_ticket.self_check(a_hash, bytes(corrupted), key)


def test_self_check_rejects_a_signature_over_a_different_digest(
    sign_rom_ticket, signing_key, openssl_required
):
    key, _ = signing_key
    a_hash = sign_rom_ticket.compute_a_hash(GOLDEN_ROM)[-1]
    other = sign_rom_ticket.compute_a_hash(b"a different rom")[-1]
    signature = sign_rom_ticket.sign(other, key)
    with pytest.raises(SystemExit):
        sign_rom_ticket.self_check(a_hash, signature, key)


def test_signing_is_deterministic(sign_rom_ticket, signing_key, openssl_required):
    """PKCS#1 v1.5 has no randomness — relied on by the slicing test above."""
    key, _ = signing_key
    a_hash = sign_rom_ticket.compute_a_hash(GOLDEN_ROM)[-1]
    assert sign_rom_ticket.sign(a_hash, key) == sign_rom_ticket.sign(a_hash, key)
