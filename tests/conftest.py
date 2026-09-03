"""A skipped test isn't a pass -- it's "nothing was verified." pytest's
default exit code doesn't distinguish the two (a run with only skips still
exits 0), which lets a green CI check hide the fact that nothing actually
ran. Force the exit status to failure whenever any test is skipped, so a
skip is exactly as visible as a failure -- both locally and in CI.

Also collects BOOT_EVIDENCE (see record_evidence, called from
test_boot.py) -- the actual boot-phase markers matched in each platform's
captured log -- and, when running in CI, writes it to the job summary.
That's so a green check is backed by visible proof (which markers, from
which log, with a snippet) right on the Actions run page, not just a
"passed" line that has to be taken on faith.

Also provides fixtures for the offline-tooling unit tests (issue #24):
tools/ holds standalone scripts, not an importable package -- there's no
__init__.py and the filenames aren't valid module paths to import from a
test that lives elsewhere. Loading them through importlib keeps them
runnable as plain `python3 tools/foo.py` scripts (which is how build.sh
invokes them) while still letting the tests reach their individual
functions.
"""
import importlib.util
import os
import pathlib
import shutil
import sys

import pytest

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOLS_DIR = REPO_ROOT / "tools"


def _load(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


@pytest.fixture(scope="session")
def sign_rom_ticket():
    return _load("sign_rom_ticket", TOOLS_DIR / "sign_rom_ticket.py")


@pytest.fixture(scope="session")
def generate_signing_key():
    return _load("generate_signing_key", TOOLS_DIR / "generate_signing_key.py")


@pytest.fixture(scope="session")
def openssl_required():
    """Both tools shell out to openssl; skip rather than fail if it's absent."""
    if shutil.which("openssl") is None:
        pytest.skip("openssl not on PATH")


@pytest.fixture(scope="session")
def signing_key(tmp_path_factory, generate_signing_key, openssl_required):
    """A throwaway RSA-2048 keypair, generated once for the whole session.

    Explicitly writes both outputs into a tmp dir. The default paths point at
    tools/keys/update_signing_key.pem and the *tracked*
    OrreryPkg/Include/TrustedUpdateKey.h — a test run must never touch either,
    or it would silently rewrite the real compiled-in signing key.
    """
    out = tmp_path_factory.mktemp("signing-key")
    key = out / "test_key.pem"
    header = out / "TrustedUpdateKey.h"
    generate_signing_key.main(["--key-out", str(key), "--header-out", str(header)])
    return key, header


BOOT_EVIDENCE = []


def record_evidence(platform, log_path, markers, log):
    """Record what was actually found in `log` against `markers`, whether
    or not the calling test goes on to pass -- so the summary reflects
    what happened even for a test that fails or raises right after this.
    """
    found = [m for m in markers if m in log]
    missing = [m for m in markers if m not in log]
    BOOT_EVIDENCE.append(
        {
            "platform": platform,
            "log_path": str(log_path),
            "found": found,
            "missing": missing,
            "snippet": "\n".join(log.splitlines()[-15:]),
        }
    )


def _write_job_summary():
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not summary_path or not BOOT_EVIDENCE:
        return

    lines = [
        "## Boot verification evidence",
        "",
        "Proof each platform actually booted -- not just that pytest exited "
        "0 -- these are the boot-phase markers matched against that run's "
        "own captured log.",
        "",
        "| Platform | Result | Markers matched |",
        "| --- | --- | --- |",
    ]
    for e in BOOT_EVIDENCE:
        total = len(e["found"]) + len(e["missing"])
        status = f"✅ {total}/{total} matched" if not e["missing"] else f"❌ {len(e['found'])}/{total} matched"
        marker_list = ", ".join(f"`{m}`" for m in e["found"]) or "(none)"
        lines.append(f"| {e['platform']} | {status} | {marker_list} |")

    lines.append("")
    for e in BOOT_EVIDENCE:
        lines.append(
            f"<details><summary>{e['platform']} -- last lines of "
            f"<code>{e['log_path']}</code></summary>\n"
        )
        lines.append("```")
        lines.append(e["snippet"] or "(log was empty)")
        lines.append("```")
        lines.append("</details>")
        lines.append("")

    with open(summary_path, "a") as f:
        f.write("\n".join(lines) + "\n")


def pytest_sessionfinish(session, exitstatus):
    terminalreporter = session.config.pluginmanager.get_plugin("terminalreporter")
    skipped = terminalreporter.stats.get("skipped", [])
    if skipped and exitstatus == 0:
        terminalreporter.write_line(
            f"\n{len(skipped)} test(s) skipped -- treating the run as failed "
            "(a skip means nothing was verified, not that it passed)",
            red=True,
            bold=True,
        )
        session.exitstatus = 1
    _write_job_summary()
