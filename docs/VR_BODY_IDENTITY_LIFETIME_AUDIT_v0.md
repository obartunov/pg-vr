# VR Body Identity / Lifetime Design Audit v0

Baseline: `origin/value-representation-v0 @ b61ebe28e7`.
This is an architecture/evidence note. It contains no implementation, no
descriptor format change, and no GC design. It records what is true today,
what would be required to move from physical-verbatim rehome to body-identity
preservation / no-copy rewrite, and a recommendation.

Superseded and excluded as an implementation source: branch
`vr-construction-recovery-v0 @ 6f7ff17` (commits `d83ca43`, `0faf619`,
`6f7ff17`) and its stale stash. Its concerns were reworked into accepted
`d2bac64` (reject unsafe storage movement) and `efc119b` (physical-verbatim
rehome). It is marked **superseded**; it is not deleted.

Non-goals (out of scope for this note and for v1, per gate): dedup,
content-addressing, dictionary, partial read, delta/COW, refcount
implementation, decoupled body store, logical-decoding support, any descriptor
format change.

---

## 1. Backward — lessons from the failure classes

**Body ownership must be explicit and checked at store time, never implied by a pointer copy.**
The accepted line makes ownership a checked invariant: at store, a persistent
VR whose locator does not name this relation's effective TOAST home is refused
(`heaptoast.c:178-198`). A raw pointer copy that "looks fine" is exactly the
hidden-ownership failure; the safety net converts it into an error instead of a
dangling locator. Boundary: the check is per attribute, at every heap store.
Cost: one descriptor read per external VR attribute. Conclusion: ownership is a
store-time obligation, not a property of having a pointer.

**An orphan/dangling body is prevented by tying reclamation to exactly one owner per generation.**
A VR body is reclaimed through the ordinary TOAST delete path
(`toast_delete_datum -> vr_toast_body_delete`, `toast_internals.c:489-508`) at
the value-changing or value-removing operation. The only sharing today is an
UPDATE whose VR value is unchanged: the new tuple version reuses the same
locator and the old body is kept (`toast_helper.c:95-118`, `vr_toast_same_body`
`vr_toast.c:102-112`). That is a single linear version chain with
last-reference reclamation, not true multi-owner sharing. Conclusion:
generalized sharing has no reclaiming rule today and would orphan or
double-free without one.

**The relfilenode swap destroys per-relation TOAST ownership; anything that must survive rewrite cannot live in per-relation TOAST.**
During rewrite the body is written into the new relation's physical TOAST and
the locator is stamped with `rd_toastoid`, the pre-swap surviving OID
(`heaptoast.c:171-176`); the old relfilenode and its TOAST are dropped at the
swap, which is what reclaims the old bodies (`vr_toast.c:337-341`,
`toast_helper.c:164-166`). Conclusion: no-copy rewrite is impossible while body
lifetime is bound to the relfilenode being replaced.

**Identity must be defined on logical/contract terms, never on the physical byte stream or its size.**
The descriptor carries the logical size including header (`vr_logical_size`)
and, in `vr_body_size`, the *saved physical* (possibly compressed) stream size
— `VARATT_EXTERNAL_GET_EXTSIZE(ve)` (`vr_toast.c:128-129,229,414`). The reader
reconstructs the external pointer with the recorded compression method and
detoasts to the logical value, and the logical-size contract is enforced before
any write (`vr_toast.c:170-173`). The header comment in `varatt.h` now matches
the implementation: `vr_body_size` is documented as the saved physical
(possibly compressed) stream size (B6 fixed). Conclusion: the physical stream is interchangeable
(recompressible, relocatable, copied verbatim on rewrite); only the
descriptor+kind contract and the reconstructed logical value are stable. Fix
the stale comment before any identity reasoning leans on the field name.

**The accepted line already chose SAFE REFUSAL + copy over the prototype's approach.**
The superseded prototype used "substrate" terminology and an across-TOAST
rejection design. The accepted code keeps the same safety posture (refuse a
mis-homed locator) but adds the physical-verbatim copy so rewrite preserves the
VR representation instead of flattening or failing. Conclusion: copy/rehome is
the deliberate, evidence-backed v0 posture, not a placeholder.

---

## 2. Downward — accepted source facts (line-exact, `b61ebe28e7`)

