"""Boot smoke tests: launch each platform's qemu.sh headless and check its
debug.log for markers proving each firmware stage actually ran (SEC/DXE/BDS,
or TF-A's BL1->BL32->BL33 chain for SBSA) -- not just that QEMU started.

Requires each platform to already be built (see <Platform>/build.sh). Tests
for an unbuilt platform are skipped, not failed, so `pytest` is safe to run
without having built everything first.
"""
from __future__ import annotations

import os
import select
import signal
import subprocess
import time
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent


def _pick_build_flag(
    fv_file_template: str,
    build_dir_names: tuple[str, str] = ("DEBUG_GCC", "RELEASE_GCC"),
) -> str | None:
    """fv_file_template has one '{bt}' placeholder for the DEBUG/RELEASE
    build-dir name -- most platforms use edk2's own "<BUILD_TYPE>_<TOOLCHAIN>"
    convention (DEBUG_GCC/RELEASE_GCC, the default), but SBSA's padded vars/
    images use plain "DEBUG"/"RELEASE" instead (see build.sh's own
    FLASH0_PADDED/FLASH1_PADDED naming) -- pass build_dir_names=("DEBUG",
    "RELEASE") for those. Returns the qemu.sh flag (-d/-r) for whichever
    build exists, preferring DEBUG (richer trace output) if both do. None
    if neither is built.
    """
    debug_dir, release_dir = build_dir_names
    for build_dir, flag in ((debug_dir, "-d"), (release_dir, "-r")):
        if (REPO_ROOT / fv_file_template.format(bt=build_dir)).exists():
            return flag
    return None


def _boot_and_capture(
    platform_dir: Path,
    qemu_flag: str,
    debug_log: Path,
    final_marker: str,
    timeout: float,
    swtpm_pattern: str | None = None,
) -> str:
    """Launch `qemu.sh <qemu_flag>` headless (no -H — this is the AI/CI-safe
    default), poll debug_log until final_marker shows up or timeout expires,
    then tear the whole thing down and return what was captured.
    """
    debug_log.unlink(missing_ok=True)

    proc = subprocess.Popen(
        ["./qemu.sh", qemu_flag],
        cwd=platform_dir,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,  # own process group -> we can kill qemu + its `tee` together
    )
    try:
        deadline = time.monotonic() + timeout
        # Poll the log itself rather than trusting qemu.sh's own lifecycle:
        # Q35's qemu.sh backgrounds the real QEMU with `&` and waits on
        # `tail -f --pid=` as its last command, which can return well before
        # QEMU actually does in this non-interactive/no-tty context. Whether
        # the wrapper script is still alive doesn't tell us whether the
        # actual firmware is still booting, so don't treat its exit as a
        # reason to stop waiting for the marker.
        while time.monotonic() < deadline:
            if debug_log.exists() and final_marker in debug_log.read_text(errors="replace"):
                break
            time.sleep(1)
    finally:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        # Belt-and-suspenders: Q35's qemu.sh backgrounds the real QEMU
        # process with `&` (unlike SBSA/ArmVirt's foreground `tee` pipe),
        # and bash puts that backgrounded child in its own process group --
        # killpg above never reaches it, leaking a live QEMU that holds a
        # write-lock on the platform's disk images for the next test. Match
        # by the platform dir, which is unique to every -drive/-pflash path
        # each platform's qemu.sh passes, to catch stragglers regardless of
        # which process group they ended up in.
        subprocess.run(["pkill", "-9", "-f", f"qemu-system.*{platform_dir}"], check=False)
        # swtpm daemonizes (--daemon) and detaches from qemu.sh's process
        # group entirely, so nothing above ever reaches it either.
        if swtpm_pattern:
            subprocess.run(["pkill", "-f", swtpm_pattern], check=False)

    return debug_log.read_text(errors="replace") if debug_log.exists() else ""


def _assert_markers(log: str, markers: list[str], platform: str) -> None:
    missing = [m for m in markers if m not in log]
    if missing:
        tail = "\n".join(log.splitlines()[-40:])
        pytest.fail(
            f"{platform}: missing boot markers {missing}\n"
            f"--- last 40 lines of debug.log ---\n{tail}"
        )


