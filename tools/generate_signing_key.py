#!/usr/bin/env python3
"""
generate_signing_key.py — one-time setup (N=0), run offline.

Generates the RSA-2048 update-signing keypair:
  - Private key -> tools/keys/update_signing_key.pem
    Never ships in firmware. Never touches the device. Keep this off any
    machine an attacker could reach.
  - Public key  -> OrreryPkg/Include/TrustedUpdateKey.h
    A TPM2B_PUBLIC C constant, compiled directly into TpmProvisionApp and
    TpmVerifyBootApp. This is what TPM2_LoadExternal loads on-device, and
    its wire-marshalled Name is what gets locked into the NV index's
    authPolicy at provisioning time — permanently, until the TPM is
    cleared. Re-running this script after that point orphans every
    already-provisioned device's vault (see the key-rotation caveat in
    issue #15 / the design doc).

Requirements:
  - Python 3 (stdlib only — no pip packages needed)
  - `openssl` on PATH (any reasonably recent build; this was developed
    against OpenSSL 3.0.13). That's the only external dependency — see
    below for why this doesn't use Python's `cryptography` package.

Shells out to `openssl` rather than using the `cryptography` package —
the latter's Rust bindings are broken in this environment
(ModuleNotFoundError: _cffi_backend / pyo3 panic on import). openssl is
confirmed present and does everything needed here.

Field choices, and why (all cross-checked against the vendored
SecurityPkg/Library/Tpm2CommandLib/Tpm2Object.c RSA marshalling code,
since this fork's Tpm20.h has TPM_ALG_RSA commented out — see
Tpm2PolicyAuthorizeLib.h for the same note):
  - type            = TPM_ALG_RSA
  - nameAlg         = TPM_ALG_SHA256    (must match the session hash alg
                                          used everywhere else in this flow)
  - objectAttributes.sign = 1            (verification-only key)
  - objectAttributes.userWithAuth = 1    (standard default; this key's own
                                          auth is never actually used)
  - objectAttributes.restricted = 0      (unrestricted — Tpm2VerifySignature
                                          isn't checking a TPM-internal hash
                                          sequence, it's checking a signature
                                          computed entirely offline here)
  - parameters.rsaDetail.scheme = TPM_ALG_NULL
                                          (unrestricted key -> scheme comes
                                          from the TPMT_SIGNATURE at verify
                                          time, not fixed on the object)
  - parameters.rsaDetail.exponent = 0    ("0" means "use the TPM's default,
                                          65537" — matches what openssl
                                          generates by default)
"""

import argparse
import pathlib
import subprocess

REPO_ROOT   = pathlib.Path(__file__).resolve().parent.parent
KEY_DIR     = REPO_ROOT / "tools" / "keys"
PRIVATE_KEY = KEY_DIR / "update_signing_key.pem"
HEADER_OUT  = REPO_ROOT / "OrreryPkg" / "Include" / "TrustedUpdateKey.h"

RSA_KEY_BITS  = 2048
RSA_KEY_BYTES = RSA_KEY_BITS // 8   # 256


