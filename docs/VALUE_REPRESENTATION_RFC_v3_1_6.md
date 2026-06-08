# Value Representation: lifecycle interface for type-defined persistent values

Draft v3.1.6. Gate-applied working version. v3.1.6 keeps the v3.1.5 API boundary and strengthens the architectural text around TOAST, ToastAPI, and future non-TOAST body storage.

This version deliberately uses PostgreSQL-ish storage terminology. The preferred terms are:

```text
body storage
storage layer
storage mechanism
storage representation
TOAST-backed body storage
TOAST-compatible body storage
```

Core statement:

```text
TOAST stores VR body bytes in v0.
TOAST does not define VR identity.
VR owns value representation and body lifecycle.
```

## 0. v3.1.6 changes

This version incorporates the API gate review from v3.1.5 and adds three architectural clarifications.

```text
T1: TOAST-backed body storage is the current v0 implementation choice for
    large VR bodies. It is not the identity or boundary of VR.

T2: VR is broader than ToastAPI. ToastAPI exposed a storage-mechanism seam;
    VR defines an in-core value-representation seam.

T3: Large Objects are useful prior art for non-TOAST external data, but they
    are object-owned, not value-owned. A future LO-like VR body store must
    be value-owned and must not reuse Large Object semantics directly.
```

Accepted v3.1 gate changes remain:

```text
A1: VrValidateContext has report/report_arg for amcheck-style collection.

A2: rewrite body transfer is copy, not relocate.
    Internal helper: vr_toast_body_copy_to_relation(...).
    Semantics: copy body into target physical home; old body remains governed
    by old lifecycle.

A3: logical decoding uses a transient in-memory VR body form.
    This is not a persistent vr_flags bit and is invalid for heap storage.
    vr_header_info()/vr_body_read() work on both on-disk and transient forms.

A4: varatt_vr has both vr_logical_size and vr_body_size.
    Both are fixed-width on-disk int32 fields.

C1: VR remains a distinct external tag, but update-time ownership/share
    detection must include VR external bodies like ordinary ONDISK toast values.

C2: VrKind is a fixed in-core on-disk registry in v1.
    Unknown kind is hard ERROR.
    pg_upgrade pre-flight must reject unknown kinds before production use.
```

v1 remains narrow:

```text
in-core only;
fixed VrKind enum;
no handler;
no DDL;
no catalog object;
no extension registration;
external-only VR;
ordinary TOAST-backed body storage;
no relation-level preserve/swap;
no inline VR;
no non-transactional own-GC storage layer;
no semantic read API;
no raw write.
```

## 1. Thesis

PostgreSQL types are extensible on the read side, but persistent write-side representation did not become equally extensible.

A type can define its logical behavior:

```text
input/output
operators
functions
casts
opclasses
statistics
subscripting
expanded in-memory form
```

But persistent value storage is still mostly:

```text
ordinary flat datum / varlena
generic type-agnostic TOAST
```

TOAST solved a very important general problem: large values can be stored safely outside the heap tuple, with WAL, MVCC, vacuum, rewrite and crash recovery handled by PostgreSQL.

But TOAST is deliberately type-agnostic. It sees large bytes, not the internal structure of a value.

This limitation becomes visible when a type is not just large, but structurally large. `jsonb` is the clearest example.

A `jsonb` value may contain:

```text
small hot metadata
large cold payloads
keys
paths
nested containers
repeated structure
parts that are read often
parts that are rarely read
parts that are updated independently at the logical level
```

The type knows this structure. TOAST does not.

So PostgreSQL can extend how a type is read and interpreted, but it has no small general interface for saying:

```text
this logical value has a type-defined persistent physical representation,
and PostgreSQL must preserve that representation correctly across write-side
lifecycle paths.
```

Value Representation is the proposed missing boundary.

In compact form:

```text
type:
  owns semantics and read path

Value Representation:
  owns write-side/lifecycle obligations of the type-defined representation

TOAST-backed body storage, v0:
  stores and fetches raw representation body bytes
```

Or shorter:

```text
jsonb owns meaning.
VR owns lifecycle.
TOAST stores bytes.
```

## 1a. Why we are doing this

This work is not about making a new TOAST implementation.

It started from a practical PostgreSQL problem: some modern values are not just large byte strings. They are structurally large values. The clearest example is `jsonb`.

Ordinary TOAST treats the whole serialized value as a large byte string. That is safe and general, but it is type-agnostic.

