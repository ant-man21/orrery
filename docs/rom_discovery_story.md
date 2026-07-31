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
ArmVirt boot-tested by the repo owner). **Wrong again** — see next.

## 8. GCD check ships, real flash turns out to be GCD type NonExistent

Real re-run, both platforms, after a full clean rebuild + wiped shared.img:

    ArmVirt:
      handle 0: addr=0x1000      size=0x2FF000  gcdType=0 (not MMIO, skipped)  <- FVMAIN_COMPACT, real flash
      handle 1: addr=0x473B5010  size=0x578380  gcdType=2 (not MMIO, skipped)  <- FvMain, RAM

    Q35:
      handle 0: addr=0x900000    size=0xE80000  gcdType=2 (not MMIO, skipped)  <- DXEFV, RAM

`gcdType=2` is `EfiGcdMemoryTypeSystemMemory` — correct for the RAM copies,
confirms that half of the check works. But ArmVirt's *real* flash
(`FVMAIN_COMPACT` at `0x1000`) comes back `gcdType=0`,
`EfiGcdMemoryTypeNonExistent` — never registered in GCD by anyone, not
`EfiGcdMemoryTypeMemoryMappedIo` as expected. And Q35 still only enumerates
one FV2 handle total, same as before — the real flash-resident
`FVMAIN_COMPACT`/`SECFV` still never show up as candidates at all.

Root cause, found by actually reading `VirtNorFlashDxe.c`'s init path
(`NorFlashInitialise` → `NorFlashCreateInstance` → `NorFlashFvbInitialize`):
the `gDS->AddMemorySpace(EfiGcdMemoryTypeMemoryMappedIo, ...)` call — and the
`gEfiFirmwareVolumeBlockProtocolGuid` install alongside it — only happens
`if (SupportFvb)`, and `SupportFvb` is passed in as `ContainVariableStorage`:
literally, VirtNorFlashDxe only turns *the NOR flash instance holding the
variable store* into an FVB2-producing, GCD-MMIO-registered device. The CODE
bank (holding the actual ROM, `FVMAIN_COMPACT`) is never touched by this
driver at all.

So where does `FVMAIN_COMPACT`'s FV2/FVB2 handle (`addr=0x1000`) come from,
if not `VirtNorFlashDxe`? The same generic DXE Core `FwVolBlock` HOB-wrapping
mechanism as the RAM-decompressed copies (step 7's own comment already
described this, just hadn't connected that it applies to `FVMAIN_COMPACT`
too) — PEI hands DXE Core an `EFI_HOB_TYPE_FV` HOB pointing at
`FVMAIN_COMPACT` so DXE Core can find `FvMain` inside it, and the same
generic wrapper that never calls `AddMemorySpace` handles that HOB exactly
like it handles the RAM one. **Real flash-resident content, on this
platform, never goes through a code path that registers it in the GCD at
all.** The GCD memory type is not a usable signal here, full stop — not "use
a different type than MMIO," genuinely not present in GCD either way.

This means the entire family of "ask the firmware to tell you where its own
flash is via some protocol/memory-map introspection" is a dead end on both
platforms as currently built. Standard EDK2 practice for "what's my own
flash base/size" is not runtime introspection at all — it's a **build-time
PCD**, set by the platform's own DSC/FDF (which is exactly what both
`ArmVirtQemu.fdf`'s `PcdFdBaseAddress`/`PcdFdSize` and OVMF's
`PcdOvmfFdBaseAddress`/`PcdBfvBase`-family PCDs already are — the very
values these platforms' own FDFs use to lay out the FD in the first place).
Reaching for FV2/FVB2 protocol introspection (#12's approach) instead of a
PCD was the wrong tool from the start; it happened to look reasonable
because it avoided one hardcoded C literal, but a per-platform PCD isn't
"hardcoding" in the sense issue #7 was worried about (a single constant
wrong for every platform) — it's the standard mechanism for a platform to
tell its own shared code a platform-specific constant, and every real EDK2
platform port does exactly this.

## 9. PlatformRomInfoLib — a library class, not a hardcoded PCD read

Landed as a `PlatformRomInfoLib` library class (`GetPlatformRomInfo(OUT
EFI_PHYSICAL_ADDRESS *RomBase, OUT UINT64 *RomSize)`), declared once in
`OrreryPkg.dec`, with one instance per platform:

- `PlatformRomInfoLibQ35` — reads `gUefiOvmfPkgTokenSpaceGuid.PcdBfvBase` /
  `PcdBfvRawDataSize`, OVMF's own "Boot Firmware Volume" PCDs (the same pair
  OVMF's TDX/SEV measurement code, `PeilessStartupLib`, reads for the exact
  same reason — finding its own flash-resident firmware).
- `PlatformRomInfoLibArmVirt` — reads `gArmTokenSpaceGuid.PcdFvBaseAddress` /
  `PcdFvSize`, the standard ArmPkg pair `ArmVirtQemu.fdf` itself uses to
  patch `FVMAIN_COMPACT`'s FD region, and that `ArmPlatformPkg/Sec/Sec.c`
  reads for the same reason.

Both PCD pairs are populated by `GenFds` directly from each platform's own
FDF at image-build time — no new DSC-level PCD declarations were needed,
they already existed and already had exactly this meaning; the earlier
"has FVB2"/"is GCD MMIO" runtime checks were solving an already-solved
problem the wrong way.

Each platform's DSC selects its own instance via
`[LibraryClasses.common.UEFI_APPLICATION]` — `Q35Pkg.dsc` maps
`PlatformRomInfoLib` to `PlatformRomInfoLibQ35`, `ArmVirtOrreryPkg.dsc` to
`PlatformRomInfoLibArmVirt`. `TpmProvisionApp.c`/`TpmVerifyBootApp.c` call
`GetPlatformRomInfo()` and never reference a platform-specific PCD, GUID, or
protocol directly — `ReadRomImage()` dropped from ~90 lines of
protocol-enumeration-plus-heuristics to a single library call plus an
`AllocateCopyPool`. Porting to real hardware (Tegra, per the project's
stated direction) means writing one more `PlatformRomInfoLib` instance and
pointing that platform's DSC at it — the app itself doesn't change.

Status: both platforms build clean (compile-verified only in the dev
sandbox — no `qemu-system-x86_64` there to boot-test Q35). Awaiting a real
QEMU run on both platforms — this is the third attempt at this bug, so
"builds clean" is being stated plainly, not as a substitute for the actual
verification still owed.
