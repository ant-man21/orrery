# TPM Anti-Tampering / OTA Roadmap

Project: measured-boot anti-tampering for a custom Q35/OVMF platform, using a
TPM 2.0 to bind a secret to the state of the running firmware (PCR[16]), so
that a modified BIOS can no longer unlock it. Long-term goal is to extend this
into a signed OTA firmware update flow.

This doc is written to be dropped into a fresh session as the starting prompt.
It captures current status, near-term work, and the longer arc, so a new
conversation doesn't need this project's full history to be productive.

## Where things stand today

- **Platform**: custom `Q35Pkg` OVMF build (`Q35Pkg/Q35Pkg.dsc`), built via
  `Q35Pkg/build.sh`, run via `Q35Pkg/qemu.sh`. Host machine runs the x86_64
  guest fully emulated under QEMU/TCG (host is aarch64 — no KVM).
- **`TpmProvisionApp`** (`Q35Pkg/Drivers/TpmProvisionApp/`) — UEFI application,
  run manually from the shell (`fs1:\apps\TpmProvisionApp.efi`). Implements
  the full "Setup First Boot" flow: locate `EFI_TCG2_PROTOCOL`, print
  capabilities, snapshot the running ROM (OVMF flash at `0xFFC00000`, 4MB)
  and extend PCR[16] with its hash, compute the PCR[16] policy digest via a
  trial session, define an NV index gated on that policy, and write the
  secret into it via a real policy session. Meant to run once, against a
  wiped/clean TPM.
- **`TpmVerifyBootApp`** (`Q35Pkg/Drivers/TpmVerifyBootApp/`) — companion app,
  implements the full "Reboot / Verify" flow: measure the live ROM, extend
  PCR[16], open a real policy session, and read the secret back out of the
  NV index — which only succeeds if PCR[16] still matches what was locked
  in at provisioning time. No golden/reference image is used or needed;
  real hardware never has one to fall back to, so this app always measures
  the live ROM (see "Demo flow" below — the earlier `dummy.fd` dev aid was
  removed as unreliable and unnecessary).
- **Dev tooling**: `build.sh` builds + syncs `.efi`s into `shared/apps/` +
  pushes them into `shared.img` in place (no wipe). `post-run.sh` pulls
  `fs1:\data\` back out of `shared.img` onto the host after a run.
  `qemu.sh --reset-shared` still exists as an explicit full-wipe/start-over
  button, separate from normal build/run.

## Demo flow

1. `TpmProvisionApp` runs **once**, against a wiped/clean TPM: it measures
   the ROM, extends PCR[16], and defines + seals the secret into the NV
   index gated on that PCR[16] value.
2. Every subsequent boot runs `TpmVerifyBootApp` instead, against the
   **live ROM only** — it re-measures whatever firmware is actually
   running and re-derives PCR[16], then tries to read the secret back out.
   A match means the firmware hasn't changed since provisioning; a
   mismatch means it has, and the unseal fails.
3. Tamper detection is exercised by rebuilding/flashing a *different* ROM
   (e.g. add or remove a driver from the FV) and re-running
   `TpmVerifyBootApp` — expect the unseal to fail. Flash the original ROM
   back to confirm it succeeds again. This has been validated end-to-end.

**Open question, carried forward to [#5](https://github.com/ant-man21/orrery/issues/5):**
what's the re-provisioning story when the ROM legitimately changes (a real
firmware update)? Running `TpmProvisionApp` a second time against an
already-defined NV index currently fails the write step silently if
PCR[16] changed, since the index is still gated on the old policy. Phase 3
below sketches a resealing flow for this, but it isn't built yet.

## Gotchas already paid for (don't re-learn these)

- **`Tpm2PcrRead`'s `PcrSelectionOut` param is never NULL-checked** by its
  implementation. Passing `NULL` compiles fine but is a real null deref;
  under this build's LTO/`-O2` it got compiled into a `ud2` trap, surfacing
  as a `#UD` exception instead of the expected `#PF`. Always pass a real
  buffer for every `OUT` param in this library, even ones you don't need.
