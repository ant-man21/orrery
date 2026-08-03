# Testing

Three tiers, three independent CI checks. They answer different questions and
fail for different reasons, which is exactly why they aren't one job:

| Tier | CI check | Question it answers | Runtime |
|------|----------|--------------------|---------|
| Compile | `Build (q35)` / `Build (arm-virt)` | Does the firmware build for both platforms? | ~minutes |
| Unit | `Unit tests (offline tooling)` | Is the offline signing math right? | ~1 second |
| Boot | `Boot test (q35)` | Does it actually boot and provision against a real TPM? | ~minutes |

Before any of this existed, CI only ever compile-checked. Both real bugs caught
in #16's review — the `Tpm2PolicyAuthorize` hierarchy issue and the arm-virt
ticket-signing offset issue — compiled perfectly clean and were only found
because a human built, booted, typed shell commands by hand, and pasted logs
back. That's the gap these two new tiers close.

---

## Unit tests (issue #24)

Pure host-side Python. No TPM, no QEMU, no EDK2 build — just `python3` and
`openssl`, both of which the tools under test already require.

```bash
python3 -m pip install pytest
python3 -m pytest tests/ -v
```

### What's covered

**`tools/sign_rom_ticket.py`**
- The 5-layer hash chain (`inner` → `pcr16` → `pcrdig` → `approved` → `aHash`)
  against golden vectors derived independently from the TPM spec formulas,
  rather than recorded from the implementation's own output. A
  self-consistency check would pass even if the whole chain were wrong.
- `TPML_PCR_SELECTION` wire bytes, including bit placement across byte
  boundaries (PCR 0/7/8/16/23). These must match `Tpm2PcrLib`'s
  `BuildPcrSelection()` exactly — if they drift, every ticket silently fails
  verification on real hardware.
- **`--offset`/`--length` slicing** — the arm-virt regression from #24.
  Signing `file[0x1000:0x1000+N]` must produce a byte-identical ticket to
  signing a file containing only those bytes. RSASSA-PKCS#1 v1.5 is
  deterministic, so comparing tickets directly proves which bytes actually got
  hashed without reaching into the implementation.
- The self-check that stops a bad ticket reaching a device: corrupted
  signatures and signatures over the wrong digest are both rejected.
- `--out` being required with no platform-specific default.

**`tools/generate_signing_key.py`**
- Generated `TPM2B_PUBLIC` header shape: `TPM_ALG_RSA`, `TPM_ALG_SHA256`,
  `sign`/`userWithAuth` set, `keyBits`/`exponent`, `scheme = TPM_ALG_NULL`
  (an unrestricted verification key must not pin a scheme, or
  `Tpm2VerifySignature` rejects the ticket).
- The header's modulus really is the public half of the generated private key.
- The overwrite guard, since regenerating orphans every device already
  provisioned against the old public half.

### These tests have teeth

Verified by mutation testing — each of these was introduced deliberately and
the suite caught all three:

| Mutation | Caught by |
|----------|-----------|
| Ignore `--offset`, hash the whole file (**the actual arm-virt bug**) | `test_cli_offset_length_produces_same_ticket_as_presliced_file` |
| Drop the `pcrdig` layer from the chain | `test_compute_a_hash_matches_golden_vectors` |
| Reverse PCR bit placement within a byte | 5 × `test_pcr_selection_bit_placement` + golden vectors |

---

## Headless boot test (issue #25)

Builds the firmware, boots it under QEMU with no display and no human, runs a
TPM app automatically, and greps the captured serial log for a pass/fail
result.

```bash
./tools/boot_test.sh                      # provision, then verify
./tools/boot_test.sh --phase provision    # provisioning only (what CI runs)
./tools/boot_test.sh --skip-build         # reuse whatever is in Build/
./tools/boot_test.sh --timeout 120        # per-boot wall-clock bound
```

Requires `qemu-system-x86_64`, `swtpm`, `mtools`, and the usual EDK2 build
dependencies. The script checks for all of them up front and names whatever's
missing rather than dying halfway through a build.

### How it works

1. **Reset state** — `tools/tpm_reset.sh` clears swtpm's NVRAM and the UEFI
   variable store is reseeded, so every run starts from a known-clean TPM.
   swtpm is killed *before* its state is deleted; a live swtpm flushes its own
   NVRAM back out on exit and would silently un-reset what was just cleared.
2. **Build** — `Q35Pkg/build.sh`, which also signs this exact build's ROM into
   `shared/data/rom.ticket` and pushes the apps onto `shared.img`.
3. **Generate `startup.nsh`** — the UEFI shell has always looked for one and
   printed *"Press ESC in 5 seconds to skip startup.nsh"*; nothing was ever
   providing it, which is why every boot until now needed a human to type
   `fs1:`, `cd apps`, `<app>.efi`. The generated script probes `fs0:`–`fs4:`
   for the app rather than hardcoding `fs1:`, because volume enumeration order
   is explicitly not stable in this project.
