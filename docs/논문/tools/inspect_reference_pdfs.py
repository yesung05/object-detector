"""Extract compact review segments from local reference PDFs for manual citation review."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import fitz


KEYWORDS = [
    "abstract", "conclusion", "discussion", "background subtraction", "frame difference",
    "abandoned object", "stationary object", "occlusion", "false positive", "false alarm",
    "cache", "reuse", "redundant", "video analytics", "dataset", "litter", "trash",
    "초록", "결론", "논의", "무인", "쓰레기", "무단투기", "이상행동", "배경 차분",
    "가림", "오탐", "데이터셋", "환경", "위생", "청결", "감시", "범죄",
]


def normalize(text: str) -> str:
    text = text.replace("\u00ad", "").replace("\x00", " ")
    text = re.sub(r"[ \t]+", " ", text)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip()


def contexts(text: str, limit: int = 14) -> list[dict[str, str | int]]:
    lowered = text.lower()
    found = []
    occupied: list[tuple[int, int]] = []
    for keyword in KEYWORDS:
        start = 0
        while len(found) < limit:
            index = lowered.find(keyword.lower(), start)
            if index < 0:
                break
            left, right = max(0, index - 420), min(len(text), index + len(keyword) + 650)
            start = index + len(keyword)
            if any(not (right < a or left > b) for a, b in occupied):
                continue
            occupied.append((left, right))
            found.append({"keyword": keyword, "offset": index, "text": text[left:right]})
    return found


def inspect(path: Path) -> dict:
    document = fitz.open(path)
    page_text = [normalize(page.get_text("text")) for page in document]
    full = "\n\n".join(f"[PAGE {index + 1}]\n{text}" for index, text in enumerate(page_text))
    metadata = {key: value for key, value in document.metadata.items() if value}
    return {
        "file": path.name,
        "pages": len(document),
        "metadata": metadata,
        "opening": full[:5000],
        "contexts": contexts(full),
        "ending": full[-4500:],
    }


def main() -> None:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser()
    parser.add_argument("files", nargs="+")
    args = parser.parse_args()
    for name in args.files:
        result = inspect(Path(name))
        print("\n===== PDF REVIEW =====")
        print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
