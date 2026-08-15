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
| `trusted-firmware-a` | BL1/BL2/BL31 source, and wraps a pre-built BL32 into the FIP when given one |
| `edk2-platforms` | `Platform/Qemu/SbsaQemu/SbsaQemu.dsc` — BL33 (pinned to commit `d5a9ea8`, *one commit before* `edk2-platforms` started requiring `MdeModulePkg/Library/GptLib`, which doesn't exist in our `edk2` fork's vintage yet — a straight version-skew fix, not a feature choice) |
| `edk2-non-osi` | Just a landing spot: `Platform/Qemu/Sbsa/{bl1,fip}.bin`, staged by `build.sh` from our own TF-A build, consumed by `SbsaQemu.fdf`'s `FILE =` regions |

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
| Secure NV variable store | `0x0100_0000`, 1 MiB reserved (we use the first 256 KiB) | `QEMU_SECURE_VARSTORE_BASE/_SIZE` |

The NOR flash driver for that variable store
(`SbsaOrreryPkg/StandaloneMm/Library/NorFlashSbsaQemuLib`) is a from-scratch
`NorFlashPlatformLib` instance — same three-function interface as Juno's,
same generic `P30NorFlashDeviceLib`/`NorFlashStandaloneMm.inf` pair, just
pointed at the address above instead of Juno's SPI-NOR offsets.

## Current status: BL1→BL33 works, BL32 doesn't reach BL33 yet

Two things are true at once, and it's worth being precise about which is
which:

- **`./build.sh -M`** — BL1→BL2→BL31→BL33, no BL32 at all — is fully
  verified. It reaches the interactive TianoCore front page with
  `Boot0001: UEFI Shell` enumerated as a boot option. This is "TF-A does
  early boot" in full, and it's solid.
- **`./build.sh`** (default, BL32 included) — BL31 genuinely dispatches
  into BL32; StandaloneMmCore actually starts running at S-EL0 (you can see
  it print `Secure Partition init...` over EL3's console). But its very
  first runtime action — asking TF-A to mark part of its own image
  read-only via the `MM_SP_MEMORY_ATTRIBUTES_SET_AARCH64` SMC — trips an
  `ASSERT` inside TF-A itself, which halts EL3 outright. BL33 never gets a
  chance to run.

The rest of this doc is the story of finding that, and of the two smaller
bugs on the way to it — because the debugging process is the actual
deliverable here.

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

**Tried, and made it worse**: passing
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
doesn't budget for. Fixing it properly needs the kind of
`MAX_XLAT_TABLES`/`MAX_MMAP_REGIONS` (re)tuning Juno's own
`platform_def.h` does alongside the flag, not just the flag by itself.
Left as the concrete next step rather than chased further here.

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
break spm_mm_main.c:124        # to catch the assert from bug #2 in the act
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
separately once you're past bug #2.

## What a working MM_COMMUNICATE round trip would look like

For context on where this was headed: once BL32 actually reaches BL33, the
plan (not yet wired into `SbsaOrreryPkg.dsc`, which still uses stock
`VariableRuntimeDxe` talking directly to non-secure flash) is to swap BL33's
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