Descriptor / locator (`src/include/varatt.h`)
```text
varatt_vr (varatt.h:61)
  vr_kind        uint8   on-disk constant; selects VrKind                 (:63)
  vr_version     uint8   representation format version                    (:64)
  vr_flags       uint16  low 2 bits = compression of SAVED PHYSICAL stream(:65,106-111)
  vr_logical_size int32  flattened logical VARSIZE incl VARHDRSZ          (:67)
  vr_body_size   int32   SAVED PHYSICAL stream size (see vr_toast.c:128)  (:71  comment stale)
  vr_storage_oid Oid     locator: TOAST relation (v1)                     (:72)
  vr_valueid     Oid     locator: value id within it                     (:73)
VARTAG_VR = 19 (persistent), VARTAG_VR_INMEM = 4 (transient, never on disk) (:161-163,82-86)
```

Public contract (`src/include/access/value_representation.h`)
```text
VrKind enum, fixed, append-only (value_representation.h:49-56)
vr_body_size()  -> LOGICAL body size = vr_logical_size - VARHDRSZ; != logical_size (:101,107,512)
vr_body_read()  -> logical bytes, offset/len within [0, vr_body_size())          (:105,109)
```

Save / read / delete / rehome (`src/backend/access/common/vr_toast.c`)
```text
vr_toast_body_save     :141  logical-size guard ereport :170-173; stores via toast path;
                             vr_body_size := GET_EXTSIZE(ve) (physical) :229
vr_build_source_external:287 rejects unknown flags :291-293 and unknown method :315-317
                             BEFORE locator use; rebuilds external (rawsize=logical,
                             extsize=physical, method from flags) :297-318
vr_toast_body_copy_to_relation:347 COPY not move :337-341; fetch as-stored
                             (detoast_external_attr, no decompress) :380;
                             toast_save_datum verbatim :383; new locator from ve_new :415-416
vr_toast_get_locator   :76   identity read = (storage_oid, valueid) :86-87; locator-private
vr_toast_same_body     :102  same body iff identical (storage_oid, valueid) :111
vr_toast_body_size     :435  PHYSICAL accessor (compressed when compressed) :442
vr_body_read           :456  reconstruct + detoast_attr; Assert logical match :476-483
```

Rewrite / VACUUM FULL / CLUSTER / REPACK / ALTER and the home invariant
```text
toast_helper.c:178-215  relocate when ttc_oldvalues==NULL && VR &&
                        vr_storage_oid != reltoastrelid -> copy_to_relation;
                        clears need_detoast; precedent jsonb_toaster copy_toast :175-176
heaptoast.c:178-198     final safety net: home = rd_toastoid?:reltoastrelid;
                        vr_storage_oid != home -> ERROR (no dangling locator persisted)
```

GC / delete dispatch (`src/backend/access/common/toast_internals.c`)
```text
toast_delete_chunks_by_id:409  scan by valueid; simple_heap_delete or
                               heap_abort_speculative; lock to commit :444-464
toast_delete_datum       :473  EXTERNAL_VR -> vr_toast_body_delete(loc) :489-508;
                               is_speculative propagated; VARTAG_VR_INMEM nothing :511-515
toast_helper.c:95-118          UPDATE: delete_old unless same_body -> NEEDS_DELETE_OLD / IGNORE
```

Logical decoding rejection
```text
reorderbuffer.c:5163-5175  VARATT_IS_VR -> ERROR "logical decoding of a value
                           representation is not supported"
proto.c:827-837            attlen==-1 && VARATT_IS_VR -> ERROR "logical replication
                           of a value representation is not supported"
```

RFC v3.1.6 storage language (`docs/VALUE_REPRESENTATION_RFC_v3_1_6.md`)
```text
3b:471  "A VR value is identified by its VR descriptor, kind, version, validation
        rules, locator semantics, and flattening behavior. A TOAST pointer is not
        VR identity."
3c:494-498  ToastAPI ceiling (custom chunking, partial read, delta, structural reuse)
            moves WAL/MVCC/rewrite/VACUUM/orphan/crash/decoding obligations into each toaster
3e:631-665  storage classes: 1 descriptor-only, 2 inline-body, 3 TOAST-backed (v0),
            4 future core-owned non-TOAST value-owned, 5 existing LO (prior art/contrast)
```

---

## 3. Forward — design analysis

**Q1. What exactly is VR body identity today?**
Two separable things. For *addressing*, identity is the VR body locator
`(storage_oid, valueid)` — the only thing `vr_toast_same_body` compares
(`vr_toast.c:111`). For *value meaning*, identity is the descriptor+kind
contract: `(vr_kind, vr_version, vr_logical_size, compression-method)` plus the
logical value the body reconstructs to. The locator is **relation-local and
re-minted on every rewrite** (copy -> new `valueid`, `vr_toast.c:415-416`), so
it is not a stable identity across rewrites; it is an ephemeral address valid
only for the current relfilenode generation.

