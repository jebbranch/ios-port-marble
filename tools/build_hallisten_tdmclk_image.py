#!/usr/bin/env python3
"""Build and verify the exact 0297 image with the vendor TDM clock vote."""
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "tools/build_hallisten_wait_image.py"
spec = importlib.util.spec_from_file_location("hallisten_0296_builder", SOURCE)
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)

builder.LABEL = "hallisten-0297-20261114"
builder.IMAGE = ROOT / "images" / (builder.LABEL + ".img")
builder.OUT = ROOT / "out" / builder.LABEL
builder.MARKERS = tuple(
    marker for marker in builder.MARKERS
    if not marker.startswith("SWEET MARK: halstream teardown v1 ")
) + (
    "SWEET MARK: halstream teardown v2 open=%d stop=%d close=%d "
    "tdmrel=%d unmap=%d",
    "SWEET MARK: listen tdmclk v2 request rc=%d",
    "SWEET MARK: listen tdmclk v2 release rc=%d",
)

if __name__ == "__main__":
    builder.main()
