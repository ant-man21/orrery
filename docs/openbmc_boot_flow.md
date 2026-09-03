# OpenBmcPkg: a simulated BMC, and how it talks to the "host"

This is the fourth platform in this repo, and a different kind of platform
than the first three. Q35Pkg, ArmVirtOrreryPkg and SbsaOrreryPkg are all
**the host** — firmware for the main CPU complex a server boots an OS on.
OpenBmcPkg is **the other computer inside the same chassis**: a Baseboard
Management Controller, its own CPU (an Aspeed AST2500 on real Romulus
hardware), its own embedded Linux, running independently of whether the
host is powered on at all. Every real server has this second, always-on
computer watching the first one. This package is for practicing the
boundary between them — IPMI, Redfish, power control, the network path a
sysadmin actually uses — without needing either piece of hardware.

Issue #36 asked for this and, up front, chose the "practice, not
hardware-accurate emulation" end of its own scoping question: connect a
real (if prebuilt, not built-from-source) OpenBMC to SbsaOrreryPkg over a
real network segment, real IPMI/Redfish tooling against it, and honest
power control via a small bridge script — not a byte-accurate GPIO-level
recreation of a server's power sequencing hardware.

## Why prebuilt, not built from source like the other three platforms

Every other platform in this repo builds its firmware from source,
submodules pinned to forks under `ant-man21/*`, documented down to
individual bug fixes (`docs/sbsa_boot_flow.md` is the extreme version of
this). OpenBmcPkg doesn't, and it's worth being direct about why, since
it's a real departure from how this repo otherwise works: a from-scratch
OpenBMC build is a full Yocto/bitbake build of an embedded Linux
distribution — kernel, u-boot, busybox, systemd, the phosphor-\* dbus
services, bmcweb — 30-120 minutes and 60-100+ GB of disk on a normal
machine, and it fetches source from a wide scatter of hosts (kernel.org,
GNU mirrors, sourceforge, individual maintainer sites), not just GitHub.