- **`Tpm2DeviceLibTcg2`'s `Tpm2SubmitCommand` assumes `Tpm2RequestUseTpm()`
  was already called** ("Assume when Tcg2 Protocol is ready, RequestUseTpm
  already done" — see its source comment). Call it before any raw
  `Tpm2CommandLib` command or you get undefined transport-level behavior.
- **This repo's vendored `SecurityPkg/Library/Tpm2CommandLib` is missing
  several commands** that a previous AI assumed existed (`Tpm2CreatePrimary`,
  `Tpm2Create`, `Tpm2Load`, `Tpm2Unseal`, `Tpm2PolicyPCR`, `Tpm2GetRandom`,
  `Tpm2ComputePolicyPCRDigest`). Verified by checking this repo's own git
  history against the real upstream tianocore commit — none of these ever
  existed, upstream or here. **`Tpm2PolicyPCR` is the only one of these we
  still need** (see Phase 2) — everything else needed is already implemented
  in this codebase (`Tpm2StartAuthSession`, `Tpm2PolicyGetDigest`,
  `Tpm2NvDefineSpace`, `Tpm2NvWrite`, `Tpm2NvRead`, `Tpm2FlushContext`).
  Before assuming any other `Tpm2*` function exists, grep for it — don't
  trust that a plausible name means real code.
- **fs0: vs fs1: enumeration order is not stable.** `TpmProvisionApp`
  identifies the shared volume by its FAT label (`"SHARED"`, matching
  `mformat -v SHARED` in `qemu.sh`) rather than assuming a handle index —
  do this for any new file I/O too.
- **`shared.img` / `uefi-shell.img` are static disk-image files, not a live
  host-folder mount.** Anything a UEFI app writes only shows up on the host
  after an explicit pull (`post-run.sh` for `data/`).
- **This sandbox cannot run `qemu-system-x86_64`** (even headless, even with
  `-display none` — gets killed outright). All testing/booting happens in
  the user's own terminal on the same machine; a coding assistant working on
  this project should verify via clean compilation and, where possible,
  `debug.log`, not by attempting to launch QEMU itself.

## Phase 2 (done): seal the secret via TPM NVRAM, not a blob

Decided: skip sealed-blob-on-disk. Use a PCR[16]-gated NV Index instead — same
hardware-enforced security property, far less new code (1 new TPM2 command
instead of 5), no file for an attacker to tamper with as an attack surface to
demonstrate.

**New library work** (`edk2/SecurityPkg/Library/Tpm2CommandLib/`):
- Implement `Tpm2PolicyPCR` (declare in `Tpm2CommandLib.h`, implement
  probably in `Tpm2EnhancedAuthorization.c` alongside `Tpm2PolicyOR`/
  `Tpm2PolicySecret` — use those as the marshalling template).

**`TpmProvisionApp` additions** (first boot / provisioning):
1. Measure ROM → extend PCR[16]. *(done)*
2. Open a **trial** session (`Tpm2StartAuthSession`, `TPM_SE_TRIAL`), call
   `Tpm2PolicyPCR` against it, then `Tpm2PolicyGetDigest` to read the
   resulting digest. Flush the session.
3. `Tpm2NvDefineSpace` — create an NV Index with `authPolicy` set to that
   digest (pick a handle + size, e.g. 32 bytes for a short secret).
4. Open a **real** policy session, `Tpm2PolicyPCR` again (same boot, same
   PCR[16] value → same digest → satisfies the policy just installed).
5. `Tpm2NvWrite` the secret (`"hello"`, i.e. keep using `SECRET_STRING` —
   no need for real randomness / `Tpm2GetRandom` yet) into the index using
   that session. Flush the session.

**`TpmVerifyBootApp`** (reboot / verify — fill in the existing stub functions):
1. Measure ROM → extend PCR[16]. *(reuse `ExtendPcr16` — factor it out of
   `TpmProvisionApp.c` into something shared, or duplicate for now and
   factor later — don't over-engineer this before it's needed twice for
   real.)* — **update:** duplicated for a while as predicted, then factored
   into `Q35Pkg/Library/Tpm2PcrLib` (`BuildPcrSelection`/
   `OpenPcrPolicySession`/`ExtendPcr`, the last now generic over PCR index)
   once both apps needed it for real. See issue #8.
2. Open a **real** policy session, `Tpm2PolicyPCR`.
3. `Tpm2NvRead` using that session.
   - Succeeds → PCR[16] matches what was locked in → print secret →
     continue "boot."
   - Fails → PCR[16] doesn't match → print failure → halt (simulate refusing
     to continue boot).

