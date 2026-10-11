#!/usr/bin/env python3
"""Generate the macOS fork status site: engine channel badges, test count, HLE coverage map."""
import argparse
import json
import re
import sys
import urllib.request
from html import escape
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nid_names
import progress

ROOT = Path(__file__).resolve().parent.parent
CPU_SRC = ROOT / "core" / "cpu" / "src"
CHANNELS = ("alpha", "beta", "stable")
FEED_URL = "https://github.com/RINNECODER/AnyPS5/releases/download/channels/engine-{}.json"
REPO = "https://github.com/RINNECODER/AnyPS5"
COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)
LITERAL = re.compile(r'"([A-Za-z0-9_+\-]+)"')
UPSTREAM_EXPORT = re.compile(r"\bANYPS5_UPSTREAM_EXPORT\(\s*(\w+)")
PLAIN_COLORS = ((90, "#4c1"), (60, "#97ca00"), (30, "#dfb317"), (0, "#fe7d37"))


def _named_set(text, name, seen):
    """Resolve `NAME = A | B | set(\'\'\'...\'\'\'.split())` definitions without executing the module."""
    if name in seen:
        return set()
    seen.add(name)
    blocks = []

    def stash(match):
        blocks.append(match.group(1))
        return f"@{len(blocks) - 1}@"

    flat = re.sub(r"\'\'\'(.*?)\'\'\'", stash, text, flags=re.S)
    match = re.search(rf"^{name}\s*=\s*(.*)$", flat, re.M)
    if not match:
        return set()
    expr = match.group(1)
    names = set()
    for index in re.findall(r"@(\d+)@", expr):
        names |= set(blocks[int(index)].split())
    for ref in re.findall(r"\b([A-Z][A-Z_]+)\b", expr):
        names |= _named_set(text, ref, seen)
    return names


def parse_native_tests(source=None):
    text = (source or ROOT / "tools" / "prepare_diagnostic_engine.py").read_text(encoding="utf-8")
    return _named_set(text, "NATIVE_REQUIRED_TESTS", set())


def native_test_count(source=None):
    return len(parse_native_tests(source))


def served_tokens(src=CPU_SRC):
    """Every name or NID a macOS provider registers: quoted literals and upstream-bridge export rows."""
    tokens = set()
    for path in sorted(src.rglob("*")):
        if path.suffix not in (".cpp", ".mm", ".hpp"):
            continue
        text = COMMENT.sub("", path.read_text(errors="ignore"))
        tokens.update(LITERAL.findall(text))
        tokens.update(UPSTREAM_EXPORT.findall(text))
    return tokens


def coverage(libraries, tokens):
    """Re-score progress.py library groups by macOS provider presence (name or computed NID)."""
    groups = []
    for group in libraries["groups"]:
        names = group["done_names"] + group["todo_names"]
        served = sum(1 for n in names if n in tokens or nid_names.compute_nid(n) in tokens)
        groups.append({"name": group["name"], "label": group["label"], "done": served, "todo": len(names) - served})
    return progress.summarize(groups)


def feed_summary(channel, feed_dir=None):
    try:
        if feed_dir:
            raw = (Path(feed_dir) / f"engine-{channel}.json").read_text(encoding="utf-8")
        else:
            with urllib.request.urlopen(FEED_URL.format(channel), timeout=30) as response:
                raw = response.read().decode()
        release = json.loads(raw)["release"]
        return {"version": release["version"], "date": release["built_at"][:10], "commit": release["commit"][:9]}
    except Exception:
        return None


def pill(label, value, color):
    left, right = 10 + 7 * len(label), 10 + 7 * len(value)
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{left + right}" height="20" role="img" '
            f'aria-label="{escape(label)}: {escape(value)}">'
            f'<rect width="{left}" height="20" fill="#555"/><rect x="{left}" width="{right}" height="20" fill="{color}"/>'
            f'<g fill="#fff" font-family="Verdana,DejaVu Sans,sans-serif" font-size="11" text-anchor="middle">'
            f'<text x="{left / 2}" y="14">{escape(label)}</text><text x="{left + right / 2}" y="14">{escape(value)}</text></g></svg>')


def percent_color(percent):
    return next(color for floor, color in PLAIN_COLORS if percent >= floor)


