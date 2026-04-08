import requests
import pytest


class TestCorsHeaders:
    """Verify CORS and Private Network Access headers on OPTIONS preflight."""

    @pytest.mark.parametrize("path", ["/", "/upload", "/list", "/delete"])
    def test_options_cors_headers(self, base_url, path):
        r = requests.options(f"{base_url}{path}", timeout=10)
        assert r.status_code == 200

        assert "Access-Control-Allow-Origin" in r.headers
        assert r.headers["Access-Control-Allow-Origin"] == "*"

        assert "Access-Control-Allow-Private-Network" in r.headers
        assert r.headers["Access-Control-Allow-Private-Network"] == "true"

    def test_get_also_has_cors_origin(self, base_url):
        """Regular GET responses should also include CORS headers."""
        r = requests.get(f"{base_url}/", timeout=10)
        assert r.status_code == 200
        assert "Access-Control-Allow-Origin" in r.headers
