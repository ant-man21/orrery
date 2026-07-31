# The ROM-discovery story (TpmProvisionApp / TpmVerifyBootApp)

A record of how `ReadRomImage()` got from a hardcoded address to (eventually)
something that actually works on both platforms, because the intermediate
steps were each wrong in non-obvious ways and are worth remembering.

## 1. Hardcoded address (pre-#12)

`ReadRomImage()` hardcoded `FlashBase = 0xFFC00000`, `FlashSize = SIZE_4MB`.
Worked on Q35 only because that happens to be OVMF's fixed QEMU memory map.
Real hardware, and ArmVirtQemu, have no such fixed, known-in-advance address
(issue #7).

## 2. FV2/FVB2 protocol lookup (#12, commit 25f8b4d)

Replaced the hardcode with: enumerate every handle exposing
`EFI_FIRMWARE_VOLUME2_PROTOCOL`, keep only the ones that *also* expose
`EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL` (FVB2) with a working
`GetPhysicalAddress`. The theory: a volume backed by real flash has a block
device (FVB2) behind it; a volume decompressed into RAM (e.g. PEIFV/DXEFV
unpacked from `FVMAIN_COMPACT`) doesn't.

A contiguity sanity check was added on top: if the discovered FVs' address
span doesn't exactly match the sum of their sizes, refuse to read a "merged"
range that might include a gap — Q35Pkg's FDF packs `FVMAIN_COMPACT` +
`SECFV` back-to-back with no gap, so in theory this should always hold.

This shipped, built clean on both platforms in CI, and nobody hit a runtime
problem — because nobody had booted the ARM port yet.

## 3. First ARM boot (2026-07-31)

First real ArmVirtQemu boot hit:

    [PROVISION] Firmware volumes are not contiguous - refusing to read a merged ROM range
    [PROVISION] ReadRomImage failed: Unsupported

Initial theory: ArmVirtQemu's NV variable store (`ArmVirtPkg/VarStore.fdf.inc`)
has a valid `_FVH` header (unlike Q35's, which was assumed to have none), so
`VirtNorFlashDxe` exposes FVB2 on it, and it gets swept into the "flash-backed
FV" set alongside `FVMAIN_COMPACT` — two FVs in separate, non-adjacent pflash
banks, tripping the contiguity check. Fix: exclude FVs by
`FileSystemGuid == gEfiSystemNvDataFvGuid`.

Built clean, shipped as commit 29f95e9. **Wrong diagnosis** — see next.

## 4. Same error survives the fix — diagnostic logging added

Rebuilt against the exact fixed commit, same identical error. Added a `Print`
of every flash-backed FV's address/size/`FileSystemGuid` before the
contiguity check (commit d8157f9), asked for a re-run.

Real ARM output:

    [PROVISION] flash-backed FV: addr=0x1000      size=0x2FF000  guid=8C8CE578-...
    [PROVISION] flash-backed FV: addr=0x473B6010   size=0x577380  guid=8C8CE578-...

Both GUIDs are `EFI_FIRMWARE_FILE_SYSTEM2_GUID` — the generic FFS2 type, not
NVRAM at all. The exclusion fix from step 3 never had anything to exclude in
this run. The second FV's size (0x577380 = 5,731,200 bytes) matches `FVMAIN`
exactly, per the build's own FV Space Information report — i.e. it's the
*decompressed-into-RAM* copy of everything past PEI, not flash.

## 5. Q35 checked for real, not just assumed

Ran the same diagnostic build on Q35/real hardware. Real output:

    [PROVISION] flash-backed FV: addr=0x900000 size=0xE80000 guid=8C8CE578-...
    [PROVISION] ROM snapshot: 0x900000, 15204352 bytes (1 firmware volume(s))
    ...
    [PROVISION] Tpm2NvWrite failed: Device Error   <- separate, expected issue: NV index
                                                       was already provisioned from an
                                                       earlier run against a different
                                                       PCR[16] value (see the open question
                                                       in TpmProvisionApp.c's header comment,
                                                       tracked as issue #5 — not this bug).

0xE80000 = 15,204,352 bytes matches **DXEFV** exactly, per Q35's own build FV
Space Information. `0x900000` is an ordinary low-memory RAM address, not
anywhere near Q35's real flash (~`0xFFC00000`).

So the "has FVB2 ⇒ real flash" filter is wrong on **both** platforms, just in
different ways:
- **Q35**: exactly one FV survives the filter, and it's the *wrong* one
  (DXEFV, RAM). The real flash-resident volumes (`FVMAIN_COMPACT` + `SECFV`)
  never surface as FVB2 handles in this enumeration at all. The contiguity
  check can't fail with only one candidate, so this has been silently
  measuring a RAM snapshot of the DXE-phase drivers into PCR[16] instead of
  the actual ROM since #12 shipped — nobody noticed because nothing else
  checked *what* was being measured, only that measurement succeeded.
- **ArmVirt**: two FVs survive — one real (`FVMAIN_COMPACT`) and one RAM
  (`FvMain`) — far enough apart in memory that the contiguity check correctly
  (if unhelpfully) refuses to bridge the gap.

## 6. `memmap` check

Ran the UEFI Shell's built-in `memmap` on Q35 to see what memory type the
DXEFV region (`0x900000`-`0x177FFFF`) reports as, and to check for Q35's real
flash address (`~0xFFC00000`) in the map at all.

Result: `0x900000` region reports as `BS_Data` (ordinary boot-services RAM,
confirming it's the decompressed copy). Real flash never appears in the
`memmap` output at all — 0 MMIO pages reported, no descriptor entry near
`0xFFC00000`. This is expected, not a red flag: `memmap`/`gBS->GetMemoryMap()`
only enumerates the UEFI "system memory" map; pure MMIO added via
`gDS->AddMemorySpace(EfiGcdMemoryTypeMemoryMappedIo, ...)` (which
`QemuFlashFvbServicesRuntimeDxe`/`VirtNorFlashDxe` both do, for their
respective platform's flash) lives in the separate, lower-level GCD memory
space map (`gDS->GetMemorySpaceMap()` / `GetMemorySpaceDescriptor()`), not
the boot-services one.

## 7. The actual fix

Stopped trusting FVB2 presence entirely. Instead of "has FVB2 + working
GetPhysicalAddress ⇒ real flash," the filter now checks the GCD memory type
at that address directly: `gDS->GetMemorySpaceDescriptor(FvAddress, &Descriptor)`,
keep only `Descriptor.GcdMemoryType == EfiGcdMemoryTypeMemoryMappedIo`. That's
the exact type both platforms' real flash drivers register their windows
with (`OvmfPkg/QemuFlashFvbServicesRuntimeDxe/FwBlockServiceDxe.c` for Q35,
`OvmfPkg/VirtNorFlashDxe/VirtNorFlashDxe.c` for ArmVirt — both call
`gDS->AddMemorySpace(EfiGcdMemoryTypeMemoryMappedIo, ...)`), independent of
which FVB2 producer happens to expose a given handle. The NV variable store
GUID exclusion from step 3 is kept as a secondary, independent check — still
correct, just no longer load-bearing on its own.

Also upgraded the diagnostic output to print every `EFI_FIRMWARE_VOLUME2_PROTOCOL`
handle found (not just the ones that pass every filter), including which
filter (no FVB2 / FVB2-but-not-MMIO / NVRAM) rejected each one — so the next
time something about a platform's memory layout doesn't match what the code
assumes, the shell output says so directly instead of a bare "not
contiguous" with no way to see why.

Status: builds clean on both platforms (verified in the dev sandbox — no
qemu-system-x86_64 available there to boot-test Q35, only compile-verified;
ArmVirt boot-tested by the repo owner). Awaiting a real-hardware/QEMU run on
both platforms to confirm the GCD check actually resolves it end to end
before calling this closed.