**Q2. Is identity the locator, the physical stream, the logical value, or the descriptor+kind contract?**
The **locator** for addressing and the **descriptor+kind contract** for value
meaning. It is explicitly **not the physical stream**: there is no
content-addressing, the stream is copied verbatim on rewrite, and two
byte-identical streams under different locators are different bodies. It is
explicitly **not the logical value**: the same logical value can recur in many
rows without sharing a body. The locator is the sole "same body" test today,
and it is per-relation and re-minted on rewrite.

**Q3. What breaks if a body outlives one heap tuple version?**
It already outlives one version safely in exactly one case — an UPDATE whose VR
is unchanged shares the locator across the old and new versions
(`toast_helper.c:95-118`), which is safe because (a) bodies are immutable once
written and (b) reclamation fires only on the value-changing/removing operation,
so the body is freed when no live version references it. What breaks under
*generalized* outliving (two independent rows, or rows in two relfilenode
generations, sharing one body): with no reference count, the first delete
reclaims the body and every other reference dangles, while VACUUM cannot tell
the body is still referenced elsewhere. The UPDATE case is safe only because it
is a single linear version chain with last-reference reclamation, not true
multi-owner sharing.

**Q4. What would be required for no-copy rewrite? (two levels — do not conflate.)**
**F2(a) — valueid-stable, dedup-correct rewrite (in-core, no new store).** Ordinary
TOAST already preserves the value OID across rewrite and skips re-writing a body
already present in the new toast table (`toast_internals.c:198-303`, valueid reuse
`:262`, short-circuit `:281-286`). VR is excluded from both (`:250-253` falls
through to a fresh OID) — the root of B0/F1. Making VR participate in the existing
`oldexternal`/valueid mechanism is an in-core change with no new body store and no
descriptor format change; it preserves the locator's valueid across rewrite
(partial identity stability) and **closes B0/F1/P3** if proven. Chunks are still
re-written once into the new relfilenode, exactly as ordinary TOAST.
**F2(b) — true no-copy (chunks not re-written).** Needs: a body store whose lifetime
is decoupled from the relfilenode (the old relfilenode + TOAST are dropped at the
swap) — RFC class 4, core-owned, value-owned; a stable cross-relfilenode identity;
a GC that survives the swap (refcount or mark-sweep, MVCC- and crash-correct); a
replacement for the home-OID invariant (`heaptoast.c:193`) that does not reintroduce
dangling; and WAL + crash recovery for the decoupled store. F2(b) crosses into
locator/format territory and is not part of the next implementation.

**Q5. What GC rule would prevent orphan bodies?**
For v0 (copy/rehome) the existing rule is already correct and needs no change:
reclaim via `toast_delete_datum -> vr_toast_body_delete` at the
value-changing/removing operation; protect UPDATE-chain sharing with
`same_body`; reclaim old bodies on rewrite by dropping the old relfilenode +
TOAST. (This reclaims the OLD store only; it does not prevent duplicate bodies on
the NEW side — see B0/F1: VR skips TOAST's valueid reuse, so multi-version rewrite
writes duplicate bodies that VACUUM never reclaims.) For any future shared/identity-bearing body the orphan-prevention rule
**must be an explicit refcount or mark-sweep on the value-owned store**: a body
is live iff at least one visible tuple references its identity; reclaim only at
zero references under the correct snapshot. This is named here as a design
obligation; it is not designed in this note and must not be hand-waved.

**Q6. What crash-recovery invariant is needed?**
Today: VR body chunks are ordinary TOAST chunks, WAL-logged with the heap
insert (the rehome inherits `ttc_options`, e.g. `HEAP_INSERT_NO_LOGICAL`
during rewrite, `toast_helper.c:172-173`) and recovered atomically with the
heap; a speculative/aborted body is super-deleted
(`toast_internals.c:451-454`). Invariant: **VR body durability equals the
durability of its owning heap tuple version — a committed tuple's body is
recoverable exactly as ordinary TOAST, and an aborted body is super-deleted; no
separate recovery contract exists for the paths exercised.** Scope: only committed
INSERT and VACUUM FULL rewrite are crash->recover verified; `wal_level=minimal`,
speculative-abort, and REPACK CONCURRENTLY rest on inheritance, not on test. Note
B0/F1 orphan duplicates are WAL-durable, so that leak survives recovery.** A future decoupled store would need the
body store and the referencing tuple to commit/abort atomically, or a
recovery-time reconciliation that never leaves a referenced body missing nor an
unreferenced body live.

