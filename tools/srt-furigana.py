#!/usr/bin/env python3
"""Generate beginner-friendly variants of a Japanese .srt file.

For input foo.ja.srt, writes:
  foo.ja-hira.srt      -- kanji words replaced by their hiragana reading
                          (katakana and latin text left untouched)
  foo.ja-furigana.srt  -- original text, with the reading in brackets
                          after each word that contains kanji; trailing
                          okurigana stays outside the brackets

Requires fugashi + unidic-lite (pip install fugashi unidic-lite).
"""

import re
import sys
from pathlib import Path

import fugashi

KANJI = re.compile(r"[㐀-鿿豈-﫿々〆〇]")
KANA = re.compile(r"[ぁ-ゖァ-ヺー]")


def kata_to_hira(s):
    return "".join(
        chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in s
    )


def reading(word):
    kana = word.feature.kana or word.feature.pron
    return kata_to_hira(kana) if kana else word.surface


def with_furigana(surface, hira):
    """映し出さ + うつしださ -> 映し出（うつしだ）さ"""
    tail = 0
    while (
        tail < len(surface) - 1
        and KANA.match(surface[-1 - tail])
        and tail < len(hira) - 1
        and kata_to_hira(surface[-1 - tail]) == hira[-1 - tail]
    ):
        tail += 1
    stem_s = surface[: len(surface) - tail]
    stem_h = hira[: len(hira) - tail]
    okuri = surface[len(surface) - tail :]
    return f"{stem_s}（{stem_h}）{okuri}"


def convert_line(tagger, line):
    hira_out, furi_out = [], []
    for w in tagger(line):
        if KANJI.search(w.surface):
            r = reading(w)
            hira_out.append(r)
            furi_out.append(with_furigana(w.surface, r))
        else:
            hira_out.append(w.surface)
            furi_out.append(w.surface)
    return "".join(hira_out), "".join(furi_out)


def main():
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} <subtitles.ja.srt>")
    src = Path(sys.argv[1])
    stem = re.sub(r"\.ja$", "", src.stem)
    out_hira = src.with_name(stem + ".ja-hira.srt")
    out_furi = src.with_name(stem + ".ja-furigana.srt")

    tagger = fugashi.Tagger()
    timing = re.compile(r"^\d+$|^\s*$|-->")

    hira_lines, furi_lines = [], []
    for line in src.read_text(encoding="utf-8").splitlines():
        if timing.search(line):
            hira_lines.append(line)
            furi_lines.append(line)
        else:
            hira, furi = convert_line(tagger, line)
            hira_lines.append(hira)
            furi_lines.append(furi)

    out_hira.write_text("\n".join(hira_lines) + "\n", encoding="utf-8")
    out_furi.write_text("\n".join(furi_lines) + "\n", encoding="utf-8")
    print(f"wrote {out_hira}\nwrote {out_furi}")


if __name__ == "__main__":
    main()
