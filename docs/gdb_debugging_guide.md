# Attaching a debugger to QEMU: a JTAG-equivalent workflow

This is a how-to, not a war story — if you want the bugs this workflow was
actually used to find, see `docs/sbsa_boot_flow.md`. This doc is for one
goal: attach a debugger to a booting VM and step through it, the same way
you'd attach a JTAG probe (a J-Link, a Lauterbach TRACE32, whatever your
job hands you) to a real board.

That's not a loose analogy. `gdb` talks the same wire protocol — the GDB
Remote Serial Protocol — to QEMU's built-in gdbstub that it talks to a real
hardware probe via OpenOCD or a Lauterbach gdb bridge. The commands you
build muscle memory for here (`break`, `continue`, `step`, `info
registers`, reading a backtrace) are the actual skill, not a simulation of
it. What's genuinely different on real silicon: you can't just read a
build tree sitting next to you for symbols and load addresses, and you'll
occasionally lose the debug connection across a secure/non-secure world
switch or a warm reset in a way QEMU's gdbstub conveniently never does.

## Quick start, all three platforms

Every platform's `qemu.sh` takes `-g`: start QEMU **paused at reset**, gdbstub
listening on TCP port 1234. `-S` is the QEMU flag that freezes the guest
CPU at startup instead of letting it run (same idea as JTAG's power-on
halt); `-s` is shorthand for `-gdb tcp::1234`. Nothing executes until a
debugger attaches and tells it to.

| Platform | Launch | Attach | Why a different gdb |
|---|---|---|---|
| `Q35Pkg` | `./qemu.sh -d -g` | `gdb -ex 'target remote :1234'` | x86_64 target — plain `gdb` on an x86 host handles it |
| `ArmVirtOrreryPkg` | `./qemu.sh -d -g` | `gdb-multiarch -ex 'target remote :1234'` | AArch64 target — plain `gdb` on an x86 host can't decode AArch64 instructions or registers |
| `SbsaOrreryPkg` | `./qemu.sh -d -g` | `gdb-multiarch -ex 'target remote :1234'` | same — AArch64 |

Use a `-d` (DEBUG) build for this. RELEASE builds strip the DEBUG-level
serial prints that hand you load addresses as they happen — without them
you're back to guessing.

For Q35/ArmVirt, that's the whole story: one firmware image, one ELF's
worth of symbols, no further complications. **SBSA is the interesting
case**, because it isn't one firmware image at all.

## SBSA: five different programs, one gdb session

Unlike Q35/ArmVirt (QEMU loads UEFI directly), `sbsa-ref` boots through
TF-A first: BL1 → BL2 → BL31 → BL32 (StandaloneMm) → BL33 (UEFI). See
`docs/sbsa_boot_flow.md` for what each stage does. What matters here: **each
stage is a separate ELF/PE image, loaded at a different time, at a
different address**, and gdb has no way to know which one it's looking at
unless you tell it — and tell it again every time execution moves to the
next stage. This is the one real skill this platform teaches that Q35/
ArmVirt don't: symbol management across a multi-stage handoff, which is
exactly what you'll be doing on a real server SoC.

Addresses below were captured against this repo's current, fully-working
boot chain (`SbsaOrreryPkg/build.sh -d -S -F && qemu.sh -d -g`) — re-verify
them yourself if `SbsaQemu.dsc`/`platform_def.h`/the TF-A build config
changes; BL2 and BL31's load addresses in particular are computed by BL1/
BL2 at build+link time, not hardcoded platform constants like BL32's is.