**Q7. What would logical decoding need to represent?**
Today VR is refused on both paths (`reorderbuffer.c:5172-5175`,
`proto.c:834-837`). To support decoding at all (independent of identity work),
the decoder must obtain the logical value: either flatten VR to its logical
value before it enters the change stream, or carry the descriptor+body so a
downstream with the same kind registry can reconstruct it. Identity
preservation makes this harder, not easier: a shared body would also need its
identity (or its logical materialization) represented so the replica does not
assume per-row independent bodies. Decoding stays a separate, currently-refused
concern; identity work must not silently change what the still-refused decoder
would have to carry.

**Q8. What must remain explicitly out of scope?**
No implementation; no descriptor format change; no GC by hand-waving; no dedup,
content-addressing, dictionary, partial read, delta, or COW. Also deferred:
relaxing the home-OID invariant, introducing a refcount, cross-relfilenode body
identity, the decoupled value-owned body store (RFC class 4), and logical
decoding support. This note records what each would require; it designs none of
them.

---

## 4. Current-state lifecycle graph

Flow (one VR-bearing attribute)
```text
INSERT   value -> vr_make_save_body -> body in rel TOAST (valueid V)
                                      locator = (reltoastrelid, V)
UPDATE   new locator == old ?
           yes -> reuse locator, KEEP body            (same_body; TOASTCOL_IGNORE)
           no  -> NEEDS_DELETE_OLD -> toast_delete_datum(old) -> delete body chunks
DELETE   toast_delete_datum -> vr_toast_body_delete(storage_oid, valueid)
REWRITE  per tuple -> copy_to_relation -> NEW body in new physical TOAST
                      locator stamped rd_toastoid (surviving home)
                      OLD body dropped with old relfilenode at swap   (COPY, not move)
                      !! NO valueid reuse: multi-version rewrite writes DUPLICATE
                         bodies; dead-version copies orphan forever (B0/F1)
STORE    if locator != home (rd_toastoid ? : reltoastrelid) -> ERROR  (safety net)
DECODE   VR datum -> ERROR (serialize and decode both refuse)
```

Layering
```text
logical SQL value                       (the kind owns its semantics)
  | flatten / reconstruct               (detoast_attr; bounded by vr_logical_size)
VR descriptor                           (kind, version, flags, logical_size,
                                         body_size=physical, locator)
  | locator (storage_oid, valueid) ->
TOAST-backed body store                 (ordinary toast relation; chunks;
                                         WAL/MVCC/VACUUM/recovery all in core)
```

**Conclusion: identity today is an ephemeral, relation-local locator over a
per-relfilenode TOAST body, with reclamation tied to the heap tuple version and
sharing confined to the UPDATE-unchanged chain. Rewrite is clean on the OLD side
(drop) but NOT on the NEW side: lacking TOAST's valueid dedup, multi-version
rewrite leaks duplicate orphan bodies (B0/F1, reproduced).**

---

## 5. Proposed future-state lifecycle graph (contrast only, gated)

Shown to make the required invariants concrete. It is not a commitment and not a
design; it is the shape any body-identity-preserving variant would have to take.

Flow
```text
INSERT/UPDATE/DELETE   as today, but the body store is value-owned and
                       relfilenode-independent
REWRITE (no-copy)      tuple keeps its body identity; the new relfilenode
                       references the SAME body identity; no per-tuple copy
GC                     refcount or epoch mark-sweep on the value-owned store;
                       reclaim at zero visible references
STORE                  home-OID invariant replaced by "identity resolvable and
                       refcount-correct" (must not reintroduce dangling)
CRASH                  body-store WAL + reconciliation: never referenced-missing,
                       never unreferenced-live
DECODE                 still must reconstruct the logical value or carry identity
                       (unchanged obligation; currently refused)
```

Layering
```text
logical SQL value
  | flatten / reconstruct
VR descriptor                           (stable cross-relfilenode body identity)
  | identity ->
core-owned, value-owned body store      (RFC class 4: own WAL, own GC
                                         (refcount/mark-sweep), relfilenode-independent)
```

**Conclusion: every box that changes between Section 4 and Section 5 is a
blocker below; none is started here.**

---

## 6. Invariants (must hold; any future change must preserve or explicitly replace)

