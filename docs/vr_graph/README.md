# VR lifecycle graph (web)

A single-file, no-build, offline web view of the PostgreSQL Value
Representation (VR) lifecycle for this fork. It is the visual companion to
`VR_LIFECYCLE_GRAPH_V1.md` and `VR_V0_RELEASE_CANDIDATE_FREEZE.md`.

## What the graph is

Nodes are the mechanisms and value states of the VR line (construction gate,
representation policy, kinds, rewrite/rehome seam, capture seams, logical
output, dump/restore, pg_upgrade boundary, admin surface, the unsafe-boundary
holders, and the v0 release-freeze node). Edges are the lifecycle
transitions E1–E26 plus the freeze and unsafe-boundary-holder links.

It exists because the text graph is dense; this view lets you zoom into one
mechanism, see its transitions, filter by readiness, and read the source
files behind each edge without scrolling a long table.

## How to open it locally

Open `index.html` directly in any browser:

    xdg-open docs/vr_graph/index.html      # Linux
    open docs/vr_graph/index.html          # macOS

No server, no network, no npm. All graph data is embedded in the file in a
`<script id="graphData" type="application/json">` block and the renderer is
inline SVG + vanilla JS. It works offline and on mobile (pinch zoom,
one-finger pan).

`Graph only` hides the side panels to give the graph the full browser window;
it does NOT trigger browser fullscreen.

## Files

- `index.html` — the whole thing in one self-contained file, with a mode
  switch in the header:
  - **VR** — the full VR lifecycle graph (all mechanisms, E1–E26).
  - **jsonb** — a typed view projected onto the JSONB cold kind: selection /
    floor / policy, the cold body in home TOAST, flatten, the
    rewrite/capture/output lifecycle as it applies to jsonb, and the admin
    surface. It also shows the hot/warm/cold three-temperature physics as a
    SEPARATE design track (status `design`, dotted, purple) — that work
    lives in jbtl-public-clean / jsonb-p1-bench-suite, NOT in this fork's v0
    freeze, and is drawn for context only, never as accepted v0.
  - **overlay** — VR plus the jsonb nodes, each jsonb node linked to the VR
    node it specializes by a dashed `derives` edge. This is the inheritance
    view: jsonb cold is a specialization of the VR framework, not a separate
    thing. Every jsonb node also carries a `derivesFrom` shown in its
    tooltip and details panel (e.g. jsonb SEL derives from VR C1; jsonb FLAT
    from VR N8; jsonb K2/A1 ARE the VR K2/A1).
- `README.md` — this file.
- `graph_data.json` — the curated data (you edit this).
- `template.html` — the engine with a data placeholder.
- `../../scripts/build_vr_graph.py` — embeds the data into the template to
  produce `index.html`.

`index.html` is generated from `template.html` + `graph_data.json`; see
"How it is built" below.

(Earlier drafts shipped a separate jsonb.html; it was folded into this one
file for integrity — one engine, one dataset, no drift between pages.)

## Reading an edge

Each edge carries a midpoint dot you can hover or tap (easier to hit than a
thin line, especially on touch). The dot shows the count of source files
that implement that transition. Hovering shows the transition name, its
`from → to` (state/mechanism to state/mechanism — this is the real
relationship: edges connect lifecycle states, NOT file-to-file), and the
implementing source files on a monospace line. Clicking opens the details
panel.

## Groups (lifecycle phases)

The top-level groups are lifecycle phases, kept disjoint by substance:

- **Core representation** — what VR *is*: descriptor, kinds (K1/K2), policy
  API (P1), value states (LV/VR).
- **Write path / storage policy** — how a row *becomes* VR: the construction
  gate (C1), construct/refuse edges (E1/E2), and the selector on writes
  (E24).
- **Read path / flatten**, **Rewrite / rehome**, **Logical decode /
  replication**, **Dump / restore**, **Upgrade / format stability**,
  **Admin surface** — the remaining phases.
- **Unsafe boundaries** — the failure classes and their holders. WAL safety
  lives here explicitly as UB7 (decoded-WAL implies gated construction,
  held by C1); there is no separate WAL pipeline in the VR line. Crash
  recovery is not a separate group either: VR bodies are TOAST chunks
  recovered by the normal TOAST/WAL path, stated in UB3.
- **Release freeze**, **Deferred / future**.

There are deliberately no WAL or recovery phase-groups: VR adds no
WAL-specific or recovery-specific pipeline, so a phase-group there would be
an empty box. Both are represented honestly as invariants/boundaries.

## How statuses map to VR semantics

Status is readiness, colored:

- `pass` (green) — works / runtime-evidenced / accepted. This INCLUDES the
  by-design refusals (former green-c and the refused safety backstops): a
  refusal that is the correct behavior is a pass, shown green with a
  `REFUSED` badge.
- `eng_v0` / accepted-v0 (yellow) — implemented and accepted as an
  engineering v0 surface, not yet a finalized/core interface (E26).
- `partial` (orange) — works partially, with a named remaining constraint
  (E7 rewrite-flatten foreign home; E20 walsender/live-read enablement).
