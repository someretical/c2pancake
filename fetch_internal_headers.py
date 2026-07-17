#!/usr/bin/env python3

import argparse
import logging
from pathlib import Path
import requests

OWNER = "llvm"
REPO = "llvm-project"
REF = "llvmorg-22.1.8"
START_PATHS = ["clang/lib/CodeGen", "clang-tools-extra/clangd"]
OUTPUT_DIR = Path("include") / REPO

API = f"https://api.github.com/repos/{OWNER}/{REPO}/contents"

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(name)s - %(levelname)s | %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
logger = logging.getLogger(__name__)


def download_headers(path: str, ref: str):
    url = f"{API}/{path}?ref={ref}"
    logger.info(f"Scanning {path}")

    r = requests.get(url)
    r.raise_for_status()

    total = sum(1 for item in r.json() if item["type"] == "file" and item["name"].endswith(".h"))

    cur = 0
    for item in r.json():
        if item["type"] == "file" and item["name"].endswith(".h"):
            cur += 1

            out_file = OUTPUT_DIR / path / item["name"]
            out_file.parent.mkdir(parents=True, exist_ok=True)

            logger.info(f"[{cur:>{len(str(total))}}/{total}] Downloading {item['path']}")

            resp = requests.get(item["download_url"])
            resp.raise_for_status()

            out_file.write_bytes(resp.content)


def main():
    parser = argparse.ArgumentParser(description="Fetch Clang internal headers from GitHub")
    parser.add_argument(
        "-v",
        "--version",
        action="store",
        default=REF,
        help="Clang internal headers version to fetch (default: %(default)s)",
    )

    args = parser.parse_args()

    logger.info(f"Fetching Clang internal headers from {OWNER}/{REPO} at ref {args.version}")

    OUTPUT_DIR.mkdir(exist_ok=True)
    for start_path in START_PATHS:
        download_headers(start_path, args.version)
    logger.info("Done")


if __name__ == "__main__":
    main()