```text
TOAST knows:
  this is a large varlena value

jsonb knows:
  this field is hot
  this payload is cold
  this key is often read
  this part is rarely read
  this update logically touches only part of the value
```

The goal is to keep PostgreSQL correctness while giving selected types a way to define a better persistent physical representation for one logical value.

The immediate motivation is `jsonb`, because `jsonb` is becoming a natural container for semi-structured application state, document-like data, event payloads, and AI/task-oriented workloads.

In those workloads, one value often has a temperature structure:

```text
hot:
  small metadata, status, ids, routing keys, current step

warm:
  compact context, constraints, summaries, tool-result metadata

cold:
  large evidence, raw outputs, logs, snapshots, bodies, rarely-read payloads
```

The database should not have to read, rewrite or physically manage the cold part every time the hot part is accessed or changed.

But the solution cannot be a hidden `jsonb` trick. PostgreSQL must still know enough to preserve the representation across:

```text
UPDATE
VACUUM
VACUUM FULL
CLUSTER
ALTER TABLE rewrite
logical decoding
rollback
pg_upgrade
```

That is why this work moved from "custom TOAST" to Value Representation.

```text
TOAST solved large values.
VR addresses structurally large values.
jsonb exposed the gap.
```

## 2. Interface hierarchy

```text
PostgreSQL type system
│
├── jsonb
│   │
│   ├── owns semantics
│   │     ├── keys
│   │     ├── paths
│   │     ├── jsonpath
│   │     ├── containment
│   │     ├── jsonb_set / update rules
│   │     └── jsonb-specific read path
│   │
│   ├── owns representation policy
│   │     ├── what is hot
│   │     ├── what is cold
│   │     ├── body layout
│   │     ├── directories / offsets
│   │     └── version-specific format
│   │
│   └── uses Value Representation
│         │
│         v
│
├── other possible VR-aware types
│   │
│   ├── bytea block representation
│   ├── vector array representation
│   ├── sparse array representation
│   ├── tree/path representation
│   └── per-value dictionary representation
│
└─────────────────────────────────────────────────────────────

Value Representation layer
│
├── value-level lifecycle interface
│   │
│   ├── make
│   ├── replace
│   ├── rewrite
│   ├── cleanup
│   ├── validate
│   └── flatten
│
├── mechanical access only
│   │
│   ├── vr_header_info()
│   ├── vr_body_size()
│   └── vr_body_read()
│
├── owns lifecycle visibility
│   │
│   ├── INSERT
│   ├── UPDATE
│   ├── DELETE / VACUUM
│   ├── VACUUM FULL
│   ├── CLUSTER
│   ├── ALTER TABLE rewrite
│   ├── logical decoding
│   └── pg_upgrade checks
│
└── does NOT own type semantics
    │
    ├── no jsonb key API
    ├── no generic slice API
    ├── no vector API
    └── no semantic partial-read API

TOAST-backed body storage, v0
│
├── type-agnostic durable body storage
│   │
│   ├── stores raw representation body bytes
│   ├── fetches raw body ranges
│   ├── copies body to new physical home
│   └── deletes body through normal lifecycle
│
├── provides PostgreSQL storage guarantees
│   │
│   ├── WAL
│   ├── MVCC
│   ├── vacuum
│   ├── crash recovery
│   ├── replication
│   └── relation lifetime
│
└── does NOT understand value structure
    │
    ├── no jsonb keys
    ├── no paths
    ├── no hot/cold meaning
    ├── no vector blocks
    └── no dictionary semantics
```

## 3. How VR relates to TOAST

Strictly speaking, TOAST does not use VR as an upper-level API.

The direction is:

```text
VR uses TOAST-backed body storage for v0 large bodies.
```

But selected TOAST/varlena lifecycle paths must become VR-aware, because a VR value is an external value with its own pointer/header layout and its own body ownership.

Correct hierarchy:

```text
jsonb / other type
│
│  defines:
│    logical semantics
│    body format
│    hot/cold policy
│    read path
│
v
Value Representation
│
│  defines:
│    make / replace / rewrite / cleanup / validate / flatten
│    persistent representation lifecycle
│
v
TOAST-backed body storage, v0
│
│  provides:
│    durable storage of opaque body bytes
│    body read
│    body copy to another relation
│    body delete through existing TOAST lifecycle
│
v
pg_toast table + index + WAL + MVCC + vacuum
```

