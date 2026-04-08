import requests
import pytest


class TestSdAccessGet:
    def test_returns_json_with_enabled_key(self, base_url):
        r = requests.get(f"{base_url}/sd_access", timeout=10)
        assert r.status_code == 200
        body = r.json()
        assert "enabled" in body
        assert isinstance(body["enabled"], bool)


class TestSdAccessToggle:
    def test_enable_then_disable(self, base_url):
        # Enable
        r = requests.post(f"{base_url}/sd_access", data={"enable": "1"}, timeout=10)
        assert r.status_code == 200
        body = r.json()
        if body.get("error"):
            pytest.skip(f"Cannot enable SD: {body['error']}")
        assert body["enabled"] is True

        # Verify GET reflects enabled
        r = requests.get(f"{base_url}/sd_access", timeout=10)
        assert r.json()["enabled"] is True

        # Disable
        r = requests.post(f"{base_url}/sd_access", data={"enable": "0"}, timeout=10)
        assert r.status_code == 200
        assert r.json()["enabled"] is False

        # Verify GET reflects disabled
        r = requests.get(f"{base_url}/sd_access", timeout=10)
        assert r.json()["enabled"] is False


class TestSdAccessRequired:
    def test_list_with_sd_enabled(self, base_url, sd_access):
        """With SD access enabled, /list should return a JSON array."""
        r = requests.get(f"{base_url}/list?path=/", timeout=10)
        assert r.status_code == 200
        data = r.json()
        assert isinstance(data, list)