def test_q35_boots():
    platform_dir = REPO_ROOT / "Q35Pkg"
    flag = _pick_build_flag("Build/OvmfX64/{bt}/FV/OVMF_CODE.fd")
    if flag is None:
        pytest.skip("Q35 firmware not built -- run Q35Pkg/build.sh first")
    if flag != "-d":
        # Q35Pkg/debug.log comes from -debugcon, which only carries anything
        # useful on a DEBUG build (RELEASE strips the DEBUG()-level traces).
        pytest.skip("Q35's debug.log needs a DEBUG build -- run Q35Pkg/build.sh -d")

    log = _boot_and_capture(
        platform_dir,
        flag,
        debug_log=platform_dir / "debug.log",
        final_marker="[BdsDxe] Locate Variable Policy protocol - Success",
        timeout=90,
        swtpm_pattern=f"swtpm socket.*{platform_dir}",
    )
    _assert_markers(
        log,
        [
            # NOTE: "SEC: Normal boot" (the very first -debugcon line) is
            # deliberately not checked -- observed dropped under load, a
            # race between the chardev backend's file being ready and
            # SEC's first write, not a boot failure. The two markers below
            # are later, real forward-progress signals and have been
            # reliable across many runs; that one early line hasn't been.
            "BdsDxe.efi",
            "[BdsDxe] Locate Variable Policy protocol - Success",
        ],
        "Q35",
    )


def test_armvirt_boots():
    platform_dir = REPO_ROOT / "ArmVirtOrreryPkg"
    flag = _pick_build_flag("Build/ArmVirtQemu-AArch64/{bt}/FV/QEMU_EFI.fd")
    if flag is None:
        pytest.skip("ArmVirt firmware not built -- run ArmVirtOrreryPkg/build.sh first")

    log = _boot_and_capture(
        platform_dir,
        flag,
        debug_log=platform_dir / "debug.log",
        final_marker="Shell>",
        timeout=60,
        swtpm_pattern=f"swtpm socket.*{platform_dir}",
    )
    _assert_markers(
        log,
        [
            "UEFI firmware",
            "BdsDxe: loading Boot",
            "UEFI Interactive Shell",
            "Shell>",
        ],
        "ArmVirt",
    )


def _drive_shell(
    platform_dir: Path,
    qemu_flag: str,
    commands: list[tuple[str, str, float]],
    overall_timeout: float,
    swtpm_pattern: str | None = None,
) -> str:
    """Launch `qemu.sh <qemu_flag> --reset-shared` headless, then drive the
    UEFI shell over its stdin/stdout pipe: for each (wait_for, send, timeout)
    in `commands`, block until `wait_for` appears in the output, then write
    `send` to the guest's serial console. Returns everything captured.

    This is for shell.efi interaction (typing commands, reading responses)
    -- test_*_boots above only ever watches a log file passively, which
    can't drive an interactive session. `select()`-based reads are
    required here, not `readline()`: QEMU's stdout is a pipe, not a tty, so
    it's block-buffered -- `readline()` blocks past any timeout waiting for
    a full buffered chunk that may not come until the process exits.
    """
    proc = subprocess.Popen(
        ["./qemu.sh", qemu_flag, "--reset-shared"],
        cwd=platform_dir,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
        start_new_session=True,
    )
    fd = proc.stdout.fileno()
    buf = b""

    def read_until(marker: str, timeout: float) -> bool:
        nonlocal buf
        deadline = time.monotonic() + timeout
        marker_b = marker.encode()
        while time.monotonic() < deadline:
            remaining = max(0.0, deadline - time.monotonic())
            r, _, _ = select.select([fd], [], [], min(1.0, remaining))
            if fd in r:
                chunk = os.read(fd, 65536)
                if not chunk:
                    break
                buf += chunk
                if marker_b in buf:
                    return True
        return marker_b in buf

    try:
        for wait_for, send, timeout in commands:
            read_until(wait_for, timeout)
            if send:
                proc.stdin.write(send.encode())
                proc.stdin.flush()
        # settle time for whatever the last command's output was still
        # producing when its own wait_for/timeout above was satisfied
        deadline = time.monotonic() + min(3.0, overall_timeout)
        while time.monotonic() < deadline:
            r, _, _ = select.select([fd], [], [], 0.5)
            if fd in r:
                chunk = os.read(fd, 65536)
                if not chunk:
                    break
                buf += chunk
    finally:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        # See _boot_and_capture's comment on this same pattern -- Q35's
        # qemu.sh no longer backgrounds QEMU (fixed), but this backstop
        # costs nothing and protects every platform against the same class
        # of leak if that ever regresses.
        subprocess.run(["pkill", "-9", "-f", f"qemu-system.*{platform_dir}"], check=False)
        if swtpm_pattern:
            subprocess.run(["pkill", "-f", swtpm_pattern], check=False)

    return buf.decode(errors="replace")


