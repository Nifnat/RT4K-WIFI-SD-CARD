import time

import pytest
import requests

from conftest import upload_file, list_dir, download_file, delete_file, rename_file


class TestUploadAndList:
    """Upload files of various sizes and verify they appear in the directory listing."""

    def test_upload_small_text(self, base_url, test_folder, small_text_file):
        name, content = small_text_file
        r = upload_file(base_url, test_folder, name, content)
        assert r.status_code == 200
        assert r.text == "ok"

        items = list_dir(base_url, test_folder)
        names = [i["name"] for i in items]
        assert name in names

    def test_upload_1mb(self, base_url, test_folder, medium_file):
        name, content = medium_file
        r = upload_file(base_url, test_folder, name, content)
        assert r.status_code == 200
        assert r.text == "ok"

        items = list_dir(base_url, test_folder)
        entry = next((i for i in items if i["name"] == name), None)
        assert entry is not None
        assert int(entry["size"]) == len(content)

    @pytest.mark.timeout(120)
    def test_upload_4mb(self, base_url, test_folder, large_file):
        name, content = large_file
        r = upload_file(base_url, test_folder, name, content)
        assert r.status_code == 200
        assert r.text == "ok"

        items = list_dir(base_url, test_folder)
        entry = next((i for i in items if i["name"] == name), None)
        assert entry is not None
        assert int(entry["size"]) == len(content)


class TestDownload:
    """Upload files then download them, checking byte-for-byte integrity."""

    def test_download_small_text(self, base_url, test_folder, small_text_file):
        name, content = small_text_file
        upload_file(base_url, test_folder, name, content)

        status, downloaded = download_file(base_url, f"{test_folder}/{name}")
        assert status == 200
        assert downloaded == content

    def test_download_1mb(self, base_url, test_folder, medium_file):
        name, content = medium_file
        upload_file(base_url, test_folder, name, content)

        status, downloaded = download_file(base_url, f"{test_folder}/{name}")
        assert status == 200
        assert downloaded == content

    @pytest.mark.timeout(120)
    def test_download_4mb(self, base_url, test_folder, large_file):
        name, content = large_file
        upload_file(base_url, test_folder, name, content)

        status, downloaded = download_file(base_url, f"{test_folder}/{name}")
        assert status == 200
        assert downloaded == content


class TestListSubdirectory:
    """Test navigating into a subdirectory."""

    def test_list_subfolder(self, base_url, test_folder, small_text_file):
        name, content = small_text_file
        subfolder = f"{test_folder}/subdir"
        upload_file(base_url, subfolder, name, content)

        # The parent should show "subdir" as a directory
        items = list_dir(base_url, test_folder)
        dirs = [i for i in items if i["type"] == "dir"]
        dir_names = [d["name"] for d in dirs]
        assert "subdir" in dir_names

        # The subfolder should contain the file
        sub_items = list_dir(base_url, subfolder)
        sub_names = [i["name"] for i in sub_items]
        assert name in sub_names


class TestRename:
    """Test file rename operations."""

    def test_rename_file(self, base_url, test_folder, small_text_file):
        name, content = small_text_file
        upload_file(base_url, test_folder, name, content)

        new_name = "renamed_test.txt"
        r = rename_file(base_url, f"{test_folder}/{name}", new_name)
        assert r.status_code == 200
        assert r.text == "ok"

        items = list_dir(base_url, test_folder)
        names = [i["name"] for i in items]
        assert name not in names
        assert new_name in names

    def test_rename_to_existing_name(self, base_url, test_folder, small_text_file):
        name, content = small_text_file
        upload_file(base_url, test_folder, name, content)
        upload_file(base_url, test_folder, "other.txt", b"other content")

        r = rename_file(base_url, f"{test_folder}/{name}", "other.txt")
        assert "DESTEXISTS" in r.text

    def test_rename_nonexistent(self, base_url, test_folder):
        r = rename_file(base_url, f"{test_folder}/does_not_exist.txt", "new.txt")
        assert "SOURCEMISSING" in r.text or r.status_code != 200


class TestDelete:
    """Test file deletion."""

    def test_delete_file(self, base_url, test_folder, small_text_file):
        name, content = small_text_file
        upload_file(base_url, test_folder, name, content)

        r = delete_file(base_url, f"{test_folder}/{name}")
        assert r.status_code == 200
        assert r.text == "ok"

        items = list_dir(base_url, test_folder)
        names = [i["name"] for i in items]
        assert name not in names


class TestRelinquish:
    """Test the SD relinquish endpoint."""

    def test_relinquish(self, base_url):
        r = requests.get(f"{base_url}/relinquish", timeout=10)
        assert r.status_code == 200
        assert r.text == "ok"


class TestModeline:
    """Test reading a modeline file (read-only, no POST)."""

    def test_download_modeline_returns_200_or_404(self, base_url, sd_access):
        """Modeline files may or may not exist — either status is acceptable."""
        status, _ = download_file(base_url, "/modelines/custom1.txt")
        assert status in (200, 404)
