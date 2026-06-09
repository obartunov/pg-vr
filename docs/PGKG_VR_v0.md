# PGKG-VR-v0 — Value Representation lifecycle source graph

Baseline: `origin/value-representation-v0` after F2(a) (`7c8d6df`, `b9f5e91`, `4d783f3`).
Revision: v0 + hacker/architect pass. Adds: decode-guard recolor; gate at the
construction primitive; WAL made explicit with **both** consumers (redo vs decode);
crash recovery + physical replication path (N20); REPACK CONCURRENTLY edge;
VR-intrinsic vs TOAST-v0-mechanism layering; ownership named; INMEM reserved.

Purpose: one grounded map of the VR lifecycle so design/patch/verdict work answers
from the graph instead of re-reading the same files. Every node carries a source
anchor verified against this tree.

Two classification channels, kept separate on purpose:
- **Node fill = layer.** `VR` = intrinsic VR contract (durable across a storage
  swap). `v0` = TOAST-backed body store (the replaceable v0 mechanism; changes
  under F2(b)). `core` = PostgreSQL infrastructure VR rides on (rewrite, WAL, redo,
  decode). `gate?` = construction gate (N18, now implemented in vr-brepl-gate-v0).
- **Edge label = safety.** **green** safe/enforced; **yellow** correct only while a
  named invariant holds, or a known cost/untested path; **red** a boundary that, if
  crossed, causes corruption / orphan / slot wedge.

Scope limit: this is a lifecycle-**structure** graph, not a concurrency/interleaving
graph. It does not model races, lock order, or snapshot interleaving (see the REPACK
CONCURRENTLY edge, deliberately marked untested).

---

## Layered flow

```mermaid
flowchart TB
  classDef vr fill:#e7f5e7,stroke:#2e7d32,color:#1b3d1b;
  classDef v0 fill:#e6f0fb,stroke:#1f5fa8,color:#10314f;
  classDef core fill:#f0f0f0,stroke:#555,color:#222;
  classDef gate fill:#fdecea,stroke:#c0392b,color:#5a1a14,stroke-dasharray:5 4;

  subgraph POLICY[Policy]
    N4["N4 selector hook (which values -> VR)\ntoast_helper.c:368-396"]:::vr
  end

  subgraph CONS[Construction - mechanism]
    N5["N5 vr_make_save_body / vr_toast_body_save\nvr_toast.c:141,254 (sole primitive, exported)"]:::vr
    N18["N18 GATE: RelationIsLogicallyLogged (implemented)\nin vr_make_save_body; rel.h:721"]:::gate
  end

  subgraph DESC[Descriptor and identity - intrinsic]
    N1["N1 varatt_vr\nvaratt.h:61"]:::vr
    N2["N2 VrKind + methods vtable\nvalue_representation.h:49-56"]:::vr
    N3["N3 locator = addressing identity (not value identity)\nvr_toast.c:76-112"]:::vr
  end

  subgraph STOR[TOAST-backed body store - v0, replaceable]
    N10["N10 toast_save_datum\ntoast_internals.c:120-303"]:::v0
    N11["N11 toastid_valueid_exists\ntoast_internals.c (exported)"]:::v0
    N12["N12 home-OID safety net (I1)\nheaptoast.c:178-198"]:::v0
  end

  subgraph READ[Read / flatten - intrinsic]
    N6["N6 vr_build_source_external (read-only)\nvr_toast.c:287-318"]:::vr
    N7["N7 vr_body_read\nvr_toast.c:455-489"]:::vr
    N8["N8 vr_detoast_flatten\ndetoast.c:61"]:::vr
  end

  subgraph REW[Rewrite / lifecycle]
    N13["N13 rewrite drivers VF/CLUSTER/REPACK[/CONCURRENTLY]\nrepack.c:1370,266-349; rewriteheap.c:619-630"]:::core
    N9["N9 vr_toast_body_copy_to_relation (copy-not-move)\nvr_toast.c:347"]:::vr
  end

  subgraph GC[GC / delete - intrinsic]
    N14["N14 toast_delete_datum VR dispatch (I5)\ntoast_internals.c:489-508"]:::vr
    N15["N15 vr_toast_same_body + UPDATE path\nvr_toast.c:102; toast_helper.c:95-118"]:::vr
  end

  subgraph WALC[WAL and its two consumers]
    N19["N19 WAL stream (carries VR datum)\nno VR-specific WAL records"]:::core
    N20["N20 redo: crash recovery + physical standby replay\nheap/toast redo; no VR-specific redo"]:::core
    N16["N16 reorderbuffer refusal (backstop)\nreorderbuffer.c:5172-5175"]:::core
    N17["N17 pgoutput refusal (backstop)\nproto.c:834-837"]:::core
  end

  N4 -->|green: opts in| N5
  N18 ==>|red: refuse if logically logged| N5
  N5 -->|green: store body| N10
  N5 -->|green: enforce I2 logical-size| N1
  N5 -->|green: emit WAL| N19
  N10 --> N3
  N13 -->|green: per tuple| N9
  N13 -.->|yellow: CONCURRENTLY path untested for VR| N9
  N13 -->|green: emit WAL| N19
  N9 -->|green: reuse valueid + verbatim| N10
  N9 -->|green: dedup probe| N11
  N9 -.->|yellow: must name home, I1| N12
  N5 -.->|yellow: must name home, I1| N12
  N6 -->|green: reject bad flags/method first| N3
  N6 --> N7
  N7 -.->|yellow: O(full body) per call, P2| N8
  N14 -->|green: reclaim by locator| N3
  N15 -->|green: share on UPDATE-unchanged| N3
  N1 -->|carries| N3
  N2 -->|selects impl| N5
  N19 -->|green: redo replays heap+toast| N20
  N20 -->|green: same read-only path on standby / after recovery| N6
  N19 -.->|red: any slot decoding this WAL| N16
  N16 --> N17
```

