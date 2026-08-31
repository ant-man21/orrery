# The SBSA boot flow: TF-A early boot + StandaloneMm in BL32

SbsaOrreryPkg targets QEMU's `sbsa-ref` machine — the closest thing QEMU has
to a real Arm server: SBSA/SBBR-shaped memory map, PSCI-based CPU power
management, a proper GICv3, and (unlike `virt`) no `-bios` shortcut. Nothing
runs before the EL3 firmware does. This doc is two things: a map of the
BL1→BL33 handoff so you can find your way around it in a debugger, and a
record of what broke while bringing it up and why, because the intermediate
failures are more instructive than the final state (see
`docs/rom_discovery_story.md` for the same idea applied to a different bug).

## Why this platform needs TF-A at all

ArmVirtOrreryPkg (`virt`) and Q35Pkg (`q35`) both let QEMU load UEFI directly
via `-pflash`/`-bios` — there's no separate secure-world firmware layer.
`sbsa-ref` doesn't offer that shortcut: it always resets into secure state at
EL3, address 0x0, and something has to run there before anything else can.
That something is Trusted Firmware-A (TF-A), and what it does before handing
off to UEFI is the actual subject of this doc.

## The four-stage handoff

```
 BL1 (ROM, EL3)        BL2 (EL3→EL3)     BL31 (EL3)         BL32 (S-EL0)        BL33 (NS-EL2, "OS")
 secure boot ROM  -->  loads BL2/31/  -->  EL3 runtime  --> StandaloneMm  -->  edk2 UEFI (SbsaOrreryPkg)
 (trusted-firmware-a)  32/33 images       firmware,          Secure           our SbsaQemu.dsc +
                       from FIP           PSCI, SPM_MM       Partition        OrreryPkg TPM drivers
```

- **BL1** — the secure boot ROM. QEMU maps it at address 0 via
  `SBSA_FLASH0.fd`'s first region (`0x0000_0000`–`0x0001_2000`). Its only job
  is to authenticate and load BL2.
- **BL2** — the trusted boot firmware. Loads BL31, BL32 (if present), and
  BL33 into RAM from the FIP (a TLV container also embedded in
  `SBSA_FLASH0.fd`, right after BL1), does the platform's early DRAM/GIC
  setup, then hands off to BL31.
- **BL31** — the EL3 runtime firmware that stays resident for the life of
  the system (PSCI power management, SMC dispatch, the thing every future
  `smc`/`hvc` from the OS eventually lands in). Before handing off to BL33,
  it does one more thing if BL32 is present: it *synchronously enters* BL32
  once, letting it run its own init to completion, before returning to the
  normal boot sequence.
- **BL32** — StandaloneMm, a Secure Partition running at S-EL0. This is
  where "TPM in the secure world" / "variables backed by a secure NOR flash
  region no one in the non-secure OS can reach directly" kind of features
  live on a real server. TF-A dispatches into it via **SPM_MM** — the
  older, simpler "MM-based" Secure Partition Manager (as opposed to the
  newer FF-A/SPMC model most current Arm reference platforms use). Requests
  from BL33 later reach it via the `MM_COMMUNICATE` SMC.
- **BL33** — "the OS," which for us is edk2 UEFI: `edk2-platforms`'
  `Platform/Qemu/SbsaQemu/SbsaQemu.dsc`, with `SbsaOrreryPkg.dsc` layering
  OrreryPkg's TPM demo apps and a couple of local fixes on top (below).
  Loaded straight into RAM by BL2 (not executed in place from flash), then
  entered by BL31 the same way BL1 entered BL2.

## Flash layout

Unlike ArmVirt/Q35's single CODE+VARS pflash pair, `sbsa-ref` splits things
by *who owns the flash*, not by *what's in it*:

- **`SBSA_FLASH0.fd`** — secure-only. BL1 at offset 0, FIP (BL2+BL31+BL32)
  starting at `0x12000`. TF-A never lets non-secure code near this bank.
- **`SBSA_FLASH1.fd`** — non-secure. BL33's own FVMAIN_COMPACT at offset 0,
  then the standard `_FVH`-style NV variable store BL33's own
  `VariableRuntimeDxe` uses for its (non-secure) UEFI variables.

