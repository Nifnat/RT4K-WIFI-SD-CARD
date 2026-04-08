import os
import sys
import time

import pytest
import requests

from conftest import (
    upload_file,
    list_dir,
    download_file,
    delete_file,
    rename_file,
    stress_range,
    finite_or_once,
)


# All tests in this module require SD access and are marked as stress tests
pytestmark = [pytest.mark.stress, pytest.mark.timeout(0)]

STRESS_FOLDER = "/System Volume Information/stress_test"


@pytest.fixture(autouse=True)
def _require_sd(sd_access):
    """All stress tests need SD access."""


@pytest.fixture(autouse=True)
def _ensure_stress_folder(base_url):
    """Make sure the stress folder exists before each test."""
    upload_file(base_url, STRESS_FOLDER, ".keep", b"stress")
    yield
    # Best-effort cleanup of .keep
    try:
        delete_file(base_url, f"{STRESS_FOLDER}/.keep")
    except Exception:
        pass


def _log_iteration(i, label=""):
    """Print iteration progress to stdout (visible with pytest -s)."""
    sys.stdout.write(f"\r  [{label}] iteration {i + 1}")
    sys.stdout.flush()


class TestStressUploadDelete:
    """Repeatedly upload and delete files to stress the SD card and web server."""

    def test_upload_delete_1mb_cycle(self, base_url, stress_iterations):
        content = os.urandom(1 * 1024 * 1024)
        name = "stress_1mb.bin"
        path = f"{STRESS_FOLDER}/{name}"

        for i in finite_or_once(stress_iterations):
            _log_iteration(i, "1MB upload/delete")
            r = upload_file(base_url, STRESS_FOLDER, name, content)
            assert r.status_code == 200, f"Upload failed on iteration {i + 1}: {r.text}"
            assert r.text == "ok"

            items = list_dir(base_url, STRESS_FOLDER)
            names = [it["name"] for it in items]
            assert name in names, f"File missing from list on iteration {i + 1}"

            r = delete_file(base_url, path)
            assert r.status_code == 200, f"Delete failed on iteration {i + 1}: {r.text}"
            assert r.text == "ok"

        print()

    def test_upload_delete_small_rapid(self, base_url, stress_iterations):
        content = b"small stress test content\n" * 20  # ~500 bytes
        name = "stress_small.txt"
        path = f"{STRESS_FOLDER}/{name}"
        n = stress_iterations * 2 if stress_iterations > 0 else stress_iterations

        for i in finite_or_once(n):
            _log_iteration(i, "small rapid")
            r = upload_file(base_url, STRESS_FOLDER, name, content)
            assert r.status_code == 200, f"Upload failed on iteration {i + 1}: {r.text}"

            r = delete_file(base_url, path)
            assert r.status_code == 200, f"Delete failed on iteration {i + 1}: {r.text}"

        print()


class TestStressDownload:
    """Upload a file once, then download it many times to stress read path."""

    def test_download_1mb_repeated(self, base_url, stress_iterations):
        content = os.urandom(1 * 1024 * 1024)
        name = "stress_dl_1mb.bin"
        path = f"{STRESS_FOLDER}/{name}"

        r = upload_file(base_url, STRESS_FOLDER, name, content)
        assert r.status_code == 200

        try:
            for i in finite_or_once(stress_iterations):
                _log_iteration(i, "1MB download")
                status, downloaded = download_file(base_url, path)
                assert status == 200, f"Download failed on iteration {i + 1}"
                assert downloaded == content, f"Content mismatch on iteration {i + 1}"
        finally:
            delete_file(base_url, path)

        print()


class TestStressList:
    """Hammer the list endpoint."""

    def test_list_root_repeated(self, base_url, stress_iterations):
        n = stress_iterations * 2 if stress_iterations > 0 else stress_iterations

        for i in finite_or_once(n):
            _log_iteration(i, "list /")
            r = requests.get(f"{base_url}/list?path=/", timeout=10)
            assert r.status_code == 200, f"List failed on iteration {i + 1}"
            data = r.json()
            assert isinstance(data, list), f"Non-list response on iteration {i + 1}"

        print()