The contact points are:

```text
make / replace:
  type builds representation body
  VR asks TOAST-backed body storage to store opaque body bytes
  storage layer returns a VR pointer/header Datum

read:
  type calls vr_body_read()
  v0 implementation fetches raw bytes from TOAST chunks
  type interprets those bytes

rewrite:
  VR method calls vr_toast_body_copy_to_relation()
  storage layer copies body bytes to target relation storage
  old body remains under old lifecycle

update/share detection:
  ordinary TOAST compares va_toastrelid + va_valueid
  VR-aware path compares vr_storage_oid + vr_valueid
  this must be explicit; VR is not varatt_external ONDISK layout

cleanup/delete:
  ordinary ONDISK pointer uses ordinary TOAST delete path
  VR pointer uses VR-aware TOAST-backed body delete path

detoast/flatten:
  ordinary ONDISK fetches chunks into flat varlena
  VR dispatches by VrKind and calls flatten()
```

Important rule:

```text
VR must not globally become VARATT_IS_EXTERNAL_ONDISK.
```

`VARATT_IS_EXTERNAL_ONDISK` implies ordinary `varatt_external` layout. VR has a distinct external tag and a distinct pointer/header layout. Therefore VR-aware behavior must be added explicitly where needed.

The right wording is:

```text
VR sits above TOAST as a value-representation lifecycle layer.

TOAST stores the VR body bytes in v0.

TOAST/varlena lifecycle code must recognize VR pointers where ordinary
ONDISK assumptions would otherwise be wrong.

TOAST does not interpret VR bodies.
```

## 3a. TOAST compatibility is not VR identity

The early implementation work must touch TOAST and varlena code because that is where PostgreSQL already handles long values, external datums, compression, WAL, MVCC, vacuum, rewrite, crash recovery and logical decoding boundaries. That is an integration requirement, not the architectural identity of VR.

The correct reading is:

```text
VR starts TOAST-compatible.
VR must not become TOAST-defined.
```

TOAST is a concrete storage mechanism for large varlena bytes. It owns external pointer layout, toast relation, chunks, compression, chunk index, fetch, copy and delete rules.

VR is a value-representation ownership layer. It owns the lifecycle of a type-defined persistent physical representation of one logical value:

```text
tuple-local descriptor:
  kind
  version
  flags
  logical size
  representation body reference

representation-owned body:
  body format
  validation rules
  copy/rebuild/rehome policy
  cleanup/reachability obligations
  optional compression, sharing, dedup, directories, deltas or block maps
```

In v0, ordinary TOAST is the safest body storage for large VR bodies because PostgreSQL already knows how to make it transactional, WAL-logged, vacuumed, rewritten and crash-safe. Using it keeps the first implementation reviewable.

But if a VR value always means only "save bytes through ordinary TOAST and flatten them back", then VR has not yet become a new abstraction. That is only the compatibility floor.

The first VR-native step begins when the body has explicit representation identity and lifetime rules. At that point rewrite/repack/copy paths can be reasoned about in terms of representation ownership, not only in terms of copying another external varlena payload.

The project rule is:

```text
Use TOAST where it buys PostgreSQL correctness.
Do not let TOAST define the VR architecture.

TOAST stores large varlena bytes.
VR owns a value representation and its body lifecycle.
```

This distinction is why compression parity is a body-storage requirement, while body identity, sharing, deduplication, partial read and update-delta are VR lifecycle questions. They must be introduced only when ownership, reachability, rewrite behavior, validation and error handling are explicit.

## 3b. VR is not TOAST

VR is a value-representation layer, not a TOAST extension.

TOAST stores oversized varlena bytes. It is a storage mechanism. VR describes how a physical representation of a value is recognized, validated, owned, and reconstructed into the logical SQL value.

The current implementation uses TOAST-backed body storage for large VR bodies because this keeps WAL, MVCC, VACUUM, rewrite, crash recovery, and space accounting inside existing PostgreSQL mechanisms. This does not make VR a TOAST feature.

A VR value is identified by its VR descriptor, kind, version, validation rules, locator semantics, and flattening behavior. A TOAST pointer is not VR identity.

```text
TOAST:
  stores large varlena bytes

VR:
  defines explicit value representation and body lifecycle

TOAST-backed VR body storage:
  current v0 storage choice for large VR bodies
```

The important v0 statement is:

