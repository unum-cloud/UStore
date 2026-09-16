"""Session-wide configuration for the Python side of the UStore test suite."""

import pytest


def pytest_sessionfinish(session: pytest.Session, exitstatus: int) -> None:
    """Passes a run whose every module skips itself, which pytest otherwise reports as collecting nothing."""
    if exitstatus == pytest.ExitCode.NO_TESTS_COLLECTED:
        session.exitstatus = pytest.ExitCode.OK
