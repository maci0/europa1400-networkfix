#!/usr/bin/env python3
"""Lightweight frame checker: hashes screenshots to detect blank/black/unchanged."""
import sys, hashlib
from pathlib import Path

def blank_or_black(p):
    try:
        from PIL import Image
        im=Image.open(p).convert('L')
        hist=im.histogram()
        # blank if >95% pixels are same dark value
        return max(hist)/sum(hist) > 0.95
    except Exception:
        return False

def file_hash(p):
    h=hashlib.sha256()
    h.update(Path(p).read_bytes())
    return h.hexdigest()[:12]

if __name__ == "__main__":
    if len(sys.argv)<2:
        print("usage: check_frame.py <png> [expected_hash]")
        sys.exit(2)
    p=sys.argv[1]
    h=file_hash(p)
    blk=blank_or_black(p)
    print(f"{p}: sha={h} blank={blk}")
    if blk: print(f"{p}: blank frame", file=sys.stderr)
    if len(sys.argv)>=3 and h!=sys.argv[2]:
        print(f"mismatch expected {sys.argv[2]}", file=sys.stderr)
        sys.exit(1)
