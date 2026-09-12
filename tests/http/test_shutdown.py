"""Opt-in Windows shutdown test. Exits the selected Skyrim process without saving."""

from __future__ import annotations

import ctypes
import os
from ctypes import wintypes

import pytest
import requests


pytestmark = pytest.mark.skipif(
    os.name != "nt"
    or os.environ.get("DEVBENCH_TEST_SHUTDOWN") != "1"
    or not os.environ.get("DEVBENCH_URL"),
    reason="requires Windows, DEVBENCH_TEST_SHUTDOWN=1 and an explicit DEVBENCH_URL",
)


def test_qqq_completes_process_exit(client):
    health = client.ok("inspect", {"kind": "health"})
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    kernel.GetExitCodeProcess.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL

    handle = kernel.OpenProcess(0x100000 | 0x1000, False, health["pid"])
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        assert kernel.WaitForSingleObject(handle, 0) == 258, "game already exited"
        assert client.ok("inspect", {"kind": "health"})["pid"] == health["pid"]
        try:
            client.ok("console", {"command": "qqq"})
        except requests.ConnectionError:
            pass  # The process can exit before HTTP sends its response.

        # DLL teardown can hang after the window closes and the exit code is set.
        # Only a signalled process handle establishes that termination completed.
        result = kernel.WaitForSingleObject(handle, 30000)
        if result == 0xFFFFFFFF:
            raise ctypes.WinError(ctypes.get_last_error())
        assert result == 0, f"Skyrim PID {health['pid']} did not exit within 30 seconds"
        exit_code = wintypes.DWORD()
        if not kernel.GetExitCodeProcess(handle, ctypes.byref(exit_code)):
            raise ctypes.WinError(ctypes.get_last_error())
        assert exit_code.value == 0, f"Skyrim exited with code {exit_code.value:#x}"
    finally:
        kernel.CloseHandle(handle)