```text
TOAST stores VR body bytes in v0, but it does not define VR identity.
```

## 3c. VR and ToastAPI

VR is not a renamed ToastAPI.

ToastAPI placed the extension boundary at the storage-mechanism level: a toaster could take over toast, detoast, update, delete, chunking, reuse, and potentially partial or delta behavior for an attribute.

VR places the boundary at the value-representation level. A VR kind defines representation recognition, validation, flattening, and body lifecycle rules. It does not expose TOAST internals as a generic pluggable toaster interface.

ToastAPI has a higher performance ceiling for features such as custom chunking, partial read, delta update, and structural reuse. However, those features also move dangerous lifecycle obligations into each toaster implementation:

```text
WAL
MVCC
rewrite
VACUUM
orphan prevention
crash recovery
logical decoding
pg_upgrade availability
```

VR deliberately chooses a narrower v0 boundary. It keeps the dangerous storage lifecycle centralized in core and introduces explicit representation ownership above it.

Some VR mechanisms are informed by ToastAPI/JBTL experience:

```text
safe rehome
explicit ownership
locator validation
compressed-body preservation
physical-verbatim rehome
```

But these are reintroduced as narrow VR/core primitives, not as a generic ToastAPI provider framework.

The relationship is:

```text
ToastAPI experience
  -> VR lifecycle rules and selected safe primitives

not:

ToastAPI architecture
  -> VR
```

Short version:

```text
ToastAPI extended TOAST mechanics.
VR extends the model of a stored value.
```

## 3d. Large objects and LO-like non-TOAST body storage

PostgreSQL already has a non-TOAST mechanism for large binary data: Large Objects.

A table row stores an OID reference, while the object body is stored separately in `pg_largeobject` and described by `pg_largeobject_metadata`.

```text
heap tuple
  -> OID reference
      -> independent large object
```

This proves that large external data in PostgreSQL does not have to be stored through TOAST.

However, Large Objects are not ordinary value bodies. They have independent object identity and explicit object-level APIs. A row reference to a Large Object is not the same as tuple ownership of a value body.

This distinction matters for VR.

Large Object model:

```text
heap tuple
  -> OID reference
      -> independent large object
```

VR body model:

```text
heap tuple
  -> VR descriptor
      -> value-owned body
```

A future LO-like VR body storage may be useful, but it must not inherit Large Object semantics. It should not mean that a table row merely stores an OID to an independently managed blob. Instead, an LO-like VR body store would need value-owned lifecycle semantics:

```text
VR descriptor owns body
DELETE/VACUUM knows body
rewrite/repack has a rehome rule
logical decoding has a representation/body rule
crash recovery is defined
orphan prevention is tested
dump/restore behavior is defined
```

Existing Large Objects are useful prior art and contrast, but not a direct VR implementation shortcut.

Important historical lesson:

```text
LO:
  external object storage, independent lifecycle

TOAST:
  hidden value storage, table-owned lifecycle

VR:
  explicit value representation, value-owned body lifecycle
```

A future LO-like VR body storage must also not inherit the fixed small-page layout of PostgreSQL Large Objects. The Large Object `LOBLKSIZE` page model is an implementation detail of the Large Object API, not a requirement for value representation.

A VR locator should identify an owned body or body root. The internal split into chunks, blocks, or extents should remain private to the body storage layer.

```text
bad:
  VR locator = body page reference

better:
  VR locator = owned body root
  body storage layer decides chunk/block/extent layout
```

This keeps the VR descriptor stable while allowing future storage policies such as larger extents, compressed blocks, streaming layout, or immutable shared bodies.

Therefore:

```text
LO-like: yes, as future value-owned body storage.
Existing LO API: no, not as v0 VR body storage.
```

## 3e. VR body storage classes

VR is not limited to TOAST-backed bodies. The current implementation starts with TOAST-backed body storage because it is the safest production-compatible path for large values.

Possible VR storage classes are:

```text
1. Descriptor-only VR
   no external body
   example: generated/RLE/constant representation

2. Inline-body VR
   body stored inside the VARTAG_VR datum
   no TOAST
   example: compressed-inline representation

3. TOAST-backed external body
   current v0 implementation path

4. Future core-owned non-TOAST external body
   LO-like, but value-owned

5. Existing PostgreSQL Large Object
   prior art and contrast, not a v0 VR body store
```

The v0 implementation is intentionally focused on class 3.

