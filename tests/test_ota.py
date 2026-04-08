import requests
import pytest


class TestOtaPassword:
    def test_ota_password_get_returns_json(self, base_url):
        r = requests.get(f"{base_url}/ota_password", timeout=10)
        assert r.status_code == 200
        body = r.json()
        assert "hasPassword" in body
        assert isinstance(body["hasPassword"], bool)


class TestOtaAuthCheck:
    def test_auth_check_without_password(self, base_url):
        """Without an X-OTA-Password header, the result depends on whether
        OTA password is configured: 200 if no password set, 401 if set."""
        r = requests.get(f"{base_url}/ota_auth_check", timeout=10)
        assert r.status_code in (200, 401)

    def test_auth_check_with_wrong_password(self, base_url):
        """If OTA password is set, a wrong password should return 401.
        If no password is set, any header is accepted (200)."""
        r = requests.get(
            f"{base_url}/ota_auth_check",
            headers={"X-OTA-Password": "definitely_wrong_password_12345"},
            timeout=10,
        )
        assert r.status_code in (200, 401)
