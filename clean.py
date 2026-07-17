#!/usr/bin/env python3

import argparse
import logging
from pathlib import Path

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(name)s - %(levelname)s | %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
logger = logging.getLogger(__name__)

def main():
    parser = argparse.ArgumentParser(description="Remove all generated files from the tests directory")
    args = parser.parse_args()

    test_dir = Path(__file__).parent / "tests"
    if not test_dir.exists():
        logger.warning(f"Test directory {test_dir} does not exist. Nothing to clean.")
        return
    
    files = [file for file in test_dir.rglob("*-c2pnk.c") if file.is_file() if file.is_file()]
    
    for file in files:
        logger.info(f"[{files.index(file) + 1:>{len(str(len(files)))}}/{len(files)}] Removing file: {file}")
        file.unlink()


if __name__ == "__main__":
    main()
