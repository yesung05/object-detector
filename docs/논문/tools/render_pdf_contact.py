"""Render selected PDF pages to a temporary contact sheet for visual review."""

from __future__ import annotations

import argparse
from pathlib import Path

import fitz
from PIL import Image, ImageDraw


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("pdf")
    parser.add_argument("output")
    parser.add_argument("pages", nargs="+", type=int, help="zero-based page numbers")
    args = parser.parse_args()
    document = fitz.open(args.pdf)
    thumbs = []
    for page_number in args.pages:
        pixmap = document[page_number].get_pixmap(matrix=fitz.Matrix(1.2, 1.2), alpha=False)
        image = Image.frombytes("RGB", (pixmap.width, pixmap.height), pixmap.samples)
        image.thumbnail((700, 1000), Image.Resampling.LANCZOS)
        framed = Image.new("RGB", (720, 1040), "white")
        framed.paste(image, ((720 - image.width) // 2, 30))
        ImageDraw.Draw(framed).text((10, 8), f"page {page_number + 1}", fill="black")
        thumbs.append(framed)
    columns = 2
    rows = (len(thumbs) + columns - 1) // columns
    contact = Image.new("RGB", (columns * 720, rows * 1040), (220, 220, 220))
    for index, image in enumerate(thumbs):
        contact.paste(image, ((index % columns) * 720, (index // columns) * 1040))
    contact.save(Path(args.output), quality=88)


if __name__ == "__main__":
    main()
