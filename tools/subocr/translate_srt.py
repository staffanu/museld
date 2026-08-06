#!/usr/bin/env python3
"""Translate a Japanese .srt to English cue-by-cue via the Claude API.

Prototype for museld's live subtitle translation: each cue is translated in a
separate request (as a real-time pipeline would), with a rolling window of the
previous cues (Japanese + English) supplied as context so pronouns, names and
register stay consistent. The stable system prompt is cached across calls.

Backends:
  --backend anthropic (default)  Claude API via the anthropic SDK.
                                 Credentials: ANTHROPIC_API_KEY.
  --backend openai               Any OpenAI-compatible /v1/chat/completions
                                 server (llama.cpp, vLLM, Ollama, ...), e.g.
                                 --backend openai --base-url http://xserver:8080
                                 --model picks the model; if omitted, the first
                                 model reported by /v1/models is used.

Usage: translate_srt.py ja.srt [--out en.srt] [--model NAME] [--context N]
"""

import argparse
import re
import sys
import time
from pathlib import Path

import httpx

SYSTEM = (
    "You translate Japanese film subtitles to English, one cue at a time.\n"
    "You are given the recent dialogue (Japanese with your English translations) "
    "as context, then one new Japanese cue.\n"
    "Reply with ONLY the English subtitle text for the new cue - no quotes, no "
    "notes, no Japanese. Keep it concise and natural, as a subtitle: idiomatic "
    "English, consistent names and pronouns with the context, preserve line "
    "breaks if the cue has two lines."
)

JAPANESE_RE = re.compile(r"[ぁ-ゖァ-ヺー々〆一-鿿]")

CUE_RE = re.compile(
    r"(\d+)\s*\n(\d{2}:\d{2}:\d{2},\d{3}) --> (\d{2}:\d{2}:\d{2},\d{3})\s*\n(.*?)(?:\n\n|\Z)",
    re.DOTALL,
)


def parse_srt(text):
    return [{"start": m[1], "end": m[2], "text": m[3].strip()} for m in CUE_RE.findall(text)]


def make_anthropic_backend(model):
    import anthropic

    client = anthropic.Anthropic()

    def translate(prompt):
        try:
            response = client.messages.create(
                model=model,
                max_tokens=256,
                system=[{"type": "text", "text": SYSTEM,
                         "cache_control": {"type": "ephemeral"}}],
                messages=[{"role": "user", "content": prompt}],
            )
        except anthropic.RateLimitError:
            time.sleep(10)
            response = client.messages.create(
                model=model, max_tokens=256, system=SYSTEM,
                messages=[{"role": "user", "content": prompt}],
            )
        return next(b.text for b in response.content if b.type == "text")

    return translate, model


def make_openai_backend(base_url, model, think=False):
    base = base_url.rstrip("/")
    if not base.endswith("/v1"):
        base += "/v1"
    http = httpx.Client(timeout=120.0)
    if not model:
        models = http.get(f"{base}/models").json()
        model = models["data"][0]["id"]

    def translate(prompt):
        # with thinking on, reasoning models burn tokens in reasoning_content
        # before emitting the answer in content — hence the generous cap
        r = http.post(f"{base}/chat/completions", json={
            "model": model,
            "max_tokens": 2048 if think else 256,
            "chat_template_kwargs": {"enable_thinking": think},
            "messages": [
                {"role": "system", "content": SYSTEM},
                {"role": "user", "content": prompt},
            ],
        })
        r.raise_for_status()
        return r.json()["choices"][0]["message"]["content"]

    return translate, model


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("srt", type=Path)
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument("--backend", choices=["anthropic", "openai"], default="anthropic")
    ap.add_argument("--base-url", default=None, help="OpenAI-compatible server URL")
    ap.add_argument("--model", default=None)
    ap.add_argument("--context", type=int, default=8, help="previous cues kept as context")
    ap.add_argument("--think", action="store_true",
                    help="openai backend: leave the model's thinking mode enabled")
    args = ap.parse_args()

    cues = parse_srt(args.srt.read_text(encoding="utf-8"))
    if not cues:
        sys.exit(f"no cues parsed from {args.srt}")
    out = args.out or args.srt.with_name(args.srt.stem.replace("ja", "en") + ".srt")

    if args.backend == "anthropic":
        translate, model = make_anthropic_backend(args.model or "claude-haiku-4-5")
    else:
        if not args.base_url:
            sys.exit("--backend openai requires --base-url")
        translate, model = make_openai_backend(args.base_url, args.model, args.think)
    print(f"backend={args.backend} model={model}")

    history = []       # (ja, en) dialogue pairs for the rolling context
    translations = []  # one English text per cue, for the output file
    total_ms = 0.0
    for i, cue in enumerate(cues):
        ctx_lines = []
        for ja, en in history[-args.context:]:
            ctx_lines.append(f"JA: {ja}")
            ctx_lines.append(f"EN: {en}")
        prompt = ""
        if ctx_lines:
            prompt += "Recent dialogue:\n" + "\n".join(ctx_lines) + "\n\n"
        prompt += f"New cue to translate:\n{cue['text']}"

        # Nothing to translate (credits, OCR garbage): pass through unchanged,
        # and keep it out of the rolling context
        if not JAPANESE_RE.search(cue["text"]):
            translations.append(cue["text"])
            print(f"[{cue['start']}] {'':>8} {cue['text']!r} (passed through)")
            continue

        t0 = time.perf_counter()
        en = translate(prompt).strip()
        ms = (time.perf_counter() - t0) * 1000
        total_ms += ms
        # A meta-response ("please provide the cue...") instead of a translation
        # is always much longer than the source line; keep the Japanese then
        if len(en) > max(80, 5 * len(cue["text"])):
            print(f"[{cue['start']}] {ms:5.0f} ms  rejected response {en[:60]!r}...")
            en = cue["text"]
        history.append((cue["text"], en))
        translations.append(en)
        print(f"[{cue['start']}] {ms:5.0f} ms  {cue['text']!r} -> {en!r}")

    with open(out, "w", encoding="utf-8") as f:
        for i, (cue, en) in enumerate(zip(cues, translations), 1):
            f.write(f"{i}\n{cue['start']} --> {cue['end']}\n{en}\n\n")
    n_translated = len(history)
    print(f"\n{len(cues)} cues ({n_translated} translated), "
          f"avg {total_ms / max(1, n_translated):.0f} ms/cue -> {out}")


if __name__ == "__main__":
    main()