class TestStressMixed:
    """Full lifecycle: upload → list → rename → download → delete."""

    def test_mixed_lifecycle(self, base_url, stress_iterations):
        n = max(stress_iterations // 3, 1) if stress_iterations > 0 else stress_iterations

        for i in finite_or_once(n):
            _log_iteration(i, "mixed lifecycle")
            files = {}
            # Upload 3 files
            for j in range(3):
                name = f"mixed_{j}.txt"
                content = f"Mixed test iteration {i + 1} file {j}\n".encode() * 50
                files[name] = content
                r = upload_file(base_url, STRESS_FOLDER, name, content)
                assert r.status_code == 200, f"Upload {name} failed: {r.text}"

            # List and verify all 3
            items = list_dir(base_url, STRESS_FOLDER)
            names = [it["name"] for it in items]
            for name in files:
                assert name in names, f"{name} not in listing on iteration {i + 1}"

            # Rename file 0
            old_name = "mixed_0.txt"
            new_name = "mixed_renamed.txt"
            r = rename_file(base_url, f"{STRESS_FOLDER}/{old_name}", new_name)
            assert r.status_code == 200, f"Rename failed: {r.text}"

            # Download file 1
            status, downloaded = download_file(base_url, f"{STRESS_FOLDER}/mixed_1.txt")
            assert status == 200
            assert downloaded == files["mixed_1.txt"]

            # Delete all
            for name in ["mixed_renamed.txt", "mixed_1.txt", "mixed_2.txt"]:
                r = delete_file(base_url, f"{STRESS_FOLDER}/{name}")
                assert r.status_code == 200, f"Delete {name} failed: {r.text}"

        print()


class TestStressInfinite:
    """Single test that cycles through ALL operations in a loop.
    Only runs when --stress-iterations=0 (infinite mode)."""

    def test_all_operations_loop(self, base_url, stress_iterations):
        if stress_iterations != 0:
            pytest.skip("Only runs in infinite mode (--stress-iterations 0)")

        content_1mb = os.urandom(1 * 1024 * 1024)
        content_small = b"small stress test content\n" * 20

        try:
            for i in stress_range(0):
                print(f"\n── Infinite loop iteration {i + 1} ──")

                # 1) Upload/delete 1MB
                _log_iteration(i, "1MB upload/delete")
                name = "inf_1mb.bin"
                path = f"{STRESS_FOLDER}/{name}"
                r = upload_file(base_url, STRESS_FOLDER, name, content_1mb)
                assert r.status_code == 200, f"1MB upload failed: {r.text}"
                items = list_dir(base_url, STRESS_FOLDER)
                assert name in [it["name"] for it in items]
                r = delete_file(base_url, path)
                assert r.status_code == 200, f"1MB delete failed: {r.text}"

                # 2) Small file rapid
                _log_iteration(i, "small rapid")
                name = "inf_small.txt"
                path = f"{STRESS_FOLDER}/{name}"
                r = upload_file(base_url, STRESS_FOLDER, name, content_small)
                assert r.status_code == 200, f"Small upload failed: {r.text}"
                r = delete_file(base_url, path)
                assert r.status_code == 200, f"Small delete failed: {r.text}"

                # 3) Download integrity
                _log_iteration(i, "download verify")
                name = "inf_dl.bin"
                path = f"{STRESS_FOLDER}/{name}"
                r = upload_file(base_url, STRESS_FOLDER, name, content_1mb)
                assert r.status_code == 200
                status, downloaded = download_file(base_url, path)
                assert status == 200 and downloaded == content_1mb, "Download mismatch"
                delete_file(base_url, path)

                # 4) List
                _log_iteration(i, "list")
                r = requests.get(f"{base_url}/list?path=/", timeout=10)
                assert r.status_code == 200
                assert isinstance(r.json(), list)

                # 5) Mixed lifecycle
                _log_iteration(i, "mixed lifecycle")
                mixed_files = {}
                for j in range(3):
                    fname = f"inf_mix_{j}.txt"
                    fcontent = f"iter {i + 1} file {j}\n".encode() * 50
                    mixed_files[fname] = fcontent
                    r = upload_file(base_url, STRESS_FOLDER, fname, fcontent)
                    assert r.status_code == 200

                r = rename_file(base_url, f"{STRESS_FOLDER}/inf_mix_0.txt", "inf_mix_r.txt")
                assert r.status_code == 200

                status, dl = download_file(base_url, f"{STRESS_FOLDER}/inf_mix_1.txt")
                assert status == 200 and dl == mixed_files["inf_mix_1.txt"]

                for fname in ["inf_mix_r.txt", "inf_mix_1.txt", "inf_mix_2.txt"]:
                    r = delete_file(base_url, f"{STRESS_FOLDER}/{fname}")
                    assert r.status_code == 200

                print(f"  ✓ iteration {i + 1} complete")

        except KeyboardInterrupt:
            print(f"\n\n  Stopped infinite loop after {i + 1} iterations")
            # Best-effort cleanup
            for fname in ["inf_1mb.bin", "inf_small.txt", "inf_dl.bin",
                          "inf_mix_0.txt", "inf_mix_r.txt", "inf_mix_1.txt", "inf_mix_2.txt"]:
                try:
                    delete_file(base_url, f"{STRESS_FOLDER}/{fname}")
                except Exception:
                    pass
