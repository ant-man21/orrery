"""A skipped test isn't a pass -- it's "nothing was verified." pytest's
default exit code doesn't distinguish the two (a run with only skips still
exits 0), which lets a green CI check hide the fact that nothing actually
ran. Force the exit status to failure whenever any test is skipped, so a
skip is exactly as visible as a failure -- both locally and in CI.
"""


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