The descriptor and locator design should not prevent classes 1, 2, or 4 in future versions.

The design rule is:

```text
VR must not be defined by TOAST.
VR must not regress to independent Large Object semantics.
VR body storage must be value-owned.
```

TOAST-backed body storage is the first safe implementation path. LO-like storage is a plausible future direction only if it becomes a core-owned, value-lifecycle-aware body storage layer, not a reuse of the existing Large Object API.

## 4. What this document describes

This document describes:

```text
type-defined persistent physical representation of one logical value
with explicit PostgreSQL lifecycle support
```

The proposed interface is **Value Representation**.

The key boundary is:

```text
type:
  owns semantics
  owns read path
  owns slice/key/path/vector/subtree interpretation
  owns the internal representation layout

Value Representation:
  owns lifecycle hooks for the persistent representation:
    flatten
    make
    replace
    rewrite
    cleanup
    validate

mechanical access:
  vr_header_info()
  vr_body_size()
  vr_body_read()

body storage layer:
  stores and fetches raw representation body bytes
  does not know type semantics
```

The goal is not to make core understand `jsonb`, vectors, arrays, trees or graphs. The goal is to let a type-defined persistent representation survive ordinary PostgreSQL lifecycle paths while the type keeps ownership of its semantics and read path.

## 5. Motivation: read path is not enough

The original problem did not start as a storage-framework design. It started from a narrower engineering need: a type may want to define its own persistent physical representation, but that representation must survive ordinary Postgres lifecycle paths.

A fast read path proves usefulness. It does not prove correctness. The hard part is lifecycle.

A type-defined persistent representation must remain valid across:

```text
INSERT
UPDATE
unchanged UPDATE ownership/share detection
VACUUM FULL
CLUSTER
ALTER TABLE rewrite
REPACK-like rewrite
DELETE
ABORT
logical decoding
pg_upgrade compatibility checks
fallback flattening
explicit validation
```

Without a lifecycle boundary, the implementation tends to become a collection of local fixes:

```text
special-case rewrite paths
guards around unsafe maintenance operations
ad-hoc copy rules
hidden ownership
private cleanup assumptions
readable values with unmanaged external fragments
```

Such an implementation may pass local read tests and still be architecturally weak. A value may be readable after a rewrite and still have lost its intended physical representation. Worse, an ordinary-looking value may secretly own external fragments that core lifecycle paths cannot see.

The invariant is:

```text
read path proves usefulness
lifecycle proves correctness
```

Value Representation exists to make type-defined persistent physical representation visible to the core lifecycle, without moving type semantics into core.

## 6. Lifecycle obligations

A Value Representation implementation must cover the following obligations.

```text
Path                  Required obligation

INSERT                make representation from logical value

UPDATE                replace old stored representation using new logical value

unchanged UPDATE      preserve external ownership/share detection correctly

VACUUM FULL           rewrite representation into target physical home
                      or explicitly rebuild/degrade safely

CLUSTER               same rewrite obligation

ALTER TABLE rewrite   same rewrite obligation

REPACK-like rewrite   rewrite or explicit safe refusal/degrade

DELETE/VACUUM         cleanup representation-owned resources

ABORT                 avoid reachable corrupt body or leaked ownership;
                      v0 TOAST-backed body storage relies on MVCC/vacuum

logical decoding      produce logical value or decoding-local transient body;
                      never emit opaque persistent VR pointer as logical data

pg_upgrade            detect unknown on-disk VrKind before file link/copy

fallback path         flatten representation to ordinary logical value

check/debug           validate representation invariants

type read path        type-private code, optionally using raw body read
```

These are not optimization hooks. They are correctness obligations for any persistent physical representation that is not the ordinary flat varlena form.

## 7. Historical line

The historical line starts wider than `varlena`. In the Berkeley POSTGRES design, extensibility was central: user-defined data types, operators and access methods were part of the system idea, together with complex objects and a relational foundation.

Large Objects are part of that history, but also show a cautionary boundary. They have persistent chunked storage, but they are not ordinary logical column values. Logical replication restrictions still expose that problem: a `pg_largeobject` chunk is not an independent logical value of a user table.

TOAST later made large varlena values part of ordinary SQL value lifecycle by storing oversized attributes transparently.

The useful historical framing is:

```text
LO showed that non-TOAST external storage is possible.
TOAST showed how to make large varlena values ordinary SQL values.
VR must keep value lifecycle while making physical representation explicit.
```