```text
I1  Home-OID: a persistent VR locator must name the relation's effective TOAST
    home (rd_toastoid during rewrite, else reltoastrelid).      heaptoast.c:179-196
I2  Logical-size contract: logical_size == VARHDRSZ + uncompressed body;
    ereport before any storage write.                           vr_toast.c:170-173
I3  Reconstruction safety: unknown flag bits or unknown method ERROR before any
    locator/body access.                                        vr_toast.c:291-318
I4  Reader bound: reads are logical, bounded by [0, vr_body_size()); logical
    value reconstructed via detoast_attr.        value_representation.h:105; vr_toast.c:468-483
    COST: vr_body_read(offset,len) fully detoasts the whole body every call
    (vr_toast.c:482) -> O(full body) per call, not O(len); N windowed reads are
    O(N x body).  The offset/len API advertises windowing the implementation
    lacks; a trap for any future windowing kind (P2).
I5  GC tie: a body is reclaimed exactly when its ordinary-TOAST owner would be
    (delete / value-changing UPDATE); speculative-abort propagated.
                                          toast_internals.c:489-508; toast_helper.c:95-118
I6  Copy-not-move on rewrite: the body is copied to the new home and the old is
    not deleted; old reclaimed by dropping old relfilenode+TOAST at swap.
    CAVEAT: unlike TOAST, VR does not reuse the value OID on rewrite
    (toast_internals.c:250-253), so multi-version rewrite writes duplicate bodies
    and dead-version copies orphan forever -- see B0/F1 (reproduced).  RESOLVED by
    F2(a) (vr-rewrite-valueid-reuse-v0, commits 7c8d6df + b9f5e91): the rewrite
    copy passes the source pointer as oldexternal so toast_save_datum reuses the
    value OID and short-circuits the duplicate write, exactly as ordinary TOAST.
                                          vr_toast.c:337-341; toast_helper.c:164-166
I7  Decoding refusal: VR is rejected in logical decoding and replication.
                                          reorderbuffer.c:5172-5175; proto.c:834-837
I8  Transient never persisted: VARTAG_VR_INMEM is readable/flattenable but never
    written to disk.                                            varatt.h:82-86
```

---

## 7. Blockers (gate body-identity preservation / no-copy rewrite)

```text
B0  Orphan duplicate bodies on multi-version rewrite (correctness + space leak;
    EMPIRICALLY REPRODUCED).  VR does not reuse the TOAST value OID on rewrite
    (toast_internals.c:250-253) and has no analog of TOAST's cross-version
    short-circuit (:281-286).  When a rewrite copies several versions of a row
    sharing one VR body (UPDATE with unchanged VR keeps a shared locator,
    toast_helper.c:95-118; the prior version recently-dead under a held snapshot),
    VR writes N distinct bodies where TOAST writes one; copies on dead versions
    are never reclaimed (toast_delete_datum runs on UPDATE/DELETE, not on VACUUM
    of a dead tuple).  Repro: held snapshot + UPDATE (j unchanged) + VACUUM FULL ->
    bodies VR=2 vs TOAST=1; after a plain VACUUM, live=1 but bodies stay 2 (one
    permanent orphan); toast size ~2x (32kB vs 16kB), rewrite WAL ~1.38x (54 vs
    39kB).  RESOLVED by F2(a) (accepted series on vr-rewrite-valueid-reuse-v0):
    7c8d6df reuses the value OID on rewrite so a shared body is stored once, and
    b9f5e91 skips the per-version full-body fetch for a duplicate (one body read
    per distinct value instead of per version).  Repro green under VACUUM FULL and
    REPACK: bodies 1 vs 1, toast 16 vs 16kB, WAL 39 vs 39kB; after a plain VACUUM
    live=1 bodies=1 (no orphan).  The home-OID safety net does NOT catch this
    (duplicates name the correct home); the fix prevents it at the source.
B1  Storage lifetime: per-relation TOAST ownership is destroyed at the
    relfilenode swap; bodies cannot survive rewrite in the current store.
    Needs RFC class 4 (core-owned, value-owned, relfilenode-independent).
B2  GC: no reference count and no multi-owner sharing semantics beyond the
    UPDATE same_body chain; shared identity needs a designed refcount or
    mark-sweep, MVCC- and crash-correct. Not hand-wavable.
B3  Safety net: I1 (home-OID) forbids cross-relation locators by design;
    identity preservation requires replacing I1 without reintroducing dangling.
B4  Crash recovery: today body durability == heap-tuple durability; a decoupled
    store needs its own WAL and a reconciliation rule (never referenced-missing,
    never unreferenced-live).
B5  Logical decoding: VR is currently refused (I7); identity preservation
    sharpens, not relaxes, the "what does the stream carry" question.
B6  Contract hygiene (FIXED): vr_body_size is physical in code; the varatt.h
    comment is now corrected to say saved physical (possibly compressed) stream
    size, not "uncompressed".  Identity is defined on (kind, version,
    logical_size, validated logical content), never on the physical field.
B-repl  Logical replication slot wedge (operational; EMPIRICALLY REPRODUCED).
    A VR datum reaching logical decoding raises ERROR (reorderbuffer.c:5172-5175
    during decode reassembly; proto.c:834-837 on the wire).  The refusal is the
    correct safety choice, but the consequence is not cosmetic: an ERROR while
    decoding a committed xact means the slot's confirmed_flush_lsn cannot advance
    -> the slot wedges and WAL is retained until intervention.  A storage-side
    decision (a value going VR-cold) on a published table thus silently disables
    logical replication and accumulates WAL.  Repro: test_decoding slot + VR-cold
    INSERT -> pg_logical_slot_get_changes ERROR "logical decoding of a value
    representation is not supported"; confirmed_flush_lsn unchanged across the
    failed call; repeat call errors again (permanently stuck).  This is an
    explicit deployment boundary, not a documentation note.
    GATED for new construction (vr-brepl-gate-v0): vr_make_save_body refuses
    when RelationIsLogicallyLogged(rel), before any body write, so a cold
    decision cannot create a wedge-hazard value on a logically logged relation.
    RESIDUAL: VR created before the relation became logically logged (e.g.
    wal_level raised later) is not caught by the construction gate and remains
    covered only by the decode-time backstop.  The rewrite/relocate path is
    intentionally not gated, so VACUUM FULL/CLUSTER/REPACK of pre-existing VR is
    unaffected.  Logical decoding support is not implemented.
```

