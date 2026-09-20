"""Verify that ordinary test runs cannot trigger the opt-in shutdown test."""

from pathlib import Path

import pytest

import test_shutdown


pytest_plugins = ["pytester"]


@pytest.mark.parametrize("enabled,url", [(None, "http://unused.invalid"), ("1", None)])
def test_missing_opt_in_skips_before_discovery(pytester, monkeypatch, enabled, url):
    for name, value in (("DEVBENCH_TEST_SHUTDOWN", enabled), ("DEVBENCH_URL", url)):
        if value is None:
            monkeypatch.delenv(name, raising=False)
        else:
            monkeypatch.setenv(name, value)
    pytester.makeconftest('''
import pytest
@pytest.fixture
def client():
    pytest.fail("client discovery ran before opt-in")
''')
    pytester.makepyfile(test_opt_in=Path(test_shutdown.__file__).read_text(encoding="utf-8"))
    result = pytester.runpytest("-q")
    result.assert_outcomes(skipped=1)
