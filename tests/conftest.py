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
"""
import os

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
