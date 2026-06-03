"""Update ESP32 sdkconfig OTA URL for the local PC-side service."""

from __future__ import annotations

import argparse
from pathlib import Path


def update_config_text(text: str, ota_url: str) -> str:
    line = f'CONFIG_OTA_URL="{ota_url}"'
    lines = text.splitlines()
    replaced = False
    output: list[str] = []
    for item in lines:
        if item.startswith("CONFIG_OTA_URL="):
            output.append(line)
            replaced = True
        else:
            output.append(item)
    if not replaced:
        output.append(line)
    return "\n".join(output) + "\n"


def update_config_file(path: Path, ota_url: str) -> None:
    original = path.read_text(encoding="utf-8")
    updated = update_config_text(original, ota_url)
    path.write_text(updated, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description="Set CONFIG_OTA_URL for the local Xiaozhi server")
    parser.add_argument("ota_url", help="Example: http://192.168.35.100:8000/xiaozhi/ota/")
    parser.add_argument("--sdkconfig", default="sdkconfig", help="Path to sdkconfig")
    args = parser.parse_args()

    path = Path(args.sdkconfig)
    if not path.exists():
        raise SystemExit(f"sdkconfig not found: {path}")
    update_config_file(path, args.ota_url)
    print(f"Updated {path}: CONFIG_OTA_URL={args.ota_url}")


if __name__ == "__main__":
    main()