WAL has two consumers and VR behaves oppositely on each:
- **redo (N19->N20): green.** Crash recovery and physical standby replay reconstruct
  heap + TOAST verbatim; the value is identical afterwards.
- **logical decode (N19=>N16): red.** Any slot decoding the same WAL hits the
  backstop ERROR -> `confirmed_flush` stalls (wedge, B-repl).

The B-repl gate must sit at N5: once N19 holds the datum it cannot be un-written, so
pre-existing VR (stored before the relation became logically logged) is reachable
only by the N16 backstop, not the construction gate.

---

## Durability and replication

VR introduces **no WAL record types and no redo routines** (verified: none in
`vr_toast.c` / `vr.c`) and keeps **no state outside the WAL-logged heap and TOAST**
(no VR catalog; the locator lives in the `varatt_vr` descriptor inside the heap
tuple; the body is ordinary TOAST chunks). Consequences:

- **Crash recovery (N19->N20): inherited, green, correct by construction.** Redo of
  heap + TOAST replays the descriptor bytes and body chunks verbatim. Tested scope:
  insert and VACUUM FULL; a full crash-injection matrix has not been run.
- **Physical / streaming replication (N20 on a standby): green.** The standby
  replays the same WAL; the read path (N6/N7/N8) is read-only and runs unchanged in
  recovery, so the column reads correctly on the standby. **I1 holds on a standby**
  because physical replication preserves OIDs/relfilenodes, so the locator still
  names the correct TOAST home; failover preserves VR. Reasoned from source; not
  exercised with a live standby in-sandbox.
- **Logical decoding / logical replication (N19=>N16/N17): refused, red.** Blocked at
  the source (publisher decode) before anything reaches a subscriber.

Net: **VR is durable and physically replicable; only the logical-decoding consumer
of WAL is the boundary.** That is exactly the B-repl scope — stored where *logical*
decoding cannot represent it, not replication in general.

---

## Node catalog