---

## 8. Recommendation and current posture

Three options were posed: A. keep copy/rehome only; B. allow immutable body
identity later under specific rules; C. reject body identity preservation for v1.

**As originally written, option A was not production-safe:** two empirically
reproduced boundaries of blocker rank sat inside the body lifetime/propagation
domain -- B0/F1 (multi-version rewrite wrote duplicate orphan bodies, a
correctness + space leak) and B-repl (a VR-cold value wedges a logical
replication slot and retains WAL).

**F2(a) is now implemented and RESOLVES B0/F1.** The accepted series on branch
vr-rewrite-valueid-reuse-v0 -- 7c8d6df (reuse the value OID on rewrite) and
b9f5e91 (skip the duplicate-version body fetch) -- makes VR participate in the
existing TOAST value-OID-preservation path: a rewrite stores one body per
distinct VR value, with WAL/space/body-count parity restored and no dead-version
orphan, verified under VACUUM FULL and REPACK.  No new body store and no
descriptor format change.

**F2(b) / true no-copy body identity preservation is NOT started.** It stays
deferred behind B1-B6, each its own evidence-backed milestone, with the GC rule
(B2) and the I1 replacement (B3) designed and reviewed before any code.

**B-repl is gated for new construction; a residual hazard remains.** The hard
refusal of VR in logical decoding is the correct safety choice, but the silent
slot wedge and WAL retention were an explicit deployment boundary.  It is now
gated at construction: vr_make_save_body refuses when RelationIsLogicallyLogged
(vr-brepl-gate-v0), before any body write, so a storage-side cold decision cannot
create a wedge-hazard value on a logically logged relation.  Residual: VR created
before the relation became logically logged (e.g. wal_level raised later) is not
caught by the construction gate and is covered only by the decode-time backstop;
the rewrite/relocate path is intentionally not gated, so VACUUM FULL/CLUSTER/REPACK
of pre-existing VR is unaffected.  Logical decoding support is not implemented.
B-repl is on a separate track from F2(a) and must not be conflated with it.

**Current safe posture after F2(a): physical-verbatim copy/rehome with a
valueid-stable, dedup-correct rewrite.**  This is option A made safe for the
rewrite domain -- a mechanism posture, not a lifecycle endpoint; B-repl gating and
the B1-B6 path to any future F2(b) remain open.

C is not warranted: nothing in the current descriptor or locator forecloses a
future value-owned body store (RFC 3e keeps classes 1, 2, 4 open at `:655`), so a
permanent rejection would discard reachable design space for no safety gain.