def run(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def format_c_byte_array(data: bytes, indent: str = "    ") -> str:
    lines = []
    for i in range(0, len(data), 12):
        chunk = data[i : i + 12]
        lines.append(indent + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    return "\n".join(lines)


def build_header(modulus: bytes) -> str:
    """The TPM2B_PUBLIC C constant for this modulus.

    Split out of main() so the unit tests can assert on the generated text
    without shelling out to openssl or writing anywhere near the real
    OrreryPkg/Include/TrustedUpdateKey.h.
    """
    return HEADER_TEMPLATE.format(
        RSA_KEY_BITS=RSA_KEY_BITS,
        RSA_KEY_BYTES=RSA_KEY_BYTES,
        MODULUS_BYTES=format_c_byte_array(modulus),
    )


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--key-out", type=pathlib.Path, default=PRIVATE_KEY,
        help=f"Private key output path (default: {PRIVATE_KEY})",
    )
    parser.add_argument(
        "--header-out", type=pathlib.Path, default=HEADER_OUT,
        help=f"Generated C header output path (default: {HEADER_OUT}). Both "
             f"overrides exist mainly so the test suite can drive this script "
             f"into a temp dir — the real build only ever uses the defaults, "
             f"since TrustedUpdateKey.h has to be where the .inf includes it "
             f"from to actually end up compiled into the firmware.",
    )
    parser.add_argument(
        "--force", action="store_true",
        help="Overwrite an existing private key instead of refusing. Rotating "
             "the key orphans every device already provisioned against its "
             "public half — this flag exists for throwaway CI/test keys, not "
             "for production rotation.",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> None:
    args        = parse_args(argv)
    private_key = args.key_out
    header_out  = args.header_out

    if private_key.exists() and not args.force:
        raise SystemExit(
            f"{private_key} already exists — refusing to overwrite an "
            f"existing signing key (would orphan every device already "
            f"provisioned against its public half). Delete it manually "
            f"first, or pass --force, if you really mean to rotate keys."
        )

    private_key.parent.mkdir(parents=True, exist_ok=True)

    run(
        "openssl", "genpkey", "-algorithm", "RSA",
        "-pkeyopt", f"rsa_keygen_bits:{RSA_KEY_BITS}",
        "-out", str(private_key),
    )
    private_key.chmod(0o600)

    exponent_text = run("openssl", "rsa", "-in", str(private_key), "-noout", "-text")
    if "publicExponent: 65537" not in exponent_text:
        raise SystemExit(
            "Generated key has a non-65537 public exponent — "
            "TrustedUpdateKey.h's exponent=0 shortcut assumes 65537. "
            "Aborting rather than emit a mismatched header."
        )

    modulus_line = run("openssl", "rsa", "-in", str(private_key), "-noout", "-modulus").strip()
    modulus_hex  = modulus_line.removeprefix("Modulus=")
    modulus      = bytes.fromhex(modulus_hex)
    if len(modulus) != RSA_KEY_BYTES:
        raise SystemExit(f"Modulus is {len(modulus)} bytes, expected {RSA_KEY_BYTES} — aborting.")

    header_out.parent.mkdir(parents=True, exist_ok=True)
    header_out.write_text(build_header(modulus))

    print(f"Private key : {private_key}  (chmod 600, gitignored — do not commit)")
    print(f"Public key  : {header_out}")
    print(f"Modulus     : {RSA_KEY_BYTES} bytes, exponent 65537")


HEADER_TEMPLATE = """/** @file
  TrustedUpdateKey.h — compiled-in update-signing public key.

  GENERATED by tools/generate_signing_key.py — do not hand-edit.

  This is the ONLY thing that gates the vault, forever, once
  TpmProvisionApp's trial session locks its Name into the NV index's
  authPolicy. There is deliberately no way to store or swap this key at
  runtime (see issue #15) — it is a compile-time constant so that
  TPM2_LoadExternal always produces the same Name, on every boot, on
  every device built from this source.
**/

#ifndef TRUSTED_UPDATE_KEY_H_
#define TRUSTED_UPDATE_KEY_H_

#include <IndustryStandard/Tpm20.h>

STATIC TPM2B_PUBLIC  gTrustedUpdateKey = {{
  .size = 0,   // unused on the wire — Tpm2LoadExternal computes the
               // marshalled length itself, same convention as this
               // codebase's other Tpm2NvDefineSpace callers.
  .publicArea = {{
    .type              = TPM_ALG_RSA,
    .nameAlg           = TPM_ALG_SHA256,
    .objectAttributes  = {{
      .sign         = 1,
      .userWithAuth = 1,
    }},
    .authPolicy = {{
      .size = 0,
    }},
    .parameters = {{
      .rsaDetail = {{
        .symmetric = {{
          .algorithm = TPM_ALG_NULL,
        }},
        .scheme = {{
          .scheme = TPM_ALG_NULL,
        }},
        .keyBits  = {RSA_KEY_BITS},
        .exponent = 0,   // 0 == TPM default (65537) — matches this key
      }},
    }},
    .unique = {{
      .rsa = {{
        .size = {RSA_KEY_BYTES},
        .buffer = {{
{MODULUS_BYTES}
        }},
      }},
    }},
  }},
}};

#endif // TRUSTED_UPDATE_KEY_H_
"""


if __name__ == "__main__":
    main()
