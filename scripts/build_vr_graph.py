#!/usr/bin/env python3
"""
build_vr_graph.py — generate docs/vr_graph/index.html from a template plus a
curated data file, so the committed artifact stays self-contained and
double-clickable (no runtime fetch, no npm, no backend, no network).

Workflow:
    edit   docs/vr_graph/graph_data.json
    run    python3 scripts/build_vr_graph.py
    open   docs/vr_graph/index.html
    commit graph_data.json + template.html + the generated index.html

VR_LIFECYCLE_GRAPH_V1.md remains the human source of truth; graph_data.json
is its curated machine projection. This script does NOT parse the markdown
(deliberately, to avoid fragile parsing in v0); it embeds the JSON and
validates a set of invariants that must hold for the artifact.
"""
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
GRAPH_DIR = ROOT / "docs" / "vr_graph"
TEMPLATE = GRAPH_DIR / "template.html"
DATA = GRAPH_DIR / "graph_data.json"
OUTPUT = GRAPH_DIR / "index.html"

PLACEHOLDER = "/*__GRAPH_DATA__*/"


def fail(msg):
    print(f"build_vr_graph: FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def build():
    if not TEMPLATE.exists():
        fail(f"missing template: {TEMPLATE}")
    if not DATA.exists():
        fail(f"missing data: {DATA}")

    template = TEMPLATE.read_text(encoding="utf-8")
    raw = DATA.read_text(encoding="utf-8")

    if template.count(PLACEHOLDER) != 1:
        fail(f"template must contain exactly one {PLACEHOLDER}")

    try:
        data = json.loads(raw)
    except json.JSONDecodeError as e:
        fail(f"graph_data.json is not valid JSON: {e}")

    # Re-serialize canonically so the embedded block is stable and the build
    # is byte-reproducible from (template.html, graph_data.json).
    embedded = "\n" + json.dumps(data, ensure_ascii=False, indent=2) + "\n"
    html = template.replace(PLACEHOLDER, embedded)

    validate(html, data)

    OUTPUT.write_text(html, encoding="utf-8")
    print(f"build_vr_graph: wrote {OUTPUT} "
          f"({len(data['nodes'])} nodes, {len(data['edges'])} edges, "
          f"{len(data['groups'])} groups)")


def validate(html, data):
    node_ids = {n["id"] for n in data["nodes"]}
    edge_ids = {e["id"] for e in data["edges"]}
    edge_by_id = {e["id"]: e for e in data["edges"]}
    problems = []

    # required content
    for i in range(1, 27):
        if f"E{i}" not in edge_ids:
            problems.append(f"missing edge E{i}")
    for i in range(1, 8):
        if f"UB{i}" not in node_ids:
            problems.append(f"missing node UB{i}")
    for nid in ("A1", "FREEZE"):
        if nid not in node_ids:
            problems.append(f"missing node {nid}")

    # status invariants
    e25 = edge_by_id.get("E25")
    if not e25:
        problems.append("E25 edge missing")
    elif e25.get("status") != "deferred":
        problems.append(f"E25 status must be deferred, got {e25.get('status')}")

    blocker_count = data.get("meta", {}).get("blockerCount")
    if blocker_count != 0:
        problems.append(f"meta.blockerCount must be 0, got {blocker_count}")
    live_blockers = [x["id"] for x in data["nodes"] + data["edges"]
                     if x.get("status") == "blocker"]
    if live_blockers:
        problems.append(f"blocker-status elements present: {live_blockers}")

    # stale-wording guards (must not reappear)
    if "production policy TBD" in html:
        problems.append("stale wording present: 'production policy TBD'")
    if "full green requires M3" in html.lower():
        problems.append("stale wording present: 'full green requires M3'")

    # self-contained / no-fullscreen guards
    if "requestFullscreen" in html or "webkitRequestFullscreen" in html:
        problems.append("requestFullscreen call present (must be page-local)")
    if re.search(r'(src|href)="https?://', html):
        problems.append("external network reference present")
    if "graph_data.json" in html.replace(PLACEHOLDER, ""):
        # the generated artifact must not fetch the data file at runtime
        if re.search(r'fetch\(\s*["\']?[^)]*graph_data\.json', html):
            problems.append("runtime fetch(graph_data.json) present")

    if problems:
        for p in problems:
            print(f"build_vr_graph: INVALID: {p}", file=sys.stderr)
        fail(f"{len(problems)} invariant(s) failed")
    print("build_vr_graph: invariants OK "
          "(E1-E26, UB1-UB7, A1, FREEZE, E25=deferred, blockers=0, "
          "no stale wording, no fullscreen, offline)")


if __name__ == "__main__":
    build()
