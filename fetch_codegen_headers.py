#!/usr/bin/env python3

import argparse
import logging
from pathlib import Path
import requests

OWNER = "llvm"
REPO = "llvm-project"
REF = "llvmorg-22.1.7"
START_PATH = "clang/lib/CodeGen"
OUTPUT_DIR = Path("include/third_party")

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
        if item["type"] == "dir":
            download_headers(item["path"], ref)

        elif item["type"] == "file" and item["name"].endswith(".h"):
            cur += 1

            rel_path = Path(item["path"]).relative_to(START_PATH)
            out_file = OUTPUT_DIR / rel_path
            out_file.parent.mkdir(parents=True, exist_ok=True)

            logger.info(f"[{cur:>{len(str(total))}}/{total}] Downloading {item['path']}")

            resp = requests.get(item["download_url"])
            resp.raise_for_status()

            out_file.write_bytes(resp.content)


def main():
    parser = argparse.ArgumentParser(description="Fetch Clang CodeGen headers from GitHub")
    parser.add_argument(
        "-v",
        "--version",
        action="store",
        default=REF,
        help="Clang CodeGen version to fetch (default: %(default)s)",
    )

    args = parser.parse_args()

    logger.info(f"Fetching Clang CodeGen headers from {OWNER}/{REPO} at ref {args.version}")

    OUTPUT_DIR.mkdir(exist_ok=True)
    download_headers(START_PATH, args.version)
    logger.info("Done")


if __name__ == "__main__":
    main()
