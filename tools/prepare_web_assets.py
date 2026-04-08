from __future__ import annotations

import gzip
import shutil
import sys
from pathlib import Path


COMPRESSIBLE_EXTENSIONS = {
    ".css",
    ".htm",
    ".html",
    ".js",
    ".json",
    ".svg",
    ".txt",
    ".xml",
}


def should_compress(path: Path) -> bool:
    return path.suffix.lower() in COMPRESSIBLE_EXTENSIONS


def copy_and_compress_assets(src_dir: Path, dst_dir: Path) -> None:
    for src_path in src_dir.rglob("*"):
        rel_path = src_path.relative_to(src_dir)
        dst_path = dst_dir / rel_path

        if src_path.is_dir():
            dst_path.mkdir(parents=True, exist_ok=True)
            continue

        dst_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src_path, dst_path)

        if not should_compress(src_path):
            continue

        gz_path = dst_path.with_name(dst_path.name + ".gz")
        with src_path.open("rb") as in_file, gz_path.open("wb") as raw_out:
            with gzip.GzipFile(filename="", mode="wb", fileobj=raw_out, mtime=0) as gz_out:
                shutil.copyfileobj(in_file, gz_out)

        if gz_path.stat().st_size >= dst_path.stat().st_size:
            gz_path.unlink()


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: prepare_web_assets.py <src_dir> <dst_dir>", file=sys.stderr)
        return 1

    src_dir = Path(sys.argv[1]).resolve()
    dst_dir = Path(sys.argv[2]).resolve()

    if not src_dir.is_dir():
        print(f"source directory not found: {src_dir}", file=sys.stderr)
        return 1

    copy_and_compress_assets(src_dir, dst_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())