| id | layer | entity | source | role / obligation |
|----|-------|--------|--------|-------------------|
| N1 | VR | `varatt_vr` | `varatt.h:61` | kind, version, flags(=physical-stream compression), `vr_logical_size`(incl VARHDRSZ), `vr_body_size`(=PHYSICAL saved size, possibly compressed; B6 comment fixed). `VARTAG_VR`=19; `VARTAG_VR_INMEM`=4 **reserved, no producer on current path**. |
| N2 | VR | VrKind + methods vtable | `value_representation.h:49-56` | the VR contract surface (make/read/...); append-only enum; `vr_lookup_methods` O(1). |
| N3 | VR | locator (storage_oid, valueid) | `vr_toast.c:76-112` | **addressing** identity, relation-local; `storage_oid` re-homed + `valueid` reused on rewrite. NOT value identity: no content-addressing (B2). |
| N4 | VR | selector hook (**policy**) | `toast_helper.c:368-396` | which values become VR; may decline. Sole dispatch of `m->make` (`:384`). Never trusted to self-enforce the WAL boundary. |
| N5 | VR | `vr_make_save_body` (**mechanism, sole primitive**) | `vr_toast.c:141,254` | only persistent-VR construction primitive; has `rel`; **exported** -> direct callers bypass N4, so the gate belongs here. I2 logical-size hard-error before any write. |
| N6 | VR | `vr_build_source_external` (read-only) | `vr_toast.c:287-318` | I3: reject unknown flags/method before locator/body access. Runs in recovery / on standby. |
| N7 | VR | `vr_body_read` | `vr_toast.c:455-489` | P2: full `detoast_attr` per call -> O(full body). |
| N8 | VR | `vr_detoast_flatten` | `detoast.c:61` | reader; rejects unknown flag bits. |
| N9 | VR | `vr_toast_body_copy_to_relation` | `vr_toast.c:347` | physical-verbatim rehome; routes directly through `toast_save_datum` (`vr_toast.c:421`), not N5; valueid reuse + dup fast-path (F2(a)); copy-not-move (I6). |
| N10 | v0 | `toast_save_datum` | `toast_internals.c:120-303` | body store; `Assert(!external)`(:136); rewrite valueid reuse(:259-262)/short-circuit(:281-286). |
| N11 | v0 | `toastid_valueid_exists` | `toast_internals.c` (exported, internal header) | duplicate-detection predicate. |
| N12 | v0 | home-OID safety net | `heaptoast.c:178-198` | I1: locator must name the relation's effective TOAST home. **v0-specific; gone with a value-owned store (F2(b)).** Holds on a standby (OIDs preserved). |
| N13 | core | rewrite drivers | `repack.c:1370,266-349`; `rewriteheap.c:619-630` | VF/CLUSTER/REPACK + **CONCURRENTLY** (`CLUOPT_CONCURRENT`). **Ownership root:** per-relation TOAST ownership destroyed at the relfilenode swap (B1) -> forces N9 copy. |
| N14 | VR | `toast_delete_datum` VR dispatch | `toast_internals.c:489-508` | I5: reclaim body by locator; speculative propagated (kill path not independently stress-tested). |
| N15 | VR | `vr_toast_same_body` + UPDATE | `vr_toast.c:102`; `toast_helper.c:95-118` | UPDATE-unchanged shares the locator; avoids orphan/double-free. |
| N16 | core | reorderbuffer refusal (**correct backstop**) | `reorderbuffer.c:5172-5175` | I7: VR in decode reassembly -> ERROR. |
| N17 | core | pgoutput refusal (**correct backstop**) | `proto.c:834-837` | I7: VR on the wire -> ERROR. |
| N18 | gate | `RelationIsLogicallyLogged` gate (**implemented**, vr-brepl-gate-v0) | in N5; `rel.h:721` | refuse persistent VR construction on logically-logged relations before any body write; covers N4 dispatch **and** direct API callers; does not touch N9 rewrite. ERROR "persistent value representation is not supported on logically logged relations". |
| N19 | core | WAL stream | `XLogLogicalInfoActive`; `rel.h:721` | carries the VR datum; **no VR-specific WAL records**. Two consumers: N20 (redo) and N16 (decode). |
| N20 | core | redo: crash recovery + physical standby replay | heap/toast redo | **no VR-specific redo**; replays heap+TOAST verbatim -> VR durable and physically replicable. |

## Edge catalog (safety)