## 8. Value Representation and expanded datums

Expanded datums already show that a PostgreSQL type may need a representation different from its ordinary flat varlena form.

An expanded datum is a type-defined transient in-memory representation. It is owned by a `MemoryContext` and is flattened before it crosses the storage boundary.

Value Representation is the persistent counterpart of that idea.

```text
expanded datum:
  type-defined in-memory representation

Value Representation:
  type-defined persistent physical representation
```

The analogy is useful, but limited.

Expanded datums do not need to survive heap rewrite, VACUUM FULL, CLUSTER, logical decoding, DELETE/VACUUM, ABORT or pg_upgrade.

Value Representation does.

Therefore `ValueRepresentationMethods` is not a generic read API and not a storage-provider framework. It is a lifecycle interface for a persistent type-defined representation.

A type may use all three forms:

```text
flat datum:
  canonical portable logical representation

expanded datum:
  transient in-memory working representation

VR-backed datum:
  persistent physical representation
```

## 9. Name and v1 form

The public/conceptual name is:

```text
Value Representation
```

The main vtable name for v1 is:

```c
ValueRepresentationMethods
```

`Methods` is deliberate. `Routine` in Postgres tends to suggest a handler-returned provider object and would pull the discussion toward handler, DDL, registration and framework design. `Methods` aligns better with `ExpandedObjectMethods`: an in-core static method table.

v1 form:

```text
datum tag/header -> VrKind -> static ValueRepresentationMethods
```

not:

```text
catalog OID -> handler -> routine
```

v1 has:

```text
in-core static dispatch
no handler
no SQL DDL
no new catalog object
no extension-level provider registration
```

## 10. Context-safe recognition and header access

The representation must be recognizable from the datum itself.

Recognition must be context-safe:

```text
use datum-local tag/descriptor
avoid catalog lookup
avoid syscache lookup
avoid relation lookup in unsafe contexts
```

`VrKind` should not live in the ordinary varlena size header. Plain varlena values should remain unchanged.

Expected v1 shape:

```text
ordinary varlena header:
  unchanged

external/custom tag:
  VARTAG_VR or equivalent external tag

VR pointer/header:
  kind
  version
  flags
  logical_size
  body-storage-private locator
```

`vr_header_info()` exposes only mechanical, locator-free information:

```c
typedef struct VrHeaderInfo
{
    VrKind  kind;
    uint8   version;
    uint16  flags;        /* persistent on-disk pointer flags */
    Size    logical_size; /* flattened logical size, not body size */
} VrHeaderInfo;

extern bool vr_header_info(Datum stored_value, VrHeaderInfo *out);
```

Rules:

```text
vr_header_info():
  reads only the inline VR pointer/header
  is catalog-free
  returns false if the value is not VR
  exposes no storage_oid/valueid
  defines no type semantics
```

`storage_oid`, `valueid` and other body-storage locators should remain storage-layer internal. Debug-only helpers may expose them for tests, but not through the public type-visible accessor.

## 11. Kind and versioning

`VrKind` and physical format version are different concepts.

```text
VrKind:
  selects ValueRepresentationMethods

format version:
  selects descriptor/body format inside those methods
```

Rules:

```text
known old version:
  flatten/read if supported
  rebuild on rewrite if needed

current version:
  normal path

unknown version:
  ERROR with clear message
```

The method table should use `write_version`, not `max_version`, because the code may write one current version while reading several old versions internally.

## 12. Mechanical body access

`ValueRepresentationMethods` does not define read semantics.

There is no generic method for key lookup, slice, range read, subtree read, vector access or block access.

However, a type-owned read path must not be forced to flatten the whole value just to access its own physical representation. Therefore v1 needs raw body access:

```c
Size vr_body_size(Datum stored_value);

void vr_body_read(Datum stored_value,
                  Size offset,
                  Size len,
                  void *buf);
```

Coordinate space:

```text
offset/len address the representation BODY byte stream,
not the flattened logical value

vr_body_size != VrHeaderInfo.logical_size

body coordinates != logical coordinates

the type owns the body layout, computes offsets and interprets bytes

the storage layer only fetches bytes

the body is presented as one contiguous byte stream;
the storage layer hides physical chunking
```

This is mechanical access only. It is not a partial-read API.

The difference is:

```text
raw body access:
  storage layer returns bytes at offset/len

semantic partial read:
  type decides what key/slice/range/vector/subtree means
```