Both get built by the *edk2* build (`SbsaOrreryPkg.dsc`'s underlying
`SbsaQemu.fdf`), which pulls `bl1.bin`/`fip.bin` in from
`edk2-non-osi/Platform/Qemu/Sbsa/` as raw `FILE =` regions — TF-A doesn't
know or care that edk2 packages its output; from TF-A's side, building
`bl1.bin`+`fip.bin` is the whole deliverable. `build.sh` stages them there
as step 3 of 4, between building TF-A and building BL33.

QEMU pads both flash devices to 256 MiB pflash banks at runtime regardless
of the "used" size edk2 emits — same story as
`ArmVirtOrreryPkg/qemu.sh`'s CODE/VARS padding, see that file's comments.

## Submodules added for this platform

Three, alongside the existing `edk2` one:

| Submodule | What it provides |
|---|---|
| `trusted-firmware-a` | BL1/BL2/BL31 source, and wraps a pre-built BL32 into the FIP when given one. `build.sh` applies `SbsaOrreryPkg/patches/0001-qemu_sbsa-fix-spm-mm-xlat-tables.patch` on top before building (bug #2, below) — the submodule itself stays unmodified/pinned |
| `edk2-platforms` | `Platform/Qemu/SbsaQemu/SbsaQemu.dsc` — BL33 (pinned to commit `d5a9ea8`, *one commit before* `edk2-platforms` started requiring `MdeModulePkg/Library/GptLib`, which doesn't exist in our `edk2` fork's vintage yet — a straight version-skew fix, not a feature choice) |
| `edk2-non-osi` | Just a landing spot: `Platform/Qemu/Sbsa/{bl1,fip}.bin`, staged by `build.sh` from our own TF-A build, consumed by `SbsaQemu.fdf`'s `FILE =` regions |

`vendor/libtl/` is **not** a submodule — it's our own hand-written Transfer
List library (bug #3, below), checked straight into this repo since
upstream's reference implementation lives on a Gerrit host this
environment can't reach.

`SbsaOrreryPkg/StandaloneMm/` (BL32's DSC/FDF) is **ours**, not upstream —
`edk2-platforms`' own `Platform/Qemu/SbsaQemu/Readme.md` only documents
BL1/BL2/BL31+BL33; it has no StandaloneMm story for this platform at all.
It was written from scratch using
`edk2-platforms/Platform/ARM/JunoPkg/PlatformStandaloneMm.dsc` (a *real*,
working SPM_MM platform) as a template, with every address swapped for
values pulled straight out of
`trusted-firmware-a/plat/qemu/qemu_sbsa/include/platform_def.h`:

| What | Value | Source |
|---|---|---|
| BL32 load address / size | `0x2000_8000` / `0x30_0000` | `PLAT_QEMU_SP_IMAGE_BASE` / `_SIZE` |
| Secure UART (StMm's own console, separate from BL31's/BL33's) | `0x6004_0000` | `UART2_BASE`, commented "Secure UART" |
| Secure NV variable store | `0x0100_0000`, 1 MiB reserved (we use 768 KiB: 3 x 256 KiB regions — see bug #4) | `QEMU_SECURE_VARSTORE_BASE/_SIZE` |

The NOR flash driver for that variable store
(`SbsaOrreryPkg/StandaloneMm/Library/NorFlashSbsaQemuLib`) is a from-scratch
`NorFlashPlatformLib` instance — same three-function interface as Juno's,
same generic `P30NorFlashDeviceLib`/`NorFlashStandaloneMm.inf` pair, just
pointed at the address above instead of Juno's SPI-NOR offsets.

## Current status: full BL1→BL32→BL33 boot works

Both configurations are fully verified:

- **`./build.sh -M`** — BL1→BL2→BL31→BL33, no BL32 at all. Reaches the
  interactive TianoCore front page with `Boot0001: UEFI Shell` enumerated
  as a boot option. The simple path, useful when you just want a boot and
  don't care about StandaloneMm.
- **`./build.sh`** (default, BL32 included) — BL1→BL2→BL31→BL32→BL33, the
  whole chain. BL31 dispatches into BL32; StandaloneMmCore runs its full
  init (loads all four MM drivers — `StandaloneMmCpu`, `NorFlashStandaloneMm`,
  `FaultTolerantWriteStandaloneMm`, `VariableStandaloneMm` — with a working
  secure NV variable store), returns control to BL31, and BL31 hands off to
  BL33, which reaches the same TianoCore front page.

Getting from "BL32 asserts on its first SMC" to this took four distinct
bugs, in the order they were found. Each is documented below with what it
looked like, why it happened, and the fix — because the debugging process
is the actual deliverable here, not just the diff.

## Bug #1: a QEMU-version gap that looks exactly like a hang

First boot attempt (BL1→BL31→BL33, no BL32 yet) printed TF-A's banners fine,
printed BL33's version banner, and then... nothing. No crash, no shell, no
further output — for as long as you're willing to wait. That's the "looks
like a hang" trap: **it wasn't hung, it had shut the machine off.**

`Silicon/Qemu/SbsaQemu/Library/SbsaQemuHardwareInfoLib/
SbsaQemuHardwareInfoLib.c`'s `GetCpuTopology()` asks TF-A for CPU topology
via a SIP SMC (`SIP_SVC_GET_CPU_TOPOLOGY`). TF-A's handler
(`plat/qemu/qemu_sbsa/sbsa_platform.c`,
`read_cpu_topology_from_dt()`) answers by reading a `/cpus/topology` device
tree node QEMU is supposed to generate. On the QEMU 8.2.2 available in this
environment, `sbsa-ref` never emits that node — confirmed directly:

```sh
qemu-system-aarch64 -M sbsa-ref -smp 4,sockets=1,clusters=1,cores=4,threads=1 \
  -machine dumpdtb=/tmp/sbsa.dtb -display none -serial none \
  -global e1000e.romfile= -global bochs-display.romfile=
dtc -I dtb -O dts /tmp/sbsa.dtb | grep -A6 'cpus {'
#   cpus {
#     #size-cells = <0x00>;
#     #address-cells = <0x02>;
#     cpu@0 { reg = <0x00 0x00>; };   <-- no "topology" subnode, with or without -smp decomposition
```

So the SMC legitimately answers "unknown" (`cores == 0`), and
`GetCpuTopology()`'s response to *any* SIP failure is `ResetShutdown()` —
which, on QEMU, actually powers the VM off. No crash dump, no panic
message, just silence and then process exit. If you're tailing the log
without watching the process itself, this is indistinguishable from a hang.

**Fix**: `OrreryPkg/Library/SbsaQemuHardwareInfoLibCompat` — a fork of the
upstream library (same file, byte-for-byte, everywhere except this one
function) that falls back to a flat `1 socket / 1 cluster / N cores /
1 thread` topology instead of shutting down. Wired in via
`SbsaOrreryPkg.dsc`'s `[LibraryClasses.common]` — the same "override via our
own DSC, don't patch the vendored submodule" pattern `PlatformRomInfoLib`
already uses for the other two platforms (see `OrreryPkg.dec`'s header).
Delete it once running against a QEMU that populates `/cpus/topology`.

**How to have caught this faster**: watch `ps` / the QEMU process itself,
not just the log tail. A silently-exited process and a genuinely hung one
look identical in a scrolling log; they don't look identical in `ps aux`.

## Bug #2: `-M SP_MEMORY_ATTRIBUTES_SET` and block-coalesced xlat tables

With bug #1 fixed and BL32 wired in, boot gets much further: BL31 sets up
the Secure Partition context, dispatches into it, and StandaloneMmCore
starts running:

```
INFO:    BL31: Initializing BL32
INFO:    Secure Partition init...
INFO:    Received MM_SP_MEMORY_ATTRIBUTES_SET_AARCH64 SMC
INFO:      Start address  : 0x2001b000
INFO:      Number of pages: 2 (8192 bytes)
INFO:      Attributes     : 0x5
ASSERT: services/std_svc/spm/spm_mm/spm_mm_main.c:124
```

`0x2001b000` is inside BL32's own image (`0x2000_8000`–`0x2010_8000`) —
this is `ArmPkg/Library/StandaloneMmMmuLib`'s W^X hardening,
(`ArmSetMemoryRegionReadOnlyPerm`/`ArmClearMemoryRegionNoExec`/etc. in
`ArmMmuStandaloneMmLib.c`) marking one of its own PE/COFF sections
read-only right after loading it — completely standard StandaloneMmCore
behavior, not something platform-specific.

TF-A's handler
(`services/std_svc/spm/spm_mm/spm_mm_xlat.c`,
`spm_memory_attributes_set_smc_handler` →
`lib/xlat_tables_v2/xlat_tables_utils.c`,
`xlat_change_mem_attributes_ctx`) has a hard requirement: every page in the
requested range must already be mapped as an individual `PAGE_DESC` at the
finest translation table level. If it finds a coarser block descriptor
instead, it refuses:

```c
if (((desc & DESC_MASK) != PAGE_DESC) || (level != XLAT_TABLE_LEVEL_MAX)) {
    WARN("Address 0x%lx is not mapped at the right granularity.\n", base_va);
    return -EINVAL;
}
```

BL31's static memory map for the SP image
(`plat/qemu/common/qemu_spm.c`, `QEMU_SP_IMAGE_MMAP`) does ask for
`PAGE_SIZE` granularity — but `xlat_tables_v2` coalesces same-attribute
adjacent pages into the largest block descriptor it can at table-build
time regardless, *unless* the platform is built with
`PLAT_XLAT_TABLES_DYNAMIC` (which keeps regions split and re-splittable at
runtime). That `-EINVAL` propagates back up through
`RequestMemoryPermissionChange`/`SendMemoryPermissionRequest` in edk2 as a
plain error return; StandaloneMmCore's own init treats a failed
memory-protect call as fatal and reports back to TF-A via
`MM_SP_EVENT_COMPLETE_AARCH64` with a non-zero status — which is what makes
`assert(rc == 0)` in `spm_mm_main.c:124` fire.

**The tell that this is a real upstream gap, not something we did wrong**:
`plat/arm/board/juno/platform.mk` — a platform that genuinely ships and is
tested against exactly this SPM_MM+StandaloneMm combination — sets
`BL31_CPPFLAGS += -DPLAT_XLAT_TABLES_DYNAMIC` and
`BL32_CPPFLAGS += -DPLAT_XLAT_TABLES_DYNAMIC` specifically when
`SPM_MM=1`. `plat/qemu/qemu_sbsa/platform.mk`'s own `SPM_MM` block does
not set this at all. QEMU's SBSA platform in TF-A has SPM_MM plumbing, but
it was never finished/tested against a StandaloneMmCore new enough to
require runtime page-attribute changes.

**First attempt, and why it made things worse**: passing
`BL31_CPPFLAGS=-DPLAT_XLAT_TABLES_DYNAMIC BL32_CPPFLAGS=-DPLAT_XLAT_TABLES_DYNAMIC`
straight to `make` (after confirming a *clean* `build/qemu_sbsa` — TF-A's
build doesn't track flag changes in its dependency files, so a stale build
tree silently links mismatched objects; always `rm -rf` between config
changes, which `build.sh` now does automatically) doesn't just fail to fix
the assert — it regresses to BL31 crashing *before printing its own version
banner*, i.e. before it even finishes bringing up its own initial MMU
state. `PLAT_XLAT_TABLES_DYNAMIC` changes assumptions throughout
`xlat_tables_v2` (reserved table counts, region bookkeeping) that
`qemu_sbsa`'s `platform_def.h` — never written with this flag in mind —
doesn't budget for: with dynamic tables, `xlat_table_get_empty()` returns
`NULL` on exhaustion instead of asserting, and the caller doesn't check —
a silent NULL-pointer data abort during BL31's own pre-console-init MMU
bring-up, which looks exactly like the process being reset (zero output).

**The actual fix**: bump `MAX_MMAP_REGIONS`/`MAX_XLAT_TABLES` from 13 to 32
alongside the flag — but *only* for `IMAGE_BL31`, not unconditionally.
`platform_def.h` is shared across BL1/BL2/BL31 whenever `SPM_MM=1` (it's a
build-wide flag, not image-specific), and BL1's RW budget is a mere 72 KiB
(`BL1_SIZE`) — bumping the table count unscoped blows BL1's link
(`region 'RAM' overflowed by 65536 bytes`). Scoping the bump to
`#if SPM_MM && defined(IMAGE_BL31)` (mirroring the pattern TF-A's own
`PLAT_SP_IMAGE_MMAP_REGIONS`/`PLAT_SP_IMAGE_MAX_XLAT_TABLES` already use
right below it) fixes both problems: BL31 gets the headroom dynamic tables
need, BL1 keeps its tight budget. Combined with the `PLAT_XLAT_TABLES_DYNAMIC`
flag, `MM_SP_MEMORY_ATTRIBUTES_SET_AARCH64` now succeeds every time
StandaloneMmCore calls it — confirmed by tracing
`xlat_change_mem_attributes_ctx()`'s return value through every call during
a full boot (all zero/success).

This fix lives in `trusted-firmware-a` — a pinned upstream submodule we
don't own — as
`SbsaOrreryPkg/patches/0001-qemu_sbsa-fix-spm-mm-xlat-tables.patch`,
applied by `build.sh` before each build (idempotently: it checks whether
the patch is already applied via `git apply --reverse --check` first).

## Bug #3: BL32 loads, but edk2 refuses the handoff outright

With bug #2 fixed, the assert is gone — but StandaloneMmCore now fails
even earlier, before printing anything of its own:

```
INFO:    BL31: Initializing BL32
INFO:    Secure Partition init...
<BL32 never prints "Secure Partition init..." from *its own* side, or anything else>
```

Attaching gdb at BL32's entry point (`PLAT_QEMU_SP_IMAGE_BASE`, see the
debugging section below) shows execution reaching
`ArmPkg/Library/ArmStandaloneMmCoreEntryPoint/ArmStandaloneMmCoreEntryPoint.c`'s
`ValidateSpmMmBootInfo()`, which hard-fails with `EFI_INVALID_PARAMETER`
and returns before StandaloneMmCore's own init ever runs. That function's
whole job is to find one specific thing in the registers TF-A handed it at
entry (`X0`-`X3` per the SPM_MM boot protocol): either a legacy HOB-list
pointer, or a **Transfer List** — a standardized "Firmware Handoff"
container (header + tagged entries) newer TF-A/edk2 versions use instead.
Our edk2 vintage's `ArmStandaloneMmCoreEntryPoint.c` only implements the
Transfer List path — the legacy HOB-pointer fallback was removed upstream
(commit `a5212d3db7`, `HOB_LIST -> TRANSFER_LIST` migration; 18 further
commits have since touched the same file — reverting a5212d3db7 would
mean giving all of those up too, so that was ruled out early). TF-A's
`qemu_sbsa` port, as built by default, only knows how to hand off the
*legacy* way. Neither side is wrong; they're both correct implementations
of two different, non-overlapping generations of the same handoff
protocol.

**Confirming this is a real, supported combination and not a guess**:
`edk2-platforms/Platform/ARM/Readme.md` documents `HOB_LIST=1
TRANSFER_LIST=1` as the officially-maintained TF-A build flags for exactly
this edk2-vintage + SPM_MM combination, on Juno and FVP.

**The complication**: TF-A's own reference implementation of the Transfer
List library (`contrib/libtl`, referenced by `TRANSFER_LIST=1` builds via
`LIBTL_PATH`) isn't vendored into the `trusted-firmware-a` tree — it's
pulled from `review.trustedfirmware.org/shared/transfer-list-library`, a
Gerrit host outright blocked by this environment's network egress policy,
with no GitHub mirror found under any name for that specific
sub-repository (unlike `trusted-firmware-a` itself, which *is* mirrored on
GitHub).

**Fix**: `vendor/libtl/` — a from-scratch, minimal implementation of just
the Transfer List operations `qemu_bl2_setup.c`/`qemu_bl31_setup.c`/
`spm_mm_setup.c` actually call (`transfer_list_init`, `_add`,
`_check_header`, `_entry_data`, `_relocate`, `_dump`, plus the AArch64
handoff-argument helper), built against the real, public [Firmware
Handoff spec](https://github.com/FirmwareHandoff/firmware_handoff) — but
with every struct layout and tag-ID value copied field-for-field from
`edk2/ArmPkg/Include/IndustryStandard/ArmTransferList.h`, i.e. from the
actual consumer we need to interoperate with, not just the spec text. See
`vendor/libtl/include/transfer_list.h`'s header comment for the full
provenance story. `build.sh` passes `HOB_LIST=1 TRANSFER_LIST=1
LIBTL_PATH=vendor/libtl` when building TF-A with BL32 included.

## Bug #4: a NOR-flash erase-granularity mismatch wipes the variable store

With bug #3 fixed, StandaloneMmCore finally runs its real init: all four MM
drivers load, `NorFlashStandaloneMm` and `FaultTolerantWriteStandaloneMm`
both self-format their regions cleanly — and then `VariableStandaloneMm`
asserts:

```
Firmware Volume for Variable Store is corrupted
ASSERT_EFI_ERROR (Status = Volume Corrupt)
ASSERT [VariableStandaloneMm] VariableSmm.c(1164)
```

despite `NorFlashStandaloneMm` having *just* validated a header at that
exact same address moments earlier. The first suspect was our own
`tools/make_empty_varstore.py` (written to seed a fresh
`EFI_FIRMWARE_VOLUME_HEADER`+`VARIABLE_STORE_HEADER`+FTW-working-block into
the secure NV region, since nothing else formats it on first boot) — and it
did have a real, confirmed bug: `NorFlashFvb.c`'s `ValidateFvHeader()`
checks `VariableStoreHeader->Size == PcdFlashNvStorageVariableSize -
FwVolHeader->HeaderLength`, and the script was subtracting the
`VARIABLE_STORE_HEADER`'s own size a second time, undercounting `Size` by
28 bytes. Fixing that made `NorFlashStandaloneMm`'s own validation stop
complaining — but `VariableStandaloneMm` kept failing identically.

**Isolating the real cause** took a byte-level marker test: fill the whole
1 MiB secure NV reserve with a `0xAA` marker, format only the header
regions on top, boot, then diff the *entire* reserve against the marker
afterward. Result: the Variable region and FtwWorking region came back
erased (`0xFF`) — even though nothing should touch them between
`NorFlashStandaloneMm`'s validation and `VariableStandaloneMm`'s read —
while `FaultTolerantWriteStandaloneMm` ran in between and logged "Both
working and spare blocks are invalid, init workspace" / "reclaim work
space successfully" every single boot, regardless of what was actually
written there beforehand.

The root cause: our original NV layout gave each of the three regions
(Variable / FtwWorking / FtwSpare) only **64 KiB**, matching the
`NOR_FLASH_DESCRIPTION` block size we'd chosen — but QEMU's `sbsa-ref`
secure pflash device's *actual* erase-sector granularity is **256 KiB**.
`FtwReclaimWorkSpace()`'s block-erase calls, sized to what the driver
*thinks* is one logical block, end up erasing the real (larger) physical
sector underneath — which spans across our 64 KiB region boundaries and
wipes the neighboring Variable store's header as a side effect. This is
exactly the failure mode a 64 KiB layout invites and a 256-KiB-aligned one
doesn't: it's also why the reference platforms (Juno, VExpress, and
`SbsaQemu.fdf`'s own *non-secure* store) all use 256 KiB per region rather
than something smaller.

**Fix**: resize all three regions to 256 KiB each (768 KiB total, still
comfortably inside the 1 MiB TF-A/QEMU reserve at `QEMU_SECURE_VARSTORE_BASE`)
in `SbsaOrreryStandaloneMm.dsc`'s NV storage PCDs and
`NorFlashSbsaQemuLib.c`'s device descriptor. With matching erase
granularity, `FaultTolerantWriteStandaloneMm`'s reclaim path stays inside
its own region, and `VariableStandaloneMm` initializes cleanly — full
`BL1→BL32→BL33` boot reaches the UEFI Boot Manager front page with no
asserts anywhere in the chain.

`tools/make_empty_varstore.py` is idempotent (it checks for an
already-valid header before formatting, `--force` to override) so
`build.sh` can call it unconditionally on every build without clobbering
variables a previous boot persisted — the same "only touch it if it's
missing/invalid" pattern `qemu.sh`/`build.sh` already use for the VARS
files on the other two platforms, applied at byte-region granularity
inside `SBSA_FLASH0.fd` (which also holds BL1+FIP, rebuilt fresh every
time, unlike `SBSA_FLASH1.fd` which is skipped once it exists).

## Debugging this yourself

`qemu.sh -g` starts QEMU paused with a gdbstub on `:1234` (`-S -s`), CPU0
halted at the reset vector (BL1, address 0). Attach with:

```sh
gdb-multiarch -ex 'target remote :1234'
```

The awkward part of debugging a multi-stage boot like this: **every stage
is a different ELF, loaded at a different time, at a different address**,
and gdb has no idea which one you're stopped in. You have to tell it, and
re-tell it every time execution moves to the next stage:

```gdb
# BL1 — runs from reset, no separate .elf needed (it's tiny, disassemble
# straight from memory), or:
add-symbol-file trusted-firmware-a/build/qemu_sbsa/debug/bl1/bl1.elf 0x0
break *0x0
continue

# BL2 — loaded by BL1 at a dynamic address; check the "Loading image id=1
# at address 0x..." line in the serial log for where, then:
add-symbol-file trusted-firmware-a/build/qemu_sbsa/debug/bl2/bl2.elf 0x3fbd1000
break bl2_entrypoint
continue

# BL31 — same idea, address from "Loading image id=3 at address 0x...":
add-symbol-file trusted-firmware-a/build/qemu_sbsa/debug/bl31/bl31.elf 0x3fbee000
break bl31_main
continue

# BL32/StandaloneMm — loaded at the fixed PLAT_QEMU_SP_IMAGE_BASE
# (0x20008000), matches "Loading image id=4 at address 0x...":
add-symbol-file Build/SbsaOrreryStandaloneMm/DEBUG_GCC/AARCH64/StandaloneMmPkg/Core/StandaloneMmCore/DEBUG/StandaloneMmCore.dll 0x20008000
break ArmStandaloneMmCoreEntryPoint  # or ValidateSpmMmBootInfo, spm_mm_main.c:124, etc.
continue

# BL33 — GenFds-decompressed load address varies per build; the DEBUG
# build's own serial log prints "add-symbol-file <path> 0x<addr>" for
# every driver as it loads (see the boot log excerpts above) — paste
# those lines straight into gdb as they scroll past.
```

`ArmPkg/Drivers/CpuPei/CpuPei.inf` and friends print those `add-symbol-file`
lines automatically in DEBUG builds specifically so you can do this — it's
not a trick, it's the intended workflow for source-level UEFI debugging on
ARM.

Serial consoles, by design: BL1/BL2/BL31 share one non-secure console
(`UART0`, `-serial stdio` — the only one `qemu.sh` wires up today). BL31's
own crash console is `UART1`; StMm's console is `UART2`, per
`SbsaOrreryStandaloneMm.dsc`'s `PcdSerialRegisterBase`. Add
`-serial file:secure.log` (or a second `-serial mon:stdio`) to `qemu.sh`'s
extra-args (`-- -serial file:secure.log`) to see StMm's own prints
separately.

## What a working MM_COMMUNICATE round trip would look like

BL32 now boots cleanly end to end (bugs #1-#4 above), but BL33 and BL32
still don't actually talk to each other yet — `SbsaOrreryPkg.dsc` still
uses stock `VariableRuntimeDxe` talking directly to non-secure flash, same
as before StandaloneMm was wired in at all. The plan is to swap BL33's
variable stack for the MM-communicating one — `VariableSmmRuntimeDxe` +
`MmCommunicationDxe` (ArmPkg's SMC-based one) in place of
`VariableRuntimeDxe`, matching what's already built for BL32:
`VariableStandaloneMm.inf` + `FaultTolerantWriteStandaloneMm.inf` +
`NorFlashStandaloneMm.inf` (all three already in
`SbsaOrreryStandaloneMm.fdf`). A `GetVariable()` call from BL33 would then
issue an `MM_COMMUNICATE_AARCH64` SMC, TF-A's `spm_mm_main.c` would route it
into BL32 via `mm_communicate()`, StandaloneMm's own `VariableStandaloneMm`
driver would read/write the secure NOR region at `0x0100_0000`, and the
result comes back over the same SMC. That's the real point of "StMm in
BL32" on server BIOS: UEFI variables (including Secure Boot's PK/KEK/db)
live behind a boundary the non-secure OS can never reach directly, no
matter how compromised BL33/the OS gets.