4. **Boot headless** — `qemu.sh --headless` (see below), bounded by `timeout`.
5. **Grep** — ANSI escapes are stripped from the serial log first, so a plain
   string match can't miss a sentinel that happens to carry a colour code.
   Exit code reflects the result.

### End-of-test signal

`startup.nsh` ends with `reset -s`, which powers the VM off as soon as the app
returns. Without it the shell drops back to its prompt and sits there until the
timeout fires, so every run — pass or fail — would cost the full timeout. With
it, a passing run finishes in seconds.

The runner deliberately does **not** trust QEMU's exit code. A timeout (124)
with a passing sentinel already in the log is still a pass, and a clean exit
proves nothing about what the app actually did. The log is the source of truth.

### `qemu.sh --headless`

```
--headless          -display none, serial to a file, -no-reboot
--serial-log PATH   where to write it (default: <pkg>/serial.log)
--timeout SECS      host-side wall-clock bound (default: 300)
```

Interactive mode is unchanged: GUI window, `-serial stdio`, `tail -f` on
`debug.log`. Everything except the display/serial wiring is identical between
the two modes — a headless run has to exercise the same machine a human sees
when they boot it by hand, or it isn't testing the right thing.

Grep the **serial** log, not `debug.log`. `debug.log` only ever carries
`DEBUG()` output from the `-debugcon` port, which RELEASE builds leave empty;
it never carries what the UEFI shell or an app's `Print()` writes.

`-no-reboot` matters: without it a guest reset silently restarts the boot inside
the same QEMU process, so a crash-loop would burn the whole timeout and leave a
serial log with N interleaved boots in it.

### Signing keys

The firmware embeds a signing key's public half (`TrustedUpdateKey.h`) and the
ticket is signed with its private half — they must be the same keypair or
`Tpm2VerifySignature` rejects the ticket and provisioning fails.

If no key exists, `boot_test.sh` generates a throwaway one. **This rewrites the
tracked `OrreryPkg/Include/TrustedUpdateKey.h`**, so expect a dirty tree
afterwards:

```bash
git checkout -- OrreryPkg/Include/TrustedUpdateKey.h
```

The script warns about this before it happens. In CI it's harmless — the
checkout is disposable. If you already have a real key, nothing is touched.

`generate_signing_key.py` and `sign_rom_ticket.py` both take path overrides
(`--key-out`/`--header-out`, `--key`) so the test suite can drive them into a
temp dir instead of anywhere near the real key. The normal build flow always
uses the defaults.

---

## Known failure: `--phase verify`

**`--phase verify` and `--phase all` currently fail on main, and the failure is
real — not a harness bug.**

PR #16 re-gated the secret NV index's `authPolicy` on `TPM2_PolicyAuthorize`
(keyed to the signing key's Name) and rewrote `TpmProvisionApp`'s write path
around the full chain: `PolicyPCR` → `LoadExternal` → read + verify this boot's
ticket → `PolicyAuthorize`.

`TpmVerifyBootApp` was never brought onto that chain. It still opens a
PolicyPCR-only session — no `Tpm2PolicyAuthorizeLib` in its `.inf`, no ticket
read, no `PolicyAuthorize` call — so its session digest no longer satisfies the
index's `authPolicy` and `Tpm2NvRead` is refused. The app then reports that
refusal as:

```
[VERIFY] Unseal FAILED — BIOS modified, halting.
```

which is misleading. PCR[16] is byte-identical across both boots — the harness
prints both values, and they match:

```
[PROVISION] PCR[16] = FF67BB22E4F5C0A824664D04B06EDAC05C623259FFEB32F411302AA17F5A2B43
[VERIFY]    PCR[16] = FF67BB22E4F5C0A824664D04B06EDAC05C623259FFEB32F411302AA17F5A2B43
```

The firmware measurement is fine. The authorization path is incomplete. #16's
own commit message flags the chain as *"shared with what TpmVerifyBootApp's read
**will** use"* — future tense, never landed.

This was found by the boot test on its first full run, which is more or less
the entire argument for having one.

CI runs `--phase provision` until this is fixed. Widen it to `--phase all` once
`TpmVerifyBootApp` is on the same chain — wiring up a permanently-red check now
would just train people to ignore it.

---

## Not covered yet

- **arm-virt boot testing.** `qemu-system-aarch64` installs the same way, but
  TCG-emulating AArch64 on an x86 runner is slower, and `ArmVirtOrreryPkg` has
  no `-debugcon` (DEBUG output interleaves with the shell on one serial line).
  Worth its own spike before going in the required matrix.
- **The tamper-detection negative case.** Rebuild with a deliberate ROM change,
  re-run `TpmVerifyBootApp`, assert the unseal *fails*. This is the property
  most likely to regress silently if someone loosens the policy check, so it's
  the highest-value test to add after `--phase all` goes green.
- **C-level marshalling** (`Tpm2PolicyAuthorizeLib.c` wire formats). See #24 —
  either extract the byte-layout logic into host-buildable functions, or use
  the `UnitTestFrameworkPkg` already vendored in `edk2`.