v1 exposes raw body read, not raw body write. Writes change ownership and must go through `make`, `replace` or `rewrite`.

## 13. Correctness rules

The physical representation must be lossless:

```text
flatten(physical_form) = logical_value
```

The physical representation must be recomputable:

```text
physical_form -> flatten -> make = valid current physical_form
```

The physical representation must not escape as logical output unless the output format explicitly defines it. In v1, logical output should receive the logical value, not an opaque VR pointer.

Unknown on-disk kind must fail closed:

```text
unknown VrKind -> ERROR
```

Unknown or malformed descriptor state must fail before unsafe locator/body access:

```text
unknown flags
reserved compression method
malformed logical/body sizes
invalid body locator
unsupported persistent in-memory tag
```

Do not rely on `Assert` for user-reachable or storage-corruption paths.

## 14. Rewrite and rehome

Rewrite is the key lifecycle operation.

The v1 method is per tuple:

```c
Datum (*rewrite)(Datum old_stored_value,
                 const VrRewriteContext *ctx);
```

Input:

```text
old_stored_value belongs to old physical home
```

Output:

```text
returned Datum belongs to target physical home implied by ctx->new_rel
```

The returned Datum may be:

```text
VR-backed:
  representation body copied or rebuilt in target physical home

ordinary flat:
  explicit method decision to degrade/decline, only if allowed

ERROR:
  if no safe result can be produced
```

The caller must not depend on whether the method copied or rebuilt the representation body. Those are body-storage decisions.

v1 rewrite is per-tuple. It does not perform relation-level physical preserve/swap. Relation-level preserve/swap is future work and requires a separate lifecycle hook.

For compressed bodies, physical-verbatim rehome is a narrow next optimization:

```text
old stored compressed physical stream
  -> copy physical stream into target relation body storage
  -> preserve logical size / physical size / compression method
  -> return new locator for target relation
```

This removes unnecessary CPU work:

```text
avoid:
  read logical body -> decompress -> save -> recompress

prefer:
  copy saved physical stream -> new locator
```

This must not change ownership model, GC, descriptor format or rewrite semantics.

## 15. Logical decoding

Logical decoding is a logical boundary, not a physical-body transport boundary.

The persistent VR pointer must not be emitted as logical data.

For TOAST-backed v0 bodies, logical decoding may reconstruct a decode-local transient VR form from decoded TOAST data and then call `flatten()` before output.

```text
publisher storage:
  persistent VARTAG_VR + body storage

decode-local form:
  transient VR body buffer

logical output:
  ordinary logical value

subscriber storage:
  ordinary apply path; if target type/column builds VR, it does so through make()
```

The transient form is runtime-only. It must never reach heap storage, TOAST save, rewrite output, WAL as tuple data, or logical output as stored datum.

## 16. Public boundary and internal helpers

Type code must not include `vr_toast.h` or call `toast_save_datum`.

The public-to-type layer is:

```text
ValueRepresentationMethods
VrHeaderInfo
vr_header_info()
vr_body_size()
vr_body_read()
```

The internal TOAST-backed body storage may expose helpers such as:

```c
typedef struct VrBodySaveRequest
{
    Relation      rel;
    AttrNumber    attnum;

    VrKind        kind;
    uint8         version;
    uint16        flags;

    Size          logical_size;

    const void   *body;
    Size          body_size;

    MemoryContext mcxt;
} VrBodySaveRequest;

Datum vr_toast_body_save(const VrBodySaveRequest *req);

Datum vr_toast_body_copy_to_relation(Datum old_stored,
                                     Relation new_rel,
                                     AttrNumber attnum,
                                     const VrRewriteContext *ctx);

Size vr_toast_body_size(Datum stored_value);

void vr_toast_body_read(Datum stored_value,
                        Size offset,
                        Size len,
                        void *buf);

typedef struct VrToastLocator
{
    Oid storage_oid;
    Oid valueid;
} VrToastLocator;

bool vr_toast_get_locator(Datum stored_value, VrToastLocator *out);
bool vr_toast_same_body(Datum a, Datum b);
```

`vr_toast_body_copy_to_relation()` is copy, not relocate.

Semantics:

```text
copy body into target physical home
return a new VR pointer/header for target tuple
old body remains governed by old tuple/relation lifecycle
no explicit delete of old body during rewrite
```