def test_q35_tpm_provision_and_lock():
    """End-to-end TPM provisioning on a freshly-wiped TPM: measure ROM into
    PCR15, prove it against the build's signed ticket, write a secret to an
    NV index gated on that policy, then write-lock the index so it can
    never be overwritten again (issue #9). Runs via a startup.nsh -- same
    mechanism a human would use from tools/startup.nsh.template -- so this
    doubles as a check that startup.nsh actually works, not just that
    TpmProvisionApp.efi's own logic is correct in isolation.

    Note for anyone adding a fs1:-only startup.nsh test after this one: the
    Shell auto-runs startup.nsh off of *any* mapped filesystem it finds one
    on, not just fs0: (confirmed directly -- contradicts the assumption in
    tools/startup.nsh.template's own header comment, which should probably
    be fixed). That's why this test cleans up shared.img afterward: leaving
    a startup.nsh baked into it would make test_q35_boots's plain "does it
    reach a shell" check auto-launch this TPM flow instead, breaking on an
    unrelated marker for a reason that has nothing to do with what it
    checks.
    """
    platform_dir = REPO_ROOT / "Q35Pkg"
    flag = _pick_build_flag("Build/OvmfX64/{bt}/FV/OVMF_CODE.fd")
    if flag is None:
        pytest.skip("Q35 firmware not built -- run Q35Pkg/build.sh first")

    sync_list = platform_dir / "shared" / ".sync"
    synced_apps = sync_list.read_text().splitlines() if sync_list.exists() else []
    if "TpmProvisionApp.efi" not in synced_apps:
        pytest.skip("TpmProvisionApp.efi not in Q35Pkg/shared/.sync -- won't be synced into shared.img")

    tpm_dir = platform_dir / "chips" / "q35" / "tpm"
    tpm_dir.mkdir(parents=True, exist_ok=True)
    for f in tpm_dir.iterdir():
        f.unlink()

    shared_dir = platform_dir / "shared"
    startup_nsh = shared_dir / "startup.nsh"
    (shared_dir / "apps").mkdir(parents=True, exist_ok=True)
    (shared_dir / "data").mkdir(parents=True, exist_ok=True)
    startup_nsh.write_text(
        "echo -off\n"
        "fs1:\n"
        "apps\\TpmProvisionApp.efi\n"
        "if %lasterror% == 0 then\n"
        '  echo "SENTINEL: PROVISION_PASS"\n'
        "else\n"
        '  echo "SENTINEL: PROVISION_FAIL lasterror=%lasterror%"\n'
        "endif\n"
    )

    try:
        log = _drive_shell(
            platform_dir,
            flag,
            commands=[
                ("SENTINEL:", "", 60),
            ],
            overall_timeout=90,
            swtpm_pattern=f"swtpm socket.*{platform_dir}",
        )
    finally:
        # shared.img already has this startup.nsh baked in by the
        # --reset-shared _drive_shell just did -- deleting the source file
        # alone wouldn't undo that. Delete the image too so the next
        # qemu.sh run (any test, any platform script) regenerates a clean
        # one without it (every qemu.sh rebuilds shared.img if missing).
        startup_nsh.unlink(missing_ok=True)
        (platform_dir / "shared.img").unlink(missing_ok=True)

    if "SENTINEL: PROVISION_PASS" not in log:
        tail = "\n".join(log.splitlines()[-40:])
        pytest.fail(f"TPM provision+lock did not report PASS\n--- last 40 lines ---\n{tail}")
    assert "Secret written to NV index" in log
    assert "write-locked" in log


def test_sbsa_boots():
    platform_dir = REPO_ROOT / "SbsaOrreryPkg"
    # Checks the padded vars/ image qemu.sh itself requires, not
    # Build/SbsaQemu -- the latter is an intermediate build.sh doesn't
    # need to ship (and CI's firmware-sbsa artifact deliberately excludes
    # it), so checking for it here made this test skip unconditionally
    # even when SBSA was, in fact, built.
    flag = _pick_build_flag(
        "SbsaOrreryPkg/vars/SBSA_FLASH0_{bt}.fd",
        build_dir_names=("DEBUG", "RELEASE"),
    )
    if flag is None:
        pytest.skip("SBSA firmware not built -- run SbsaOrreryPkg/build.sh first")

    log = _boot_and_capture(
        platform_dir,
        flag,
        debug_log=platform_dir / "debug.log",
        final_marker="Boot0001: UEFI Shell",
        timeout=90,  # full TF-A BL1->BL2->BL31->BL32->BL33 chain, slower than the others
        swtpm_pattern=None,  # SBSA has no TPM device wired yet
    )
    _assert_markers(
        log,
        [
            "NOTICE:  Booting Trusted Firmware",
            "Secure Partition initialized.",
            "Boot0001: UEFI Shell",
        ],
        "SBSA",
    )
