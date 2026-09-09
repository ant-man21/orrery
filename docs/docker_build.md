# Building with Docker

Compile Orrery's UEFI firmware (Q35, ArmVirtOrreryPkg, SbsaOrreryPkg)
without installing the edk2 toolchain on your machine. Works the same way
on Docker Desktop for Mac, Windows, and Linux — the actual compiling
always happens inside a Linux build container; your checkout is just
bind-mounted into it, so builds always compile *your local code*,
including uncommitted changes.

This is compile-only: the container builds the `.fd`/`.efi` outputs, but
running/booting them under QEMU (`qemu.sh`) is still a host-side step —
see `docs/setup.txt` / `docs/host_environment.txt` for that.

## Requirements

- Docker Desktop (Mac/Windows) or Docker Engine + Compose plugin (Linux)
- This repo cloned locally — submodules do **not** need to be initialized
  first; the container does that on its first run (see below).

## Quick start

```
git clone <this repo>
cd orrery
./docker-build.sh q35          # -> Build/OvmfX64/RELEASE_GCC/FV/
./docker-build.sh arm-virt      # -> Build/ArmVirtQemu-AArch64/RELEASE_GCC/FV/
./docker-build.sh sbsa           # -> Build/SbsaQemu/..., SbsaOrreryPkg/vars/
./docker-build.sh all            # build all three, in that order
```

`docker-build.sh` is a thin wrapper around `docker compose run --rm build`
(see `docker-compose.yml`) — use whichever you prefer:

```
docker compose run --rm build q35
```

The first run also builds the `orrery-build` image (installs the
toolchain) and initializes git submodules, so it takes a few minutes.
Later runs reuse both and are fast.

### Passing build.sh flags

Anything after the target is passed straight through to that platform's
own `build.sh` (`Q35Pkg/build.sh -h`, etc. for the full list):

```
./docker-build.sh q35 -d -C       # debug build, clean first
./docker-build.sh sbsa -M          # SBSA without BL32/StandaloneMm
```

### Interactive shell

```
./docker-build.sh bash
```

Drops you into the build image with the repo at `/workspace`, toolchain
on `PATH` — useful for debugging a build failure or running a platform's
`build.sh`/other tools by hand.

## What's actually happening

- `docker/Dockerfile` builds an Ubuntu 22.04 image with the exact
  toolchain `.github/workflows/build.yml` installs in CI (GCC cross
  compilers for X64/AArch64, iasl, nasm, mtools, libssl-dev, ...) — so a
  Docker build matches what CI does, not a separately-maintained list.
- `docker-compose.yml` bind-mounts the repo at `/workspace` in the
  container instead of copying it in, so build outputs land directly back
  in your checkout at the same paths a host-native build would use
  (`Build/...`, `<Platform>/vars/`, `<Platform>/shared.img`, ...) and
  nothing is left behind in the image itself.
- `docker/entrypoint.sh` runs `git submodule update --init --recursive`
  on first run (skipped once `edk2/edksetup.sh` exists), builds edk2's
  BaseTools, then dispatches to the requested platform's `build.sh`.

## Caveats

- **Private submodule remotes.** The container's git has no credentials
  mounted. If `edk2`/`edk2-platforms`/`edk2-non-osi`/`trusted-firmware-a`
  aren't public, run `git submodule update --init --recursive` on the
  host first (with your own git credentials) before the first Docker
  build — the container detects they're already populated and skips
  straight past that step.
- **File ownership on Linux hosts.** The container runs as root, so build
  outputs it creates (`Build/`, `edk2/Conf/`, etc.) end up root-owned on a
  native Linux host. If that's a problem, `sudo chown -R "$USER" Build
  */Build */vars` afterward, or add a `user: "${UID}:${GID}"` override to
  the `build` service in `docker-compose.yml`. Not an issue on Docker
  Desktop for Mac/Windows.
- **SBSA is the heaviest target** — it also builds trusted-firmware-a
  (BL1/BL2/BL31/BL32) on top of the edk2 (BL33) build, so it's slower and
  pulls in more submodules than `q35`/`arm-virt`.
