import requests
import pytest


class TestStaticFiles:
    """Test that SPIFFS-served static files are accessible and have correct headers."""

    @pytest.mark.parametrize("path,expected_type", [
        ("/", "text/html"),
        ("/index.htm", "text/html"),
        ("/settings.htm", "text/html"),
        ("/css/index.css", "text/css"),
        ("/css/bootstrap.min.css", "text/css"),
        ("/js/index.js", "application/javascript"),
        ("/js/bootstrap.min.js", "application/javascript"),
        ("/js/jquery-3.2.1.slim.min.js", "application/javascript"),
        ("/cm/codemirror.min.js", "application/javascript"),
        ("/cm/codemirror.min.css", "text/css"),
        ("/cm/darcula.min.css", "text/css"),
    ])
    def test_static_file_served(self, base_url, path, expected_type):
        r = requests.get(f"{base_url}{path}", timeout=10)
        assert r.status_code == 200, f"GET {path} returned {r.status_code}"
        ct = r.headers.get("Content-Type", "")
        assert expected_type in ct, f"Expected {expected_type} in Content-Type, got {ct}"

    def test_favicon(self, base_url):
        r = requests.get(f"{base_url}/favicon.ico", timeout=10)
        assert r.status_code == 200
        assert len(r.content) > 0

    @pytest.mark.parametrize("path,expected_type", [
        ("/index.htm", "text/html"),
        ("/css/index.css", "text/css"),
        ("/js/index.js", "application/javascript"),
    ])
    def test_gzip_encoding(self, base_url, path, expected_type):
        """Verify the server sends gzip-compressed responses when client accepts it."""
        r = requests.get(
            f"{base_url}{path}",
            headers={"Accept-Encoding": "gzip, deflate"},
            timeout=10,
        )
        assert r.status_code == 200
        # The server pre-compresses to .gz and sets Content-Encoding header
        ce = r.headers.get("Content-Encoding", "")
        assert "gzip" in ce, f"Expected gzip Content-Encoding for {path}, got '{ce}'"

    def test_404_for_nonexistent(self, base_url):
        r = requests.get(f"{base_url}/nonexistent_abc123.xyz", timeout=10)
        assert r.status_code == 404
