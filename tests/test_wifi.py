import requests
import pytest


class TestWifiStatus:
    def test_wifistatus_returns_wifi_prefix(self, base_url):
        r = requests.get(f"{base_url}/wifistatus", timeout=10)
        assert r.status_code == 200
        assert r.text.startswith("WIFI:")


class TestWifiScan:
    def test_wifiscan_returns_ok(self, base_url):
        r = requests.get(f"{base_url}/wifiscan", timeout=10)
        assert r.status_code == 200
        assert r.text == "ok"

    def test_wifilist_returns_json_array(self, base_url):
        """Trigger a scan then fetch results. List may be empty if scan is still running."""
        requests.get(f"{base_url}/wifiscan", timeout=10)
        # Give the scan a moment
        import time
        time.sleep(3)
        r = requests.get(f"{base_url}/wifilist", timeout=10)
        assert r.status_code == 200
        data = r.json()
        assert isinstance(data, list)
        # If results came back, verify structure
        if len(data) > 0:
            entry = data[0]
            assert "ssid" in entry
            assert "rssi" in entry