| from -> to | kind | color | meaning |
|------------|------|-------|---------|
| N4 -> N5 | policy->mechanism | green | selector opts in; make() builds VR |
| N18 => N5 | gates | red | refuse if `RelationIsLogicallyLogged` (**implemented**; primitive-level; ERROR before body write) |
| N5 -> N10 | stores | green | body saved via TOAST (v0) |
| N5 -> N1 | enforces | green | I2 logical-size before any write |
| N5 -> N19, N13 -> N19 | emits | green | construction/rewrite writes WAL |
| N13 -> N9 | drives | green | per-tuple rehome (non-concurrent verified) |
| N13 => N9 (CONCURRENTLY) | drives | yellow | REPACK CONCURRENTLY path **untested for VR** |
| N9 -> N10 | reuses | green | valueid reuse + short-circuit (F2(a) closed B0) |
| N9 -> N11 | probes | green | dup fast-path |
| N9 -> N12, N5 -> N12 | must-satisfy | yellow | correct only while I1 holds |
| N6 -> N3 | guards | green | I3 reject bad flags/method before locator |
| N7 -> N8 | reads | yellow | P2 full-body detoast per call |
| N14 -> N3, N15 -> N3 | reclaim/share | green | I5 GC + UPDATE-unchanged sharing |
| N19 -> N20 | redo | green | crash recovery + physical standby replay heap+TOAST |
| N20 -> N6 | reads | green | same read-only path on standby / after recovery |
| N19 => N16 | decoded | red | any slot decoding WAL with a VR datum -> wedge (B-repl) |

---

## Invariant index

| inv | statement | nodes | layer note |
|-----|-----------|-------|------------|
| I1 | locator names the relation's effective TOAST home | N12; deps N5,N9 | **v0-specific**; holds on standby |
| I2 | logical_size == VARHDRSZ + body, enforced before write | N5 | intrinsic |
| I3 | reject unknown flags/method before locator/body access | N6, N8 | intrinsic |
| I4 | reads bounded by logical `[0, vr_body_size())` | N7 | intrinsic |
| I5 | body reclaimed with its TOAST owner; speculative propagated | N14, N15 | intrinsic (via v0 today) |
| I6 | copy-not-move on rewrite; valueid-stable, dedup-correct (F2(a)) | N9, N13 | **v0 consequence of B1** |
| I7 | VR refused in logical decoding / replication | N16, N17, N19 | core boundary |
| I8 | transient VR never persisted (vacuous: no producer) | N1 | intrinsic, reserved |
| I9 | no VR-specific WAL/redo and no out-of-band state -> durability and physical replication inherited | N19, N20 | core boundary |

## Status / blocker index

| item | state | where in graph |
|------|-------|----------------|
| B0/F1 multi-version rewrite orphan duplication | **RESOLVED** (F2(a)) | N9->N10, N9->N11 |
| crash recovery / physical replication | **green** — inherited by construction (I9); reasoned + insert/VF tested | N19->N20, N20->N6 |
| B-repl logical-replication slot wedge | **CLOSED for new construction** (gate at N5, vr-brepl-gate-v0); residual pre-existing-value hazard under N16/N17 backstop | chain N5->N19=>N16; gate N18=>N5 |
| REPACK CONCURRENTLY x VR | **OPEN — untested** | yellow N13=>N9 |
| direct-API bypass of construction gate | **closed by gate placement** | N18 at N5 |
| B6 `vr_body_size` doc/code drift | **FIXED** (varatt.h comment now says saved physical, possibly compressed) | N1 |
| P2 windowed read O(full body) | open scalability trap | N7 |
| B1 per-relation TOAST ownership destroyed at swap | open; ownership root, gates F2(b) | N13, N12 |
| B2 GC for shared/value identity (no content-addressing) | open, gates F2(b) | N3, N14 |
| F2(b) true no-copy / value-owned store | **NOT started** | replaces the `v0` layer (N10/N11/N12) + N9 semantics |

---

## How to use (PGKG-first recipe)

For any VR design / patch / verdict, answer from this graph first:
1. which node(s) the change touches and their layer (VR-intrinsic vs v0-mechanism vs core);
2. their obligations (invariant column);
3. unsafe edges in scope (red/yellow) and whether the change crosses them;
4. which obligations are v0-storage-specific (drop with F2(b)) vs intrinsic to any VR;
5. for WAL-touching changes: which consumer (redo N20 / decode N16) is affected;
6. the source line that refines the graph if reality differs.

Maintenance: when a patch changes a node's behavior, update its row, edge color, and
the status index — so the next round reads the graph, not the files.