```gdb
target remote :1234

# --- BL1: runs from the reset vector, address 0. We're already halted
# here (that's what -S did). It's small; disassemble straight from memory
# if you need to, or:
add-symbol-file trusted-firmware-a/build/qemu_sbsa/debug/bl1/bl1.elf 0x0

# --- BL2: BL1 loads it at a fixed-for-this-build address — confirm from
# the serial log's "Loading image id=1 at address 0x..." line.
break *0x3fbc9000
continue
add-symbol-file trusted-firmware-a/build/qemu_sbsa/debug/bl2/bl2.elf 0x3fbc9000
# now `break bl2_entrypoint`, `break bl2_main`, etc. resolve by name

# --- BL31: same idea, from "Loading image id=3 at address 0x...":
break *0x3fbe6000
continue
add-symbol-file trusted-firmware-a/build/qemu_sbsa/debug/bl31/bl31.elf 0x3fbe6000
break bl31_main
continue
# confirmed live: lands at bl31/bl31_main.c:108, backtrace shows
# bl31_entrypoint() one frame up, exactly as the source implies

# --- BL32 (StandaloneMm): loaded at the *fixed* PLAT_QEMU_SP_IMAGE_BASE
# (0x20008000 — a real platform constant, not build-dependent), matches
# "Loading image id=4 at address 0x...". A raw-address breakpoint here is
# reliable and confirmed live every time:
break *0x20008000
continue
# `x/5i $pc` here shows a single `b <addr>` instruction followed by `udf`
# padding — the module's own entry trampoline, not a named C function.
# Symbol-name breakpoints (`break CEntryPoint`, etc.) do NOT resolve
# correctly yet against a naive `add-symbol-file ... 0x20008000` the way
# they do for BL2/BL31 above — unlike those two (plain ELF, loaded exactly
# as linked), BL2 extracts this SP image from a wrapping Firmware Volume
# before placing it in memory, and the offset that introduces between the
# standalone StandaloneMmCore.dll/.debug file's own addressing and where
# BL2 actually puts it isn't a simple, verified constant (tracked as a
# follow-up — see the note at the end of this section). Until that's
# nailed down: `stepi` past the branch, then `x/10i $pc` and `info symbol
# $pc` to orient yourself, or cross-reference disassembly against `nm
# .../StandaloneMmCore.debug` by hand.