def coverage_map(data):
    width, height = progress.PANEL_WIDTH, progress.HEADER + progress.MAP_HEIGHT
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" height="{height}">',
             progress.rect(0, 0, width, height, progress.BORDER, 0)]
    parts += progress.treemap("macOS HLE coverage", data, 0)
    parts.append("</svg>")
    return "\n".join(parts)


def page(feeds, tests, data):
    rows = "".join(
        f"<tr><td>{c}</td><td>{escape(f['version']) if f else 'none'}</td><td>{f['date'] if f else ''}</td></tr>"
        for c, f in feeds.items())
    lib_rows = "".join(
        f"<tr><td>{escape(g['name'])}</td><td>{g['done']}</td><td>{g['done'] + g['todo']}</td>"
        f"<td>{100 * g['done'] // (g['done'] + g['todo'])}%</td></tr>"
        for g in sorted(data["groups"], key=lambda g: g["name"].lower()) if g["done"] + g["todo"])
    imgs = " ".join(f'<img src="badge-engine-{c}.svg" alt="engine {c}">' for c in CHANNELS)
    return f"""<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>AnyPS5 for macOS status</title>
<style>body{{font-family:sans-serif;max-width:1000px;margin:auto;padding:16px}}img{{max-width:100%}}
table{{border-collapse:collapse}}th,td{{border:1px solid #ccc;padding:2px 8px}}</style></head><body>
<h1>AnyPS5 for macOS status</h1>
<p>{imgs} <img src="badge-macos-tests.svg" alt="macOS tests"> <img src="badge-macos-hle.svg" alt="macOS HLE"></p>
<h2>Engine channels</h2><table><tr><th>Channel</th><th>Version</th><th>Built</th></tr>{rows}</table>
<p>Read from the channel feeds on the <a href="{REPO}/releases/tag/channels">channels release</a>.</p>
<h2>macOS HLE coverage</h2><img src="macos-coverage.svg" alt="macOS HLE coverage map">
<p>{tests} native required tests (<code>NATIVE_REQUIRED_TESTS</code> in <code>tools/prepare_diagnostic_engine.py</code>).</p>
<table><tr><th>Library</th><th>Served on macOS</th><th>Declared</th><th>%</th></tr>{lib_rows}
<tr><th>Total</th><th>{data['done']}</th><th>{data['total']}</th><th>{data['percent']}%</th></tr></table>
<p><b>Method.</b> The denominator is every function declared in <a href="{REPO}/tree/main/core/libs/prx">core/libs/prx</a>
(same parsing as <code>tools/progress.py</code>). A function counts as served when its name or its computed NID appears as a
string literal, or as an <code>ANYPS5_UPSTREAM_EXPORT</code> row, in the macOS providers under
<a href="{REPO}/tree/main/core/cpu/src">core/cpu/src</a> (comments stripped). This is a static scan: it shows a provider
is registered, not that it is complete, and it misses providers that derive names at runtime.</p>
<p>Generated by <a href="{REPO}/blob/main/tools/macos_status.py">tools/macos_status.py</a> on every push to <code>main</code>.</p>
</body></html>
"""


def generate(out, feed_dir=None):
    out.mkdir(parents=True, exist_ok=True)
    feeds = {c: feed_summary(c, feed_dir) for c in CHANNELS}
    badges = {}
    for channel, feed in feeds.items():
        value = f"{feed['version']} ({feed['date']})" if feed else "none"
        badges[f"badge-engine-{channel}.svg"] = pill(f"engine {channel}", value, "#2ea043" if feed else "#9f9f9f")
    tests = native_test_count()
    badges["badge-macos-tests.svg"] = pill("macOS tests", str(tests), "#2ea043")
    data = coverage(progress.collect_libraries(), served_tokens())
    badges["badge-macos-hle.svg"] = pill("macOS HLE", f"{data['percent']}%", percent_color(data["percent"]))
    for name, svg in badges.items():
        (out / name).write_text(svg, encoding="utf-8")
    (out / "macos-coverage.svg").write_text(coverage_map(data), encoding="utf-8")
    (out / "index.html").write_text(page(feeds, tests, data), encoding="utf-8")
    return feeds, tests, data


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--feed-dir", type=Path, help="read engine-<channel>.json from here instead of GitHub")
    args = parser.parse_args()
    feeds, tests, data = generate(args.output, args.feed_dir)
    for channel, feed in feeds.items():
        print(channel, feed["version"] + " " + feed["date"] if feed else "none")
    print("tests", tests, "hle", f"{data['done']}/{data['total']} {data['percent']}%")
