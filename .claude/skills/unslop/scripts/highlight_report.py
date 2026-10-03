#!/usr/bin/env python3
"""Write syntax highlighting into an HTML report as static markup.

Many viewers do not run JavaScript (Quick Look, mobile file viewers, chat
previews), so a report must not highlight code at view time. This script
rewrites each <pre data-lang="..."><code>...</code></pre> block in place into
numbered lines with highlight spans. The template CSS styles the spans.

Usage: highlight_report.py <page.html>
Exit:  0 success, 2 usage or input error.

Block attributes:
  data-lang   c, objc, metal, slang, hlsl, glsl, cpp, json, sh, bash, zsh
  data-start  first source line number (default 1)
  data-mark   lines to mark, for example "29,31-33"

A processed block gets data-highlighted, so a second run leaves it unchanged.
"""

import html
import re
import sys

C_KEYWORDS = (
    "if else for while do switch case default break continue return goto "
    "struct enum union typedef static const volatile extern inline sizeof void "
    "true false NULL nullptr kernel vertex fragment device constant thread "
    "threadgroup using namespace template auto register restrict "
    "_Static_assert static_assert in out inout uniform cbuffer import module"
).split()

RULES = {
    "c": [
        ("comment", r"//[^\n]*|/\*[\s\S]*?\*/"),
        ("string", r'"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\''),
        ("pre", r"#[ \t]*(?:include|define|undef|if|ifdef|ifndef|elif|else|endif|pragma|error)\b[^\n]*"),
        ("number", r"\b(?:0[xX][\da-fA-F]+|\d+\.?\d*(?:[eE][+-]?\d+)?)[uUlLfFhH]*\b"),
        ("keyword", r"\b(?:" + "|".join(C_KEYWORDS) + r")\b"),
        ("type", r"\b(?:[A-Z][A-Za-z0-9]*[a-z][A-Za-z0-9]*|\w+_t|bool|char|short|int|long|float|double|unsigned|signed|half|u?int[234]|float[234](?:x[234])?|half[234])\b"),
        ("macro", r"\b[A-Z][A-Z0-9_]{2,}\b"),
        ("function", r"\b[a-z_]\w*(?=\s*\()"),
    ],
    "json": [
        ("type", r'"(?:[^"\\]|\\.)*"(?=\s*:)'),
        ("string", r'"(?:[^"\\]|\\.)*"'),
        ("number", r"-?\b\d+\.?\d*(?:[eE][+-]?\d+)?\b"),
        ("keyword", r"\b(?:true|false|null)\b"),
    ],
    "sh": [
        ("comment", r"(?:(?<=\s)|^)#[^\n]*"),
        ("string", r'"(?:[^"\\]|\\.)*"|\'[^\']*\''),
        ("macro", r"\$\{?\w+\}?"),
        ("keyword", r"(?:(?<=\s)|^)--?[A-Za-z][\w-]*"),
    ],
}

ALIASES = {
    "objc": "c",
    "metal": "c",
    "slang": "c",
    "hlsl": "c",
    "glsl": "c",
    "cpp": "c",
    "bash": "sh",
    "zsh": "sh",
}

COMPILED = {
    lang: [(kind, re.compile(pattern, re.MULTILINE)) for kind, pattern in rules]
    for lang, rules in RULES.items()
}

WORD = re.compile(r"\w+|\W")

BLOCK = re.compile(
    r"<pre(?P<attrs>[^>]*\bdata-lang=\"[^\"]+\"[^>]*)>\s*<code>(?P<body>.*?)</code>\s*</pre>",
    re.DOTALL,
)


def tokenize(text, rules):
    tokens = []
    plain = []
    pos = 0
    while pos < len(text):
        hit = None
        for kind, pattern in rules:
            match = pattern.match(text, pos)
            if match and match.end() > pos:
                hit = (kind, match.group(0))
                break

        if hit:
            if plain:
                tokens.append((None, "".join(plain)))
                plain = []
            tokens.append(hit)
            pos += len(hit[1])
        else:
            word = WORD.match(text, pos).group(0)
            plain.append(word)
            pos += len(word)

    if plain:
        tokens.append((None, "".join(plain)))
    return tokens


def parse_marks(spec):
    marks = set()
    for part in spec.split(","):
        bounds = part.strip().split("-")
        if not bounds[0].isdigit():
            continue
        low = int(bounds[0])
        high = int(bounds[1]) if len(bounds) > 1 and bounds[1].isdigit() else low
        marks.update(range(low, high + 1))
    return marks


def attribute(attrs, name):
    match = re.search(r"\b" + name + r"=\"([^\"]*)\"", attrs)
    return match.group(1) if match else ""


def highlight_block(match):
    attrs = match.group("attrs")
    if "data-highlighted" in attrs:
        return match.group(0)

    lang = attribute(attrs, "data-lang")
    rules = COMPILED.get(ALIASES.get(lang, lang))
    text = html.unescape(match.group("body")).rstrip("\n")
    tokens = tokenize(text, rules) if rules else [(None, text)]

    lines = [""]
    for kind, value in tokens:
        for index, piece in enumerate(value.split("\n")):
            if index > 0:
                lines.append("")
            if not piece:
                continue
            escaped = html.escape(piece, quote=False)
            lines[-1] += f'<span class="sx-{kind}">{escaped}</span>' if kind else escaped

    start_text = attribute(attrs, "data-start")
    start = int(start_text) if start_text.isdigit() else 1
    marks = parse_marks(attribute(attrs, "data-mark"))

    rendered = []
    for index, line in enumerate(lines):
        number = start + index
        mark = " mark" if number in marks else ""
        rendered.append(f'<span class="ln{mark}" data-n="{number}">{line or " "}</span>')

    return f"<pre{attrs} data-highlighted><code>{''.join(rendered)}</code></pre>"


def main():
    if len(sys.argv) != 2:
        print("usage: highlight_report.py <page.html>", file=sys.stderr)
        return 2

    path = sys.argv[1]
    try:
        with open(path, encoding="utf-8") as file:
            page = file.read()
    except OSError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    blocks = len(BLOCK.findall(page))
    page = BLOCK.sub(highlight_block, page)

    with open(path, "w", encoding="utf-8") as file:
        file.write(page)

    print(f"highlighted {blocks} code block(s) in {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