- `deferred` (dark amber/brown) — intentionally not in v0 (E25).
- `blocker` (red) — corruption / orphan / loss / rewrite-breakage. Current
  count is 0.

### Why refused-by-design is green/pass, not blocker

The VR line refuses several operations on purpose: construction under an
active logical-decoding window (C1), capture outside the REPACK seam (N16
refusal), live-read flatten inside decoding (E21), unsupported on-disk
version (version gate). These refusals are the safety mechanism working as
designed — they prevent corruption rather than being a defect. They are
therefore green/pass and carry a `REFUSED` badge so the refusal is visible.
A blocker is an OPEN unsafe path, which is a different thing; there are none.

## Edge colors vs edge status

Two independent visual channels:

- Edge COLOR = transition kind (construction/policy, read/flatten/guard,
  rewrite/rehome/cleanup, logical capture/replication, dump/restore,
  upgrade/format-stability, admin action/inspection, unsafe-boundary holder,
  release/freeze, deferred policy). Edges are deliberately not one color.
- Edge DASH = readiness: solid = runtime-evidenced/accepted; dashed =
  accepted-v0 / partial; dotted = deferred.

## How it is built (maintainability)

`index.html` is GENERATED, not hand-edited. The maintained inputs are:

- `graph_data.json` — the curated data (nodes, edges, groups, statuses,
  evidence, source files, tip tree, blocker count). This is what you edit.
- `template.html` — the engine (renderer, styles, interactions) with a
  single `/*__GRAPH_DATA__*/` placeholder inside the
  `<script id="graphData">` block.
- `scripts/build_vr_graph.py` — embeds `graph_data.json` into
  `template.html` and writes `index.html`, validating invariants.

The generated `index.html` is still fully self-contained: the data is
embedded at build time, so it opens directly from `file://` with no runtime
`fetch()`, no npm, no backend, no network.

Update workflow:

    1. edit   docs/vr_graph/graph_data.json
    2. run    python3 scripts/build_vr_graph.py
    3. open   docs/vr_graph/index.html   (double-click; verify visually)
    4. commit graph_data.json + template.html + the generated index.html

The build is byte-reproducible: the same (template.html, graph_data.json)
always produces the same index.html. The script refuses to write if any
invariant fails (E1-E26, UB1-UB7, A1, FREEZE present; E25 = deferred;
blocker count = 0; no stale "production policy TBD" / "full green requires
M3"; no `requestFullscreen`; no external network reference).

## How to add or update a NODE

Edit the `nodes` array in `graph_data.json` (NOT index.html). Node schema:

    {
      "id": "X1", "kind": "node", "label": "X1",
      "title": "<short id + name>",
      "status": "pass|eng_v0|partial|deferred|blocker",
      "badge": "REFUSED|ACCEPTED v0|DEFERRED| (optional)",
      "group": "<one of the group ids>",
      "description": "<short lifecycle meaning / invariant — do NOT repeat the title>",
      "files": ["path/to/source.c"],
      "evidence": ["TAP name (asserts)"]
    }

Keep the title to `id · name` and the body to the lifecycle meaning; do not
duplicate the title in the body. Then re-run the build.

## How to add or update an EDGE

Edit the `edges` array in `graph_data.json`. Edge schema:

    {
      "id": "E27", "from": "<nodeId>", "to": "<nodeId>", "label": "E27",
      "title": "<transition name>",
      "status": "pass|eng_v0|partial|deferred|blocker",
      "rawStatus": "green|green-c|eng|partial|deferred",
      "badge": "", "group": "<group id>",
      "description": "<what the transition does + evidence>",
      "files": ["path/to/source.c"],
      "evidence": ["TAP name (asserts)"],
      "style": "solid|dashed|dotted"
    }

Edge color is derived from the transition kind by `edgeKind()` in
`template.html`; if a new edge id needs a specific kind, add it to the id
lists in `edgeKind()` (a template change, then rebuild). Dash is derived
from `status` in `renderEdge()`. Then re-run the build.

## How to update after a new lifecycle revision

1. Update `VR_LIFECYCLE_GRAPH_V1.md` first — it is the source of truth.
2. Reflect only the changed nodes/edges in `graph_data.json`.
3. Update the `meta` block (revision, tipCommit, tipTree, blockerCount) if
   the release tip moved, and verify the tree hash against origin.
4. Run `python3 scripts/build_vr_graph.py` and open `index.html` to verify.

The graph is a projection of the markdown; when they disagree, the markdown
wins. The build does NOT parse the markdown (deliberately avoided in v0 to
keep parsing non-fragile); `graph_data.json` is the curated machine
projection you maintain by hand.

## Source docs used

- `VR_LIFECYCLE_GRAPH_V1.md` rev v1.11
- `VR_V0_RELEASE_CANDIDATE_FREEZE.md`
- origin/vr-storage-policy-incore-selector-v0 @ tip commit
  a98c70fdadf1c105b9594a5a97b2a72dc9bc1dee, tree
  cb16a8a032a0697ed85b7a27cc6921553a482634
