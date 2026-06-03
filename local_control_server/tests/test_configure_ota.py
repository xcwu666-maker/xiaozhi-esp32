import tempfile
import unittest
from pathlib import Path

from local_control_server.configure_ota_url import update_config_text


class ConfigureOtaTests(unittest.TestCase):
    def test_update_existing_ota_url(self):
        original = 'CONFIG_OTA_URL="https://api.tenclass.net/xiaozhi/ota/"\nCONFIG_OTHER=y\n'

        updated = update_config_text(original, "http://192.168.35.100:8000/xiaozhi/ota/")

        self.assertIn('CONFIG_OTA_URL="http://192.168.35.100:8000/xiaozhi/ota/"', updated)
        self.assertIn("CONFIG_OTHER=y", updated)
        self.assertNotIn("api.tenclass.net", updated)

    def test_append_missing_ota_url(self):
        updated = update_config_text("CONFIG_OTHER=y\n", "http://192.168.35.100:8000/xiaozhi/ota/")

        self.assertIn("CONFIG_OTHER=y", updated)
        self.assertIn('CONFIG_OTA_URL="http://192.168.35.100:8000/xiaozhi/ota/"', updated)


if __name__ == "__main__":
    unittest.main()

