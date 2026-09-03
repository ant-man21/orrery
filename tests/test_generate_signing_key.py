"""Unit tests for tools/generate_signing_key.py (issue #24).

The generated header is a compile-time constant baked into both TPM apps, and
its wire-marshalled Name is what gets locked into the NV index's authPolicy
permanently. A malformed field here doesn't fail the build — it fails at
provisioning time on a real TPM, which is a much worse place to find out.
"""

import re
import subprocess

import pytest


def _modulus_bytes_from_header(header_text: str) -> bytes:
    """Pull the .buffer = { 0x.., ... } byte list back out of the C header."""
    match = re.search(r"\.buffer\s*=\s*\{(.*?)\}", header_text, re.DOTALL)
    assert match, "header has no .buffer byte array"
    return bytes(int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", match.group(1)))


# ── header shape ──────────────────────────────────────────────────────────────

def test_header_has_expected_tpm2b_public_fields(generate_signing_key):
    header = generate_signing_key.build_header(bytes(range(256)))
    for field in [
        ".type              = TPM_ALG_RSA",
        ".nameAlg           = TPM_ALG_SHA256",
        ".sign         = 1",
        ".userWithAuth = 1",
        ".keyBits  = 2048",
        ".exponent = 0",
    ]:
        assert field in header, f"missing {field!r}"


def test_header_declares_no_scheme_on_the_key(generate_signing_key):
    """An unrestricted verification key must leave scheme = TPM_ALG_NULL, so
    the scheme comes from the TPMT_SIGNATURE at verify time. Pinning a scheme
    here would make Tpm2VerifySignature reject the ticket."""
    header = generate_signing_key.build_header(bytes(256))
    assert header.count("TPM_ALG_NULL") == 2  # symmetric.algorithm + scheme.scheme


def test_header_is_include_guarded_and_self_contained(generate_signing_key):
    header = generate_signing_key.build_header(bytes(256))
    assert header.count("TRUSTED_UPDATE_KEY_H_") == 3  # ifndef + define + endif
    assert "#include <IndustryStandard/Tpm20.h>" in header
    assert "do not hand-edit" in header


def test_header_round_trips_the_modulus_exactly(generate_signing_key):
    modulus = bytes(range(256))
    assert _modulus_bytes_from_header(generate_signing_key.build_header(modulus)) == modulus


def test_header_declares_the_real_modulus_length(generate_signing_key):
    header = generate_signing_key.build_header(bytes(range(256)))
    assert ".size = 256" in header
    assert len(_modulus_bytes_from_header(header)) == 256


def test_byte_array_formatting_is_12_per_line(generate_signing_key):
    """Cosmetic, but it's what makes the generated header reviewable in a diff
    — one changed byte should not reflow the whole block."""
    lines = generate_signing_key.format_c_byte_array(bytes(range(24))).splitlines()
    assert len(lines) == 2
    assert lines[0].count("0x") == 12
    assert all(line.rstrip().endswith(",") for line in lines)


# ── overwrite guard ───────────────────────────────────────────────────────────

def test_refuses_to_overwrite_an_existing_key(generate_signing_key, tmp_path):
    """Regenerating orphans every device already provisioned against the old
    public half — that has to be a deliberate act, not an accident."""
    key = tmp_path / "existing.pem"
    key.write_text("not really a key, but it exists")
    header = tmp_path / "TrustedUpdateKey.h"

    with pytest.raises(SystemExit) as exc:
        generate_signing_key.main(["--key-out", str(key), "--header-out", str(header)])

    assert "refusing to overwrite" in str(exc.value)
    assert key.read_text() == "not really a key, but it exists"
    assert not header.exists(), "must not write a header it has no matching key for"


def test_force_overwrites_an_existing_key(generate_signing_key, tmp_path, openssl_required):
    key = tmp_path / "existing.pem"
    key.write_text("placeholder")
    header = tmp_path / "TrustedUpdateKey.h"

    generate_signing_key.main(
        ["--key-out", str(key), "--header-out", str(header), "--force"]
    )

    assert "BEGIN PRIVATE KEY" in key.read_text()
    assert header.exists()


# ── generated key <-> generated header agreement ──────────────────────────────

def test_generated_header_modulus_matches_the_generated_key(signing_key, openssl_required):
    """The whole scheme collapses if the header's modulus isn't the public half
    of the key the tickets get signed with."""
    key, header = signing_key

    modulus_line = subprocess.run(
        ["openssl", "rsa", "-in", str(key), "-noout", "-modulus"],
        capture_output=True, text=True, check=True,
    ).stdout.strip()
    expected = bytes.fromhex(modulus_line.removeprefix("Modulus="))

    assert _modulus_bytes_from_header(header.read_text()) == expected
    assert len(expected) == 256


def test_generated_key_is_rsa_2048_with_exponent_65537(signing_key, openssl_required):
    key, _ = signing_key
    text = subprocess.run(
        ["openssl", "rsa", "-in", str(key), "-noout", "-text"],
        capture_output=True, text=True, check=True,
    ).stdout
    assert "publicExponent: 65537" in text
    assert "2048 bit" in text


def test_private_key_is_not_world_readable(signing_key):
    key, _ = signing_key
    assert key.stat().st_mode & 0o077 == 0, "private key must be chmod 600"
