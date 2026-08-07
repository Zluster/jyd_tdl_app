#!/usr/bin/env python3
"""Manage dynamic Chinese keywords for the Sherpa Zipformer KWS model.

The model recognizes pinyin initials/finals rather than Hanzi.  This tool
keeps a human-readable registry and renders the keyword file consumed by
``sherpa-onnx-keyword-spotter``.  New keywords do not require model training.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import tempfile
from pathlib import Path
from typing import Iterable


INITIALS = (
    "zh", "ch", "sh", "b", "p", "m", "f", "d", "t", "n", "l", "g", "k",
    "h", "j", "q", "x", "r", "z", "c", "s", "y", "w",
)
SYLLABIC_FINALS = {"m", "n", "ng", "hm", "hng", "er"}
TONE_MARKS = {
    "a": ("\u0101", "\u00e1", "\u01ce", "\u00e0"),
    "e": ("\u0113", "\u00e9", "\u011b", "\u00e8"),
    "i": ("\u012b", "\u00ed", "\u01d0", "\u00ec"),
    "o": ("\u014d", "\u00f3", "\u01d2", "\u00f2"),
    "u": ("\u016b", "\u00fa", "\u01d4", "\u00f9"),
    "\u00fc": ("\u01d6", "\u01d8", "\u01da", "\u01dc"),
}
TONE3_RE = re.compile(r"^([a-z\u00fc:]+)([0-5]?)$", re.IGNORECASE)


def atomic_write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=path.parent, delete=False) as f:
        f.write(content)
        temp_path = Path(f.name)
    os.replace(temp_path, path)


def is_hanzi(char: str) -> bool:
    return "\u4e00" <= char <= "\u9fff"


def load_tokens(path: Path) -> set[str]:
    tokens: set[str] = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if fields:
            tokens.add(fields[0])
    if not tokens:
        raise ValueError(f"empty token vocabulary: {path}")
    return tokens


def load_registry(path: Path) -> list[dict[str, object]]:
    if not path.exists():
        return []
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or not isinstance(data.get("keywords"), list):
        raise ValueError(f"invalid registry: {path}")
    return list(data["keywords"])


def write_registry(path: Path, entries: Iterable[dict[str, object]]) -> None:
    payload = {"version": 1, "keywords": list(entries)}
    atomic_write(path, json.dumps(payload, ensure_ascii=False, indent=2) + "\n")


def tone3_to_marked(syllable: str) -> str:
    value = syllable.strip().lower().replace("u:", "\u00fc").replace("v", "\u00fc")
    match = TONE3_RE.fullmatch(value)
    if not match:
        raise ValueError(f"invalid pinyin syllable: {syllable!r}")
    base, tone_text = match.groups()
    tone = int(tone_text or "5")
    if tone == 5:
        return base
    if "a" in base:
        index = base.index("a")
    elif "e" in base:
        index = base.index("e")
    elif "ou" in base:
        index = base.index("o")
    else:
        index = max((i for i, char in enumerate(base) if char in TONE_MARKS), default=-1)
    if index < 0:
        raise ValueError(f"cannot place a tone mark in: {syllable!r}")
    vowel = base[index]
    return base[:index] + TONE_MARKS[vowel][tone - 1] + base[index + 1 :]


def split_initial(syllable: str) -> tuple[str, str]:
    value = syllable.lower().replace("u:", "\u00fc").replace("v", "\u00fc")
    if value in SYLLABIC_FINALS:
        return "", value
    for initial in INITIALS:
        if value.startswith(initial) and len(value) > len(initial):
            return initial, value[len(initial) :]
    return "", value


def pinyin_to_model_tokens(syllables: Iterable[str], vocabulary: set[str]) -> tuple[list[str], list[str]]:
    rendered: list[str] = []
    normalized_syllables: list[str] = []
    for raw_syllable in syllables:
        raw_syllable = raw_syllable.strip()
        if not raw_syllable:
            continue
        match = TONE3_RE.fullmatch(raw_syllable.lower().replace("u:", "\u00fc").replace("v", "\u00fc"))
        if not match:
            raise ValueError(f"invalid pinyin syllable: {raw_syllable!r}")
        base, tone = match.groups()
        initial, final = split_initial(base)
        final_marked = tone3_to_marked(final + (tone or "5"))
        if initial:
            rendered.append(initial)
        rendered.append(final_marked)
        normalized_syllables.append(base + (tone or "5"))
    if not rendered:
        raise ValueError("keyword has no pinyin syllables")
    unknown = [token for token in rendered if token not in vocabulary]
    if unknown:
        raise ValueError("model vocabulary does not contain token(s): " + ", ".join(unknown))
    return rendered, normalized_syllables


def auto_pinyin(text: str) -> list[str]:
    try:
        from pypinyin import Style, lazy_pinyin
    except ImportError as exc:
        raise ValueError(
            "automatic Hanzi conversion needs pypinyin. Install the board bundle's tools/pypinyin "
            "or pass --pinyin 'ni3 hao3'."
        ) from exc
    if not text or any(not is_hanzi(char) and not char.isspace() for char in text):
        raise ValueError("automatic conversion accepts Chinese Hanzi only; use --pinyin for other text")
    result = lazy_pinyin(text, style=Style.TONE3, neutral_tone_with_five=True, errors="raise")
    return [str(item).lower() for item in result if str(item).strip()]


def render_keywords(entries: Iterable[dict[str, object]]) -> str:
    lines: list[str] = []
    for entry in entries:
        text = str(entry["text"])
        token_sequences = [entry["tokens"]]
        variants = entry.get("token_variants", [])
        if not isinstance(variants, list):
            raise ValueError(f"invalid token variants for keyword {text!r}")
        token_sequences.extend(variants)
        seen: set[tuple[str, ...]] = set()
        for tokens in token_sequences:
            if not isinstance(tokens, list) or not all(isinstance(token, str) for token in tokens):
                raise ValueError(f"invalid token list for keyword {text!r}")
            key = tuple(tokens)
            if not key or key in seen:
                continue
            seen.add(key)
            fields = list(tokens)
            if entry.get("score") is not None:
                fields.append(f":{float(entry['score']):g}")
            if entry.get("threshold") is not None:
                fields.append(f"#{float(entry['threshold']):g}")
            fields.append("@" + text)
            lines.append(" ".join(fields))
    return "\n".join(lines) + ("\n" if lines else "")


def add_keyword(args: argparse.Namespace) -> None:
    vocabulary = load_tokens(args.tokens)
    entries = load_registry(args.registry)
    text = args.text.strip()
    if not text or "@" in text or "\n" in text:
        raise ValueError("keyword text must be non-empty and cannot contain '@' or a newline")
    syllables = args.pinyin.split() if args.pinyin else auto_pinyin(text)
    tokens, normalized_syllables = pinyin_to_model_tokens(syllables, vocabulary)
    new_entry = {"text": text, "pinyin": " ".join(normalized_syllables), "tokens": tokens}
    if args.score is not None:
        if args.score <= 0:
            raise ValueError("keyword score must be greater than 0")
        new_entry["score"] = args.score
    if args.threshold is not None:
        if not 0 < args.threshold <= 1:
            raise ValueError("keyword threshold must be in the range (0, 1]")
        new_entry["threshold"] = args.threshold
    entries = [entry for entry in entries if entry.get("text") != text]
    entries.append(new_entry)
    write_registry(args.registry, entries)
    atomic_write(args.keywords, render_keywords(entries))
    print(json.dumps(new_entry, ensure_ascii=False))


def remove_keyword(args: argparse.Namespace) -> None:
    entries = load_registry(args.registry)
    remaining = [entry for entry in entries if entry.get("text") != args.text]
    if len(remaining) == len(entries):
        raise ValueError(f"keyword not found: {args.text}")
    write_registry(args.registry, remaining)
    atomic_write(args.keywords, render_keywords(remaining))
    print(json.dumps({"removed": args.text}, ensure_ascii=False))


def list_keywords(args: argparse.Namespace) -> None:
    for entry in load_registry(args.registry):
        print(json.dumps(entry, ensure_ascii=False))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Register arbitrary Chinese keywords for Sherpa Zipformer KWS.")
    parser.add_argument("--tokens", type=Path, required=True, help="KWS model tokens.txt")
    parser.add_argument("--registry", type=Path, required=True, help="Persistent JSON registry")
    parser.add_argument("--keywords", type=Path, required=True, help="Rendered KWS keywords file")
    subparsers = parser.add_subparsers(dest="command", required=True)

    add = subparsers.add_parser("add", help="Add or replace a keyword")
    add.add_argument("--text", required=True, help="Display text, normally Chinese Hanzi")
    add.add_argument("--pinyin", help="Optional explicit tone-number pinyin, e.g. 'chong2 qing4'")
    add.add_argument("--score", type=float, help="Optional per-keyword boosting score")
    add.add_argument("--threshold", type=float, help="Optional per-keyword trigger threshold")
    add.set_defaults(handler=add_keyword)

    remove = subparsers.add_parser("remove", help="Remove a keyword")
    remove.add_argument("--text", required=True)
    remove.set_defaults(handler=remove_keyword)

    listing = subparsers.add_parser("list", help="Print registered keywords as JSON lines")
    listing.set_defaults(handler=list_keywords)
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        args.handler(args)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