# --- BL33: GenFds decides where things land per build; the DEBUG build's
# own serial log prints a ready-to-paste line for every driver as it
# loads — literally `add-symbol-file <path> 0x<addr>`. Copy them
# straight into gdb as they scroll past. (DxeCore itself is one of these
# — search the log for "DxeCore.dll" if you want to break there first.)
```

`ArmPkg/Drivers/CpuPei/CpuPei.inf` and friends print those `add-symbol-file`
lines automatically in DEBUG builds specifically so you can do this — it's
the intended workflow for source-level UEFI debugging on ARM, not a trick.

### What "normal" looks like at each stage

If you're not chasing a specific bug and just want to build intuition for
what a healthy boot looks like under a debugger:

- **BL1 → BL2**: one breakpoint hit, immediately. BL1's whole job is
  finding and jumping to BL2; there's nothing to step through inside it
  worth the trouble.
- **BL2 → BL31**: `bl2_main`/`bl2_image_load_v2.c` walks the FIP looking up
  each image (`BL32`, `BL33`) it was told to load — step through here to
  watch the actual image-loading decisions happen, useful if a stage isn't
  loading at all rather than crashing after it loads.
- **BL31 → BL32**: `bl31_main` → runtime service init → (if BL32 is
  present) a *synchronous entry* into BL32 that doesn't return until
  BL32's own init finishes. Stepping into that call and watching control
  genuinely leave BL31's context is the clearest way to see the secure-
  world handoff actually happen, rather than take it on faith from a log
  line.
- **BL32 (StandaloneMm)**: entry trampoline → `ArmStandaloneMmCoreEntryPoint`
  (conceptually — see the known gap below on actually breaking there by
  name) → HOB/Transfer List validation → `StandaloneMmCore`'s own PE
  loader brings up its four
  MM drivers in sequence (`StandaloneMmCpu`, `NorFlashStandaloneMm`,
  `FaultTolerantWriteStandaloneMm`, `VariableStandaloneMm`) — each one is
  itself a PE-COFF image loaded at runtime, so `info symbol $pc` here is
  more useful than a pre-set breakpoint until you've narrowed down which
  driver you care about.
- **BL33**: ordinary UEFI DXE dispatch — same experience as debugging
  Q35/ArmVirt once you're here, just with a longer path to get to it.

### Known gap: BL32 symbol-name breakpoints

Confirmed live while writing this guide: BL2 and BL31 are plain ELF
binaries loaded exactly as linked, so `add-symbol-file <elf> <load addr>`
gives you working `break <function_name>` immediately (`break bl31_main`
lands correctly on `bl31/bl31_main.c:108` with a sane backtrace). BL32
(StandaloneMm) is a PE-COFF image originally wrapped in a Firmware Volume,
which BL2 extracts before placing it at `PLAT_QEMU_SP_IMAGE_BASE` — and the
naive `add-symbol-file StandaloneMmCore.debug 0x20008000` doesn't land
symbols where the disassembly actually shows them (confirmed: it resolves
`$pc` to an unrelated function elsewhere in the file, off by a nontrivial,
not-yet-derived offset). Raw-address breakpoints work perfectly regardless
— this only affects the convenience of breaking by name once inside
StandaloneMm's own code. Worth someone nailing down the exact
extraction/relocation BL2 does for SP images if this becomes a recurring
annoyance; not attempted further here to keep this guide's claims honest
rather than confidently wrong.

## Debugging BL32/StandaloneMm's own console separately

BL1/BL2/BL31/BL33 share one non-secure console (`UART0`, `-serial stdio`
— the only one `qemu.sh` wires up by default). BL31's own crash console is
`UART1`; StandaloneMm's console is `UART2`, per
`SbsaOrreryStandaloneMm.dsc`'s `PcdSerialRegisterBase`. QEMU maps
successive `-serial` flags to successive UARTs, so to see StandaloneMm's
own prints (not just gdb's view of it), add two more:

```sh
./qemu.sh -d -g -- -serial file:/tmp/uart1.log -serial file:/tmp/uart2.log
```

`/tmp/uart2.log` gets StandaloneMm's own console — confirmed useful in
practice for watching real `MM_COMMUNICATE` requests land
(`Received delegated event`, register dumps, comm-buffer sizes) as BL33
talks to it, independent of whatever you're stepping through with gdb.

## Troubleshooting

- **`Connection refused` on attach**: QEMU isn't actually up yet, or a
  previous instance is still holding port 1234. Check `ps aux | grep
  qemu-system`; a stray one from an earlier session is the usual cause.
- **Breakpoint never hits**: you attached *after* execution already passed
  that address. `-g` halts at reset specifically so this shouldn't happen
  for early breakpoints — if it does anyway, you're probably reusing a
  QEMU instance that wasn't actually launched with `-g`. Relaunch.
- **"No such file" from `add-symbol-file`**: the ELF/DLL you're pointing
  at hasn't been built, or you built RELEASE instead of DEBUG. `find
  Build -name '*.dll'` (edk2 modules) or check `trusted-firmware-a/build/
  qemu_sbsa/debug/` (TF-A stages) to confirm the path.
- **Symbols resolve to the wrong function / garbage disassembly**: almost
  always a stale `add-symbol-file` from a *previous* stage still loaded at
  an overlapping address. Each `add-symbol-file` in the recipe above adds
  a symbol table; it doesn't replace the previous stage's. If you're
  confused about what you're looking at, `info symbol $pc` is the fastest
  way to check gdb's own belief about where you are.
- **On SBSA specifically, "I fixed the bug but the exact same thing still
  happens"**: before doubting the fix, check whether `build.sh -F` was
  needed. `vars/SBSA_FLASH1_*.fd` (where BL33 lives) is deliberately only
  seeded once, to preserve variables a previous boot persisted — which
  means a plain rebuild during active BL33 development can silently keep
  booting *old* code no matter how many times you fix and rebuild. See
  `docs/sbsa_boot_flow.md`'s note on this — it cost real time getting the
  MM_COMMUNICATE wiring right.