## 17. Suggested patch decomposition

### Patch 0001: core header skeleton and varatt sketch

```text
include/access/value_representation.h
VrKind
VrHeaderInfo
ValueRepresentationMethods
context structs
vr_header_info()
vr_body_size()
vr_body_read()
vr_lookup_methods()
varatt_vr sketch
external tag plumbing draft
unknown-kind hard ERROR policy
pg_upgrade pre-flight requirement documented
update-time VR ownership/share-detection obligation documented
no production type yet
```

### Patch 0002: varlena/ordinary TOAST-backed body storage

```text
VARTAG_VR or equivalent external tag
varatt_vr sketch
vr_toast body storage
vr_toast body read
vr_toast_body_copy_to_relation
per-tuple rewrite copy/rebuild/degrade
cleanup/validate helpers
transient in-memory VR form for logical decoding
```

No relation-level preserve/swap.

Sequencing rule for 0002:

```text
0002 must install VR recognition and safety guards before any code can create
a persistent VARTAG_VR datum.
```

The first commit that can create a persistent `VARTAG_VR` datum must already include:

```text
detoast / flatten dispatch for VARTAG_VR
toast_delete_datum VR handling
toast_save_datum guard against varatt_external misread
ownership/share detection guard or branch
logical decoding recognition or explicit ERROR
VARTAG_VR_INMEM rejection on heap/TOAST save paths
```

No patch in the series may leave a persistent VR datum constructible while delete/detoast/save/logical-decoding paths still silently skip or misinterpret it.

### Patch 0003: contrib/vr_test_vectors

```text
flat logical format
VR body format
make/flatten/replace/rewrite/cleanup/validate
type-owned get/slice/hash/storage_info
```

### Patch 0004: lifecycle regression tests

```text
construct -> recognize -> flatten -> compare bytes/hash
unknown kind ERROR
unknown flags ERROR
malformed descriptor ERROR before unsafe locator access
reserved compression method ERROR before locator/body access
rewrite/repack/cross-rel copy to new physical home
logical decoding transient path or explicit safe ERROR
no persistent VARTAG_VR_INMEM storage
```

### Patch 0005: first production user, VR_KIND_JSONB_COLD

```text
jsonb owns semantics
jsonb owns body format
VR owns lifecycle
TOAST-backed body storage stores opaque body bytes
flatten fallback returns ordinary jsonb
```

### Patch 0006: compressed body v0

```text
compressed body descriptor flags
logical size / physical body size distinction
compression method validation
read/flatten preserves logical behavior
WAL/space parity against ordinary TOAST for compressible body
guard coverage for unknown flags and reserved methods
```

### Patch 0007: physical-verbatim rehome v0

```text
copy saved compressed physical stream into target relation body storage
preserve logical size
preserve physical size
preserve compression method
produce new locator for target reltoastrelid
no ownership model change
no GC change
no descriptor format change
no rewrite semantics change
```

## 18. Future work boundaries

Not v0:

```text
body identity / lifetime decoupling
dedup / content-addressing
dictionary body storage
partial read as a semantic API
delta / COW chunks
custom external non-TOAST body store
relation-level preserve/swap
extension provider framework
catalog/DDL registration
```

Future non-TOAST body storage is allowed by the model, but it requires a separate lifecycle design.

A future core-owned LO-like body store would need answers for:

```text
WAL
MVCC
VACUUM
rewrite/rehome
crash recovery
orphan prevention
logical decoding
dump/restore
pg_upgrade
permissions
body root / locator validation
```

The current descriptor must not bake in TOAST chunking or Large Object page layout. It should identify a body root or body-storage-private locator, not expose internal chunks/pages as VR identity.

## 19. Summary

The design rule is:

```text
VR must not be defined by TOAST.
VR must not regress to independent Large Object semantics.
VR body storage must be value-owned.
```

TOAST-backed body storage is the first safe implementation path because it gives PostgreSQL-correct storage behavior for large bodies.

LO-like storage is a plausible future direction only if it becomes a core-owned, value-lifecycle-aware body storage layer, not a reuse of the existing Large Object API.

ToastAPI experience remains useful, but the lessons are imported as lifecycle rules and narrow core primitives, not as a generic storage-provider framework.

Final compact formula:

```text
TOAST stores large varlena bytes.
VR owns value representation and body lifecycle.
Large Objects show non-TOAST external storage, but also show why value ownership matters.
```