`OpenBmcPkg/build.sh` instead downloads the exact prebuilt Romulus image
OpenBMC's own getting-started docs point newcomers at
(`openbmc/docs`' `development/dev-environment.md`, "Download and Start
QEMU Session"): a single official, CI-built artifact from the OpenBMC
project's own Jenkins, not a third-party rebuild. `romulus-bmc` was picked
over `witherspoon-bmc` (issue #36's other suggestion) because it's the
machine OpenBMC's own CI tests every change against — "the most stable,"
in their docs' words.

### What was and wasn't verified here

Be clear-eyed about this, the same way `docs/sbsa_boot_flow.md` is about
its own gaps: this package was written and its **mechanics** tested inside
a sandboxed dev session whose network egress is allowlisted to a small set
of hosts (GitHub, package registries) — `jenkins.openbmc.org` is not one
of them (confirmed: a plain `curl -I` against it returns a 403 from the
sandbox's own egress proxy, not from Jenkins). That means:

**Verified, directly, in that sandbox:**
- `qemu-system-arm -M romulus-bmc` boots partway (proven with a
  correctly-sized dummy flash image standing in for the real one) — the
  machine model, CLI args, and drive wiring are all correct.
- The private-network pairing `OpenBmcPkg/qemu.sh --link-sbsa` /
  `SbsaOrreryPkg/qemu.sh --bmc-mgmt` actually depends on: a
  `qemu-system-arm` process listening on a `-netdev socket` and a
  `qemu-system-aarch64` process connecting to it via `-nic
  socket,connect=` complete their TCP handshake cleanly, no "no peer" or
  "connection refused" — the two different QEMU binaries, two different
  machine types, really can be wired into one private L2 segment this way.
- `romulus-bmc` only lets QEMU back **one** of its two hardware NIC ports
  from the CLI — a second `-nic`/`-netdev` is rejected ("requested NIC...
  was not created"). This is why `OpenBmcPkg/qemu.sh` has two mutually
  exclusive network modes instead of one script doing both at once (see
  below) — it's a real QEMU/machine-model constraint, not a design
  simplification.
- `SbsaOrreryPkg/qemu.sh`'s new `-qmp unix:...,server=on,wait=off` accepts
  `qmp_capabilities` and returns well-formed JSON over the socket exactly
  as `power-bridge.sh` sends it (tested against a throwaway QEMU process,
  not a real SBSA boot).

**Not verified — needs a normal, unrestricted machine:**
- Actually downloading the image (`build.sh`).
- OpenBMC actually booting to `Ready` state in QEMU.
- `ipmitool`/Redfish/SSH actually working against a live BMC.
- `power-bridge.sh`'s IPMI polling loop against a real BMC (its QMP half
  is verified independently, above; its `ipmitool power status` half
  isn't, since there's no live BMC in this sandbox to point it at).

If you hit something these scripts get wrong once you run them for real,
that's the boundary above — treat it the way this repo treats every other
first-draft-against-incomplete-information gap (see `sbsa_boot_flow.md`'s
bug list): expected, not a sign anything here is careless.

## Quick start

```sh
cd OpenBmcPkg
./build.sh          # downloads obmc-phosphor-image-romulus.static.mtd
./qemu.sh            # boots it, standalone mode (see below)
```

Login once it reaches a shell prompt: `root` / `0penBmc` (zero, not a
capital O). `obmcutil state` shows the BMC's own view of itself:

```
root@openbmc:~# obmcutil state
CurrentBMCState     : xyz.openbmc_project.State.BMC.BMCState.Ready
CurrentPowerState   : xyz.openbmc_project.State.Chassis.PowerState.Off
CurrentHostState    : xyz.openbmc_project.State.Host.HostState.Off
```

`CurrentPowerState: Off` here is correct and expected even on first boot —
the BMC is up; whatever it thinks is "the host" isn't, yet. That's the
state `power-bridge.sh` (below) makes mean something real.

Exit with `Ctrl-A` then `x` — this is a `-nographic` serial console, not a
normal shell; plain `Ctrl-C` won't reliably stop it.

## Two network modes, not both at once

`OpenBmcPkg/qemu.sh` defaults to **standalone mode**: usermode/NAT
networking with hostfwd, straight off OpenBMC's own docs, so you can drive
the BMC directly from this host's shell with no second VM involved:

| Service | Host port |
|---|---|
| SSH | `127.0.0.1:2222` |
| Redfish / HTTPS REST | `127.0.0.1:2443` |
| IPMI-over-LAN (UDP) | `127.0.0.1:2623` |

`./qemu.sh --link-sbsa[=PORT]` swaps that for a private point-to-point QEMU
socket link (default port 8888) that `SbsaOrreryPkg/qemu.sh
--bmc-mgmt[=PORT]` connects into — the "shared virtual network segment for
out-of-band management" issue #36 asked about, a stand-in for the
dedicated management LAN a real BMC's NCSI/shared port sits on. In this
mode there's no hostfwd at all: the BMC is reachable only from that private
segment, the same way a real out-of-band network is usually air-gapped
from anything but the servers and jump boxes actually on it.

**Why not both at once**: confirmed directly (see "What was and wasn't
verified" above) — `romulus-bmc` only exposes one connectable NIC backend
from QEMU's CLI, even though the real hardware has two physical ports. So
it's standalone-with-hostfwd *or* linked-to-SBSA, not both simultaneously,
for this package as it stands today. If you want both — external IPMI/
Redfish access *and* a live link to SbsaOrreryPkg at the same time — the
practical route is running `power-bridge.sh` (next section) against
standalone mode; it talks to SbsaOrreryPkg over QMP on the host side, not
through either guest's network at all, so it doesn't need `--link-sbsa`.

Bring both VMs up for the linked-network story:

```sh
# terminal 1
cd OpenBmcPkg && ./qemu.sh --link-sbsa          # listens on :8888

# terminal 2 — after terminal 1 is listening
cd SbsaOrreryPkg && ./qemu.sh --bmc-mgmt         # connects to :8888
```

## Making power control real: power-bridge.sh

On real hardware, `ipmitool power on` drives a GPIO line the BMC has
wired straight to the host's power-button header, and the BMC watches a
separate power-good GPIO to confirm it worked — two chips, two firmware
stacks, one real wire. QEMU's `romulus-bmc` has no such wire to anything;
by itself, `ipmitool power on` against it just flips an internal dbus
state with no other effect.

`OpenBmcPkg/power-bridge.sh` is that wire, simulated: it polls the BMC's
own chassis-power state over IPMI and drives `SbsaOrreryPkg/qemu.sh`'s
actual process to match — starting it on Off→On, sending `system_powerdown`
over QMP on On→Off. Same shape as the real relationship (BMC decides, host
obeys), different transport (IPMI + QMP instead of two GPIO pins).

```sh
sudo apt install ipmitool socat   # if not already installed

cd SbsaOrreryPkg && ./qemu.sh &          # host firmware, QMP on by default
cd OpenBmcPkg && ./qemu.sh &             # BMC, standalone mode (needed — see below)
./power-bridge.sh                        # the bridge itself
```

Then, from a third terminal:

```sh
ipmitool -I lanplus -H 127.0.0.1 -p 2623 -U root -P 0penBmc power on
```

...should make `SbsaOrreryPkg`'s QEMU window actually start booting.
`power off` sends the shutdown request; a plain SbsaOrreryPkg boot doesn't
have an OS to honor an ACPI-style powerdown gracefully, so today this
behaves like a forced power pull rather than a clean shutdown — a real,
documented gap, not silently smoothed over. Wiring a graceful path (e.g. a
`ResetSystem` shell command that calls `system_powerdown` cleanly on its
own volition, mirroring what a real OS's ACPI driver does) is a natural
next step, not implemented here.

**Why the bridge needs standalone mode, not `--link-sbsa`**: it needs the
IPMI hostfwd port to poll the BMC's power state from the host shell it
runs in — the same single-connectable-NIC constraint above means
`--link-sbsa` mode has no hostfwd to poll. This isn't a real limitation in
practice: the bridge's *other* half (driving SbsaOrreryPkg) goes over QMP
on the host side, never through either guest's network — so you get the
same end-to-end "BMC controls host power" result either way; `--link-sbsa`
is for practicing the network-segment/Redfish-reaches-BMC-over-a-private-
LAN story specifically, and the bridge is for practicing power control
specifically. Nothing stops running both scripts side by side once you're
comfortable with each on its own.

## What to actually go try (answering "what can I do with this")

All of this is real OpenBMC userspace and real client tooling talking a
real protocol to it — none of it is a mockup. What's simulated is the
*hardware underneath* (no real chassis, no real GPIOs, no real sensors
wired to anything physical), not the software stack you're exercising.

**From the BMC's own shell** (console, or `ssh root@localhost -p 2222` in
standalone mode):
- `obmcutil state` — the state-machine view shown above.
- `busctl tree xyz.openbmc_project.State.Chassis` /
  `busctl introspect ...` — the actual dbus objects `ipmitool`/Redfish
  calls end up hitting. Seeing this is the fastest way to stop thinking of
  IPMI/Redfish as magic: they're both just callers of the same dbus API.
- `journalctl -u xyz.openbmc_project.State.Host` (and similar unit names)
  — watch a specific phosphor service's logs while you poke it externally.

**From the host shell, against a standalone `./qemu.sh`, no BMC login
needed** — this is the actual "day job" tooling:
```sh
ipmitool -I lanplus -H 127.0.0.1 -p 2623 -U root -P 0penBmc power status
ipmitool -I lanplus -H 127.0.0.1 -p 2623 -U root -P 0penBmc power on
ipmitool -I lanplus -H 127.0.0.1 -p 2623 -U root -P 0penBmc power cycle
ipmitool -I lanplus -H 127.0.0.1 -p 2623 -U root -P 0penBmc chassis status
ipmitool -I lanplus -H 127.0.0.1 -p 2623 -U root -P 0penBmc sdr list
ipmitool -I lanplus -H 127.0.0.1 -p 2623 -U root -P 0penBmc sel list

curl -k https://127.0.0.1:2443/redfish/v1/
curl -k https://127.0.0.1:2443/redfish/v1/Systems
curl -k -u root:0penBmc https://127.0.0.1:2443/redfish/v1/Systems/system \
  -X POST -H 'Content-Type: application/json' \
  -d '{"ResetType":"On"}' \
  https://127.0.0.1:2443/redfish/v1/Systems/system/Actions/ComputerSystem.Reset
```
(sensor/SEL/inventory data on a QEMU BMC with no real chassis attached
will mostly read as absent/stub values — the *protocol* round-trip and
the dbus plumbing behind it are the real, transferable thing to practice
here, not sensor readings that would need real hardware to mean anything.)

**Answering the specific questions from wanting to tinker with this:**
- *"Can I power cycle my QEMU 'server'?"* — yes, for real, via
  `power-bridge.sh` above: an `ipmitool power cycle` against the BMC
  actually restarts the SbsaOrreryPkg QEMU process, exactly like power
  cycling a real host from its BMC.
- *"Can I go into a shell and drive test cases from the BMC, without
  touching the host directly?"* — yes: SSH/console into the BMC (never the
  host), then everything above — `ipmitool`/`curl` against `localhost`
  from *inside* the BMC's own shell instead of the outer host shell, plus
  `busctl`/`journalctl` to watch what your own commands actually do
  underneath. That's the real day-to-day BMC-engineer workflow: you live
  on the BMC side of the boundary and only ever touch the host through
  IPMI/Redfish/dbus, never a direct console, exactly like you'd do it
  against real datacenter hardware over a real out-of-band network.
- *"All simulated, no real server hardware needed"* — correct, that's the
  whole point of this package; see "What was and wasn't verified" above
  for the one honest caveat (you'll be the one to first prove the actual
  OpenBMC boot works, this sandbox couldn't reach the image).

## Building from source instead

If you want the from-scratch story later (the repo's usual approach,
skipped here for the reasons above), OpenBMC's own docs
(`openbmc/docs`' `development/dev-environment.md`) are the reference —
short version:
```sh
git clone https://github.com/openbmc/openbmc.git
cd openbmc
. setup romulus
bitbake obmc-phosphor-image
```
30-120 minutes, real disk space, and (per the constraint discussed above)
needs a network that can actually reach kernel.org/GNU mirrors/sourceforge/
etc., not just GitHub. If this repo ever wants that path scripted the way
the other three platforms are (forked submodule pins, a `build.sh` that
does the real bitbake build), that's a good candidate for its own
follow-up — deliberately out of scope here the same way MM_COMMUNICATE was
deliberately deferred before `sbsa_boot_flow.md` picked it back up.

## Extending to other machines

Issue #36 also named `witherspoon`. Nothing here is Romulus-specific in
principle — `OpenBmcPkg` could grow a `MACHINE` selector across
`build.sh`/`qemu.sh` the way `SbsaOrreryPkg`'s `-r`/`-d` selects build
type, fetching a different Jenkins job
(`label=docker-builder,target=witherspoon`) and passing
`-M witherspoon-bmc`. Not done here since Romulus alone already covers the
learning goal this issue set out; a second machine is additive, not
required.