**Test plan** (completed — see "Demo flow" above):
- Boot `TpmVerifyBootApp` against the *same* firmware image used for
  provisioning → expect success (PCR[16] naturally matches).
- Rebuild firmware with an intentional change (extra dummy driver, changed
  string, whatever), point `qemu.sh` at the new `OVMF_CODE.fd`, boot
  `TpmVerifyBootApp` again → expect failure/halt. This is the actual
  tamper-detection proof, and it's the only ROM comparison this project
  does — there is no golden/reference image, on real hardware or here.

**Deliverable**: a real, working, demonstrable measured-boot gate — extend
PCR wrong, TPM refuses the secret, in hardware, provably.

## Phase 3: signed OTA update flow

Out of scope until Phase 2 is solid and demonstrated. Picks up the rest of
the original "Boot With Update" design:

1. Provision a public key into a separate write-once NV Index (step 7 from
   the original plan) — needs `Tpm2NvWriteLock` (already implemented) to
   enforce write-once.
2. Update-checker downloads a signed firmware image; verify the signature
   against that NVRAM public key using `BaseCryptLib` (RSA or ECDSA verify —
   check what `CryptoPkg` actually provides before assuming a specific
   algorithm's API surface, same "don't trust a plausible name" caution as
   above).
3. Before flashing: read the current secret out of NVRAM (requires
   satisfying the *current* PCR[16] policy, since we're still running the
   old, trusted firmware at this point).
4. Set a "reseal needed" flag (a UEFI variable or a small dedicated NV
   index — decide which when we get here) so the next boot knows to redo
   step 6 below.
5. Flash the new image, reboot.
6. On the flagged reboot: measure the *new* ROM → extend PCR[16] (now a new
   value) → recompute the trial policy digest for the new PCR[16] →
   redefine the NV Index (or define a new one) with the new policy → write
   the secret back in, now gated on the new, legitimate PCR[16] → clear the
   flag.

This is meaningfully more complex than Phase 2 (signature verification,
flag/state management across a flash+reboot, secret hand-off between old and
new policy) — don't start scoping the details until Phase 2's plumbing is
proven.

## Phase 4: move off manual UEFI-shell apps

Both apps currently run as `UEFI_APPLICATION`s launched by hand from the
shell — fine for development, not how a real device boots unattended.
Eventually:

- `TpmVerifyBootApp`'s logic wants to run **early and automatically**, ideally
  gating boot before untrusted DXE drivers dispatch. Real obstacle: **the
  `EFI_TCG2_PROTOCOL` this app uses doesn't exist in PEI** — it's DXE-phase
  only. A PEI version needs to talk to `Tpm2CommandLib`/`Tpm2DeviceLib`
  directly (there's a PEI-phase device lib instance for this), which is a
  transport-layer rewrite, not a recompile. Don't attempt until the TPM
  command logic itself (Phase 2) is proven correct at the app level — much
  easier to port trustworthy logic than to debug new logic in PEI, which has
  a worse debugging experience than DXE.
- `TpmProvisionApp` runs once, ever (per device, at manufacturing/first-boot
  time) — much less pressure to leave DXE/app form. Might end up invoked by
  a controlled boot-manager step rather than becoming a PEIM itself.

## Phase 5 (stretch / hardening, not scoped yet)

- Replace the fixed `"hello"` secret with real randomness once something
  actually needs a real secret (implement `Tpm2GetRandom`, or check whether
  `RngLib` already covers this without a new TPM command at all).
- Consider anti-rollback protection (a monotonic NV counter) so an attacker
  can't just re-flash an old, previously-valid-but-since-revoked signed
  image.
- Consider locking down NV Index write access after initial provisioning
  (`Tpm2NvWriteLock`) so the "lock" itself can't be redefined by anything
  other than the intended reseal flow.

## Immediate next action

Phase 2 is done and validated (see "Demo flow" above). Next up is deciding
the re-provisioning story flagged as an open question there — start
[#5](https://github.com/ant-man21/orrery/issues/5) (OTA research) before
scoping Phase 3's implementation.
