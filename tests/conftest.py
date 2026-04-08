import itertools
import os
import time
import urllib.parse

import pytest
import requests

# ── CLI options ──────────────────────────────────────────────────────

def pytest_addoption(parser):
    parser.addoption(
        "--target",
        default=os.environ.get("RT4K_TARGET", "http://192.168.4.1"),
        help="Base URL of the ESP32 device (default: http://192.168.4.1 or $RT4K_TARGET)",
    )
    parser.addoption(
        "--stress-iterations",
        type=int,
        default=50,
        help="Number of iterations for stress tests (0 = infinite until Ctrl+C)",
    )


# ── Markers ──────────────────────────────────────────────────────────

def pytest_configure(config):
    config.addinivalue_line("markers", "stress: long-running stress / soak tests")


# ── Session-scoped fixtures ──────────────────────────────────────────

@pytest.fixture(scope="session")
def base_url(request):
    url = request.config.getoption("--target").rstrip("/")
    # Quick connectivity check
    try:
        r = requests.get(url, timeout=5)
        r.raise_for_status()
    except requests.RequestException as exc:
        pytest.skip(f"Device unreachable at {url}: {exc}")
    return url


@pytest.fixture(scope="session")
def stress_iterations(request):
    return request.config.getoption("--stress-iterations")


# ── Function-scoped fixtures ─────────────────────────────────────────

@pytest.fixture()
def sd_access(base_url):
    """Enable SD access before the test, disable after."""
    r = requests.post(f"{base_url}/sd_access", data={"enable": "1"}, timeout=10)
    assert r.status_code == 200
    body = r.json()
    if not body.get("enabled"):
        pytest.skip("Could not enable SD access (RT4K may be using SD card)")
    yield
    requests.post(f"{base_url}/sd_access", data={"enable": "0"}, timeout=10)


@pytest.fixture()
def test_folder(base_url, sd_access):
    """Create a unique test folder on the SD card, clean up after."""
    folder = f"/System Volume Information/pytest_{int(time.time())}"
    # Create the folder by uploading a placeholder file into it
    placeholder = f"{folder}/.keep"
    upload_file(base_url, folder, ".keep", b"placeholder")
    yield folder
    # Teardown: delete everything in the folder
    try:
        items = list_dir(base_url, folder)
        for item in items:
            path = f"{folder}/{item['name']}"
            if item["type"] == "dir":
                # one-level deep cleanup
                sub_items = list_dir(base_url, path)
                for si in sub_items:
                    delete_file(base_url, f"{path}/{si['name']}")
                # delete subfolder placeholder if any remains
            else:
                delete_file(base_url, path)
    except Exception:
        pass  # best-effort cleanup


# ── File generator fixtures ──────────────────────────────────────────

@pytest.fixture()
def small_text_file():
    """~500 byte text file."""
    content = ("The quick brown fox jumps over the lazy dog.\n" * 11).encode()
    return ("small_test.txt", content)


@pytest.fixture()
def medium_file():
    """1 MB binary file."""
    content = os.urandom(1 * 1024 * 1024)
    return ("medium_1mb.bin", content)


@pytest.fixture()
def large_file():
    """4 MB binary file."""
    content = os.urandom(4 * 1024 * 1024)
    return ("large_4mb.bin", content)


# ── Helper functions (importable by test modules) ────────────────────

def upload_file(base_url, dest_folder, filename, content):
    """Upload a file via multipart POST to /upload?path=<dest_folder>."""
    url = f"{base_url}/upload?path={urllib.parse.quote(dest_folder, safe='')}"
    files = {"data": (filename, content)}
    r = requests.post(url, files=files, timeout=60)
    return r


def list_dir(base_url, path):
    """GET /list?path=<path> and return parsed JSON list."""
    url = f"{base_url}/list?path={urllib.parse.quote(path, safe='')}"
    r = requests.get(url, timeout=10)
    r.raise_for_status()
    return r.json()


def download_file(base_url, path):
    """GET /download?path=<path> and return (status_code, bytes)."""
    url = f"{base_url}/download?path={urllib.parse.quote(path, safe='')}"
    r = requests.get(url, timeout=60)
    return r.status_code, r.content


def delete_file(base_url, path):
    """GET /delete?path=<path> and return response text."""
    url = f"{base_url}/delete?path={urllib.parse.quote(path, safe='')}"
    r = requests.get(url, timeout=10)
    return r


def rename_file(base_url, old_path, new_name):
    """POST /rename with oldPath and newName."""
    r = requests.post(
        f"{base_url}/rename",
        data={"oldPath": old_path, "newName": new_name},
        timeout=10,
    )
    return r


def stress_range(n):
    """Return range(n) if n > 0, else itertools.count(0) for infinite looping."""
    return range(n) if n > 0 else itertools.count()


def finite_or_once(n):
    """Return range(n) if n > 0, else range(1) — used by individual tests
    so they run a single pass when infinite mode is active."""
    return range(n) if n > 0 else range(1)
