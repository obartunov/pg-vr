# Value Representation: lifecycle interface for type-defined persistent values

Draft v3.1.4. Gate-applied working version. v3.1.4 content promoted to the current reference name after @r2d2 header-level synchronization.

## 0. v3.1 gate changes

This version incorporates the API gate review before writing patch 0001.

Accepted changes:

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
ordinary TOAST storage;
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

ordinary TOAST storage:
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

For `jsonb`:

```text
jsonb owns meaning and layout.
VR owns lifecycle.
TOAST stores bytes.
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


ordinary TOAST storage, v1
│
├── type-agnostic durable storage
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

The same boundary as a stack:

```text
┌─────────────────────────────────────────────────────────────┐
│ Type implementation                                         │
│                                                             │
│ jsonb, bytea, vector-array, sparse-array, tree/path, ...     │
│                                                             │
│ Owns:                                                       │
│   semantics                                                 │
│   read path                                                 │
│   representation policy                                     │
│   body format                                               │
│   version-specific interpretation                           │
└───────────────────────────────┬─────────────────────────────┘
                                │
                                │ lifecycle methods
                                v
┌─────────────────────────────────────────────────────────────┐
│ Value Representation                                         │
│                                                             │
│ Owns:                                                       │
│   persistent value-representation lifecycle                 │
│                                                             │
│ Methods:                                                    │
│   make / replace / rewrite / cleanup / validate / flatten   │
│                                                             │
│ Mechanical access:                                          │
│   vr_header_info / vr_body_size / vr_body_read              │
│                                                             │
│ Does not own:                                               │
│   jsonb keys, paths, vector slices, byte ranges             │
└───────────────────────────────┬─────────────────────────────┘
                                │
                                │ raw body storage
                                v
┌─────────────────────────────────────────────────────────────┐
│ ordinary TOAST storage, v1                                          │
│                                                             │
│ Owns:                                                       │
│   durable storage of opaque body bytes                      │
│   WAL / MVCC / vacuum / crash recovery / replication        │
│                                                             │
│ Does not own:                                               │
│   type semantics or body interpretation                     │
└─────────────────────────────────────────────────────────────┘
```

For `jsonb` specifically:

```text
jsonb logical value
│
├── jsonb semantics
│   ├── key lookup
│   ├── path lookup
│   ├── jsonpath
│   ├── containment
│   └── update rules
│
├── jsonb VR representation policy
│   ├── hot metadata stays near parent
│   ├── cold payload moves to body
│   ├── jsonb body directory maps keys/offsets
│   └── jsonb code interprets body layout
│
├── VR lifecycle
│   ├── make jsonb VR value
│   ├── replace old jsonb value
│   ├── rewrite during VACUUM FULL / CLUSTER
│   ├── cleanup old body
│   ├── validate body/header consistency
│   └── flatten to ordinary jsonb
│
└── ordinary TOAST storage
    ├── stores jsonb VR body bytes
    ├── reads body byte ranges
    ├── copies body to new relation
    └── knows nothing about jsonb keys or paths
```

## 3. How VR relates to TOAST

Strictly speaking, TOAST does not use VR as an upper-level API.

The direction is:

```text
VR uses TOAST as its v1 byte-storage storage layer.
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
ordinary TOAST storage, v1
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
  VR asks ordinary TOAST storage to store opaque body bytes
  storage layer returns a VR pointer/header Datum

read:
  type calls vr_body_read()
  v1 implementation fetches raw bytes from TOAST chunks
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
  VR pointer uses VR-aware ordinary TOAST storage delete path

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

TOAST stores the VR body.

TOAST/varlena lifecycle code must recognize VR pointers where ordinary
ONDISK assumptions would otherwise be wrong.

TOAST does not interpret VR bodies.
```



## 3a. Why previous ToastAPI attempts failed

The lesson from ToastAPI is that TOAST is the storage layer used by v1, not the abstraction.

Previous ToastAPI attempts failed not because the problem was artificial. They failed because the boundary was placed too low and too close to TOAST mechanics.

They tried to expose or customize TOAST behavior as a storage mechanism:

```text
how to store external chunks
how to detoast
how to copy external values
how to hook into TOAST lifecycle
how to let extensions provide toaster behavior
```

But the real problem is not "custom TOAST".

The real problem is:

```text
a PostgreSQL type wants a type-defined persistent physical representation
of one logical value, and PostgreSQL must preserve it across lifecycle paths.
```

That is a value-level problem, not a chunk-storage problem.

### Storage-first boundary

TOAST is a type-agnostic durability mechanism. It stores large varlena bytes safely. That is its strength.

Earlier ToastAPI designs started from this layer:

```text
varlena external storage
toast pointer
toast relation
chunk copy
detoast hooks
custom toaster provider
```

This made the API look like a way to replace or customize TOAST.

But the type-level need is different:

```text
jsonb does not need random custom chunk storage first.

jsonb needs PostgreSQL to preserve a jsonb-defined physical representation
of one logical jsonb value.
```

For example, `jsonb` may want hot metadata near the parent, cold payload outside, a body directory, a versioned body layout, type-owned interpretation of keys/paths, safe flatten fallback, safe rewrite/copy, safe update ownership and safe logical decoding.

Those are not ordinary TOAST semantics. They are representation-lifecycle semantics.

```text
wrong:
  how can an extension customize TOAST?

right:
  how can a type-defined physical representation survive PostgreSQL lifecycle?
```

### Mechanism and semantics were mixed

A TOAST-level API naturally wants to expose storage operations: save, fetch, copy, delete and custom detoast. But once this is exposed as a generic API, the next question becomes unclear: who owns meaning, partial read, reuse, validation, old versions and logical decoding conversion?

VR avoids this by splitting responsibilities:

```text
type:
  owns semantics and representation body format

VR:
  owns lifecycle methods

ordinary TOAST storage:
  stores opaque body bytes
```

There is no generic `jsonb key lookup` API in VR. There is no generic `read slice` API. There is no generic `update part` API. The type interprets the body. VR only makes the representation lifecycle-visible.

### Lifecycle obligations were not explicit enough

The hard failures were not just about reading data. They were about lifecycle:

```text
UPDATE
unchanged-attribute UPDATE
DELETE/VACUUM
VACUUM FULL
CLUSTER
ALTER TABLE rewrite
REPACK-like copy paths
logical decoding
abort
pg_upgrade
```

A custom TOAST mechanism can appear to work for ordinary reads and still be wrong after a rewrite or update.

The core problem is ownership:

```text
which tuple owns this external body?
can old and new tuple share it?
when is it safe to delete?
how is it copied into a new physical home?
what happens on abort?
what happens during logical decoding?
what if the new binary does not know this representation kind?
```

Earlier designs tended to expose hooks before the ownership graph was fully explicit. VR starts from the opposite side: first define lifecycle obligations; then define methods that satisfy them; then use ordinary TOAST storage as the v1 byte-storage layer.

```text
read path proves usefulness;
lifecycle proves correctness.
```

### Provider framing was too large for the first step

A general ToastAPI easily turns into a provider framework: handler, routine, catalog object, DDL, extension registration, provider lifetime, cross-version provider availability, pg_upgrade provider checks and logical decoding provider behavior.

That may be a future direction, but it is too much for the first safe patch. VR v1 deliberately avoids provider machinery:

```text
fixed in-core VrKind enum
static ValueRepresentationMethods
no handler
no DDL
no catalog object
no extension registration
ordinary TOAST storage
```

The first proof is not "arbitrary extension-defined storage". The first proof is:

```text
can core preserve a type-defined persistent representation correctly?
```

### TOAST is storage layer, not abstraction

If TOAST is the abstraction, questions become TOAST questions: custom toast relation, custom chunks, custom detoast, custom copy, custom delete.

If value representation is the abstraction, questions become lifecycle questions: how is the logical value preserved, how is representation rebuilt or copied, how is ownership preserved, how is old version read, how is unknown kind rejected, how is logical decoding made safe.

That is the shift from ToastAPI to VR.

### JSONB exposed the limit of type-agnostic TOAST

TOAST works well for values that are simply large. The problem appears when a value is structurally large. For `jsonb`, the type knows that not all bytes are equal: some fields are hot, some payloads are cold, some metadata is small and frequently read, some values are large and rarely read, and some updates logically touch only a part of the value.

TOAST sees none of that. It stores the flat serialized `jsonb` as bytes. `jsonb` does not primarily need a smarter generic TOAST. It needs its own persistent representation policy, with PostgreSQL lifecycle support.

```text
jsonb owns meaning and layout;
VR owns lifecycle;
TOAST stores bytes.
```

### Previous work was still useful

The old ToastAPI work identified the pressure points: varlena external tags, detoast dispatch, custom external value recognition, copy/rewrite paths, toast relation ownership, logical decoding hazards, update-time sharing and extension/provider temptation.

But those experiments also showed that exposing TOAST mechanics directly is not the clean architectural boundary.

```text
not custom TOAST,
but type-defined persistent value representation.
```

VR is not ToastAPI vNext. VR is the value-level lifecycle boundary that the ToastAPI attempts were implicitly looking for.
## 7. What this document describes

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

storage layer:
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

Such an implementation may pass local read tests and still be architecturally fragile. A value may be readable after a rewrite and still have lost its intended physical representation. Worse, an ordinary-looking value may secretly own external fragments that core lifecycle paths cannot see.

The invariant is:

```text
read path proves usefulness
lifecycle proves correctness
```

Value Representation exists to make type-defined persistent physical representation visible to the core lifecycle, without moving type semantics into core.

This is why the interface is intentionally not a generic read API. It does not define:

```text
lookup_key()
extract_part()
read_slice()
read_block()
get_range()
```

Those operations belong to the type.

At the same time, the type-owned read path must not be forced to flatten the whole value just to access its own representation body. Therefore v1 exposes mechanical body access:

```text
vr_header_info(stored_value)
vr_body_size(stored_value)
vr_body_read(stored_value, offset, len, buf)
```

These are not semantic read methods. The type computes offsets and interprets bytes. The storage layer returns raw bytes. There is no raw body write; all writes go through lifecycle methods because writes affect ownership.

Without this lifecycle interface, a type-specific representation is not a durable representation in the Postgres sense. It is only a hidden storage optimization that happens to work on the paths it has manually patched.

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
                      v1 ordinary TOAST storage relies on MVCC/vacuum

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

The important historical direction was:

```text
new data types
  bring their own operations
  interact with access methods
  may require different physical representations
  still live inside the database system
```

Large Objects are part of that history, but also show a cautionary boundary. They have persistent chunked storage, but they are not ordinary logical column values. Logical replication restrictions still expose that problem: a `pg_largeobject` chunk is not an independent logical value.

The useful historical framing is:

```text
Postgres narrowed physical representation choices for integrity and simplicity,
but did not later reconnect persistent value representation to the type system.
```

TOAST continued this choice successfully. It gave all varlena types generic compression, out-of-line storage, rewrite safety and portability. The cost is that the durable physical organization of a value remained mostly type-agnostic.

## 8. What Postgres already has

Modern Postgres already has many pieces of type extensibility.

At the logical level, a type can define:

```text
input/output
binary send/receive
operators
functions
casts
operator classes
statistics
```

At the in-memory representation level, Postgres has expanded datums. A type can use a richer in-memory representation for computation and then flatten it back to ordinary varlena form.

At the “part of a value” level, Postgres has type-defined subscripting. A type can define what it means to access a part of its value.

At the relation storage level, Postgres has Table AM.

The missing edge is:

```text
type-aware persistent physical representation of one value
with explicit lifecycle support
```

## 9. Value Representation and expanded datums

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

Expanded datums need methods such as `get_flat_size` and `flatten_into`. They do not need to survive heap rewrite, VACUUM FULL, CLUSTER, logical decoding, DELETE/VACUUM, ABORT or pg_upgrade.

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

`VR` must not store expanded datums on disk. Expanded datums may be built from VR bodies as a type-private runtime optimization, but the persistent VR body is a versioned byte stream owned by the type and managed by the VR lifecycle.

The useful formula is:

```text
Expanded datum proves that flat datum is not always the best working form.

VR extends that lesson to persistent physical form, but adds lifecycle.
```

## 10. Name and v1 form

The public/conceptual name is:

```text
Value Representation
```

The main vtable name for v1 is:

```c
ValueRepresentationMethods
```

`Methods` is deliberate. `Routine` in Postgres tends to suggest a handler-returned provider object and would pull the discussion toward handler, DDL, registration and framework design. `Methods` aligns better with `ExpandedObjectMethods`: an in-core static method table.

The analogy is:

```text
expanded datums:
  type-defined in-memory representation
  in-core methods

value representation v1:
  type-defined persistent physical representation
  in-core lifecycle methods
```

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

## 11. TOAST-backed, not TOAST-limited

v1 deliberately uses ordinary TOAST relations as the durability storage layer. This is a scope choice, not the definition of the abstraction.

TOAST is the existing durability storage layer for large varlena values. It already provides WAL, MVCC, vacuum, crash recovery, replication and relation lifetime. Reusing it in v1 keeps the proposal small and reviewable.

But Value Representation is not a new TOAST implementation.

The API boundary is above the storage layer:

```text
Value Representation:
  lifecycle of a type-defined physical representation of one logical value

v1 storage layer:
  ordinary TOAST relations

first proof:
  vr_test_vectors

first intended production client:
  jsonb cold-payload
```

The right reading is not:

```text
this API is only about TOAST
```

but:

```text
v1 proves Value Representation at the only existing value-level
durability storage layer Postgres already has: TOAST
```

Future storage layers may exist, but v1 does not define them. A custom external store would require new answers for WAL, MVCC, vacuum, crash recovery, replication, ownership and upgrade. That would turn a value-representation proposal into a storage-framework proposal.

## 12. Umbrella vs storage layer naming

The public-to-type layer must not expose TOAST names.

```text
public-to-type:
  ValueRepresentation*
  Vr*
  vr_header_info()
  vr_body_size()
  vr_body_read()

internal v1 storage layer:
  vr_toast_*
```

The type-visible API should not require including `vr_toast.h` just to read its own representation body. Type code sees `vr_body_read()`, not `toast_fetch_*`.

The v1 implementation may delegate internally to ordinary TOAST chunks.

## 13. Same storage layer, different value representations

TOAST provides the safe storage storage layer:

```text
WAL
MVCC
vacuum
crash recovery
replication
relation lifetime
```

Value Representation provides lifecycle support for type-specific physical form.

### JSONB cold-payload representation

```text
logical value:
  jsonb

type semantics:
  keys
  paths
  jsonpath
  subscripting
  jsonb_set
  jsonb-specific read helpers

physical representation:
  compact parent / metadata
  large cold values as external fragments
  optional dictionaries/directories

VR role:
  make / replace / rewrite / cleanup / flatten / validate

TOAST role:
  durable body/fragments storage layer
```

### Vector-array representation

```text
logical value:
  N fixed-width vectors of dimension D

type semantics:
  count
  dimension
  get vector
  slice vectors
  hash/check logical value

physical representation:
  small descriptor
  body-local header
  block directory
  vector blocks
  optional checksums

VR role:
  lifecycle of the persistent representation
```

### bytea block representation

```text
logical value:
  bytea

type semantics:
  byte offsets
  substring/range operations

physical representation:
  block sequence
  optional block directory
  tail append / block replacement

VR role:
  lifecycle of the representation
```

Generic TOAST can store chunks. It cannot know which chunks correspond to jsonb keys, vector blocks, byte ranges, array slices or tree nodes. It cannot decide which pieces are logically reusable after an update, or how a representation should be preserved across rewrite. That is the role of the type implementation plus Value Representation lifecycle.

## 14. Context-safe recognition and header access

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
  storage layer-private locator
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

`storage_oid`, `valueid` and other storage layer locators should remain storage-layer internal. Debug-only helpers may expose them for tests, but not through the public type-visible accessor.

## 15. Kind and versioning

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

## 16. Mechanical body access

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

## 17. Correctness rules

The physical representation must be lossless:

```text
flatten(physical_form) = logical_value
```

The physical representation must be recomputable:

```text
physical_form = f(logical_value, durable_representation_policy)
```

Hard invariants:

```text
logical value preserved
no dangling physical fragments
no duplicate ownership
abort-safe cleanup
rewrite-safe copy/rebuild/degrade
version understood or rejected
no accidental normalization as a side effect of maintenance
```

Exact byte-for-byte layout is not the invariant.

Better rule:

```text
preserve representation policy
copy or rebuild physical form where needed
allow explicit degrade only when policy permits it
preserve ownership always
```

## 18. Lifecycle methods

Draft shape:

```c
typedef struct ValueRepresentationMethods
{
    VrKind kind;
    uint8  write_version;

    Datum (*flatten)(Datum stored_value,
                     MemoryContext cxt);

    Datum (*make)(Relation rel,
                  AttrNumber attnum,
                  Datum logical_value,
                  const VrMakeContext *ctx);

    Datum (*replace)(Relation rel,
                     AttrNumber attnum,
                     Datum old_stored_value,
                     Datum new_logical_value,
                     const VrReplaceContext *ctx);

    Datum (*rewrite)(Datum old_stored_value,
                     const VrRewriteContext *ctx);

    void (*cleanup)(Datum stored_value,
                    VrCleanupReason reason,
                    const VrCleanupContext *ctx);

    bool (*validate)(Relation rel,
                     AttrNumber attnum,
                     Datum stored_value,
                     const VrValidateContext *ctx);
} ValueRepresentationMethods;
```

No methods like these belong here:

```text
lookup_key
extract_part
read_slice
read_block
get_range
get_vector
```

### flatten

`flatten` converts a VR-backed value to the ordinary logical datum. It is required for fallback paths, dump/logical paths, operators that do not understand the representation, validation and rebuild. The name mirrors expanded datums.

### make

`make` builds the initial persistent representation from a logical value. It may decline and return the ordinary flat value unchanged.

### replace

`replace` builds the new stored representation using the old stored value and the new logical value.

```text
old_stored_value:
  current stored value, possibly VR-backed

new_logical_value:
  requested new logical value
```

The type implementation owns semantic update logic. `replace` owns persistent representation lifecycle.

### rewrite

`rewrite` builds a stored datum suitable for the rewritten tuple in `ctx->new_rel`.

Contract:

```text
input:
  old_stored_value belongs to old physical home

output:
  returned Datum belongs to target physical home implied by ctx->new_rel
  and is safe to store in the rewritten tuple
```

The caller stores exactly the returned datum in the new tuple.

The returned datum may be:

```text
VR-backed:
  representation body copied or rebuilt in the target physical home

ordinary flat:
  explicit method decision to decline/degrade, if allowed

ERROR:
  if no safe result can be produced
```

v1 `rewrite` is per-tuple. It does not perform relation-level physical preserve/swap. Physical preserve/swap-by-content is future work and requires a separate relation-level lifecycle hook.

Suggested v1 flags:

```c
#define VR_REWRITE_ALLOW_REBUILD   0x0001  /* may flatten -> make */
#define VR_REWRITE_ALLOW_DEGRADE   0x0002  /* may return ordinary flat */
#define VR_REWRITE_VALIDATE        0x0004  /* validate during rewrite */
```

There is no `VR_REWRITE_ALLOW_PRESERVE` in v1 and no `VR_REWRITE_REQUIRE_REHOME`.

Reason:

```text
per-tuple rewrite can copy/rebuild/degrade;
relation-level preserve/swap requires a different hook;
after removing preserve, rehome is true for every v1 outcome.
```

### cleanup

Cleanup is value-based:

```c
typedef enum VrCleanupReason
{
    VR_CLEANUP_DELETE,
    VR_CLEANUP_ABORT
} VrCleanupReason;
```

If a specific storage layer needs a pending list, that remains storage-layer internal. Resources must be reachable from the value representation or governed by existing heap/TOAST MVCC cleanup.

### validate

`validate` is explicit check/maintenance validation. It is not a normal read hot-path method.

Use cases:

```text
debug/check function
rewrite/check path
amcheck-like validation
regression tests
optional defensive check when creating a representation
```

## 19. Test contrib: `vr_test_vectors`

The first proof should not be full `jsonb`. `jsonb` is the first compelling production type, but it is too rich as a first interface test.

A better first proof is:

```text
contrib/vr_test_vectors
```

SQL type:

```text
vr_vector_array
```

Purpose:

```text
test lifecycle, not performance
```

This is not:

```text
ANN search
pgvector replacement
tensor algebra
GPU/BLAS
SQL arrays
Table AM
generic partial-read API
```

Logical value:

```text
N fixed-width float4 vectors of dimension D
```

Flat logical representation:

```c
typedef struct VrTestVectorArrayFlat
{
    int32   vl_len_;
    int32   dim;
    int32   nvectors;
    float4  values[FLEXIBLE_ARRAY_MEMBER];
} VrTestVectorArrayFlat;
```

Persistent VR representation:

```text
small VR pointer/header
versioned body
body-local header
block directory
vector blocks
optional checksums
```

The block directory is private body format of `vr_vector_array`. It is not generic VR API.

Type-owned SQL functions may include:

```sql
vr_vector_array_make(nvectors int, dim int, seed int) returns vr_vector_array
vr_vector_array_count(vr_vector_array) returns int
vr_vector_array_dim(vr_vector_array) returns int
vr_vector_array_get(vr_vector_array, n int) returns float4[]
vr_vector_array_slice(vr_vector_array, first int, count int) returns vr_vector_array
vr_vector_array_hash(vr_vector_array) returns text
vr_vector_array_is_vr(vr_vector_array) returns bool
vr_vector_array_validate(vr_vector_array) returns bool
vr_vector_array_storage_info(vr_vector_array) returns text
```

These are type functions, not VR methods.

For `get`/`slice`:

```text
type reads its own body descriptor
type computes offsets
type calls vr_body_read()
type interprets bytes
```

Lifecycle tests should prove:

```text
small value may stay ordinary flat
large value becomes VR-backed
flatten reconstructs exact logical value
replace preserves logical value
VACUUM FULL rewrites safely
CLUSTER rewrites safely
ALTER TABLE rewrite rewrites safely
abort insert/update leaves no corrupt reachable body
delete/vacuum leaves no owned live representation reachable from no tuple
validate detects unknown version / missing body / bad directory / bad block
  range / missing block / checksum mismatch / logical-size mismatch
```

Critical acceptance rules:

```text
A. ValueRepresentationMethods contains lifecycle only.

B. vr_header_info is mechanical pointer/header access, not semantic read.

C. vr_body_read is storage layer raw-body access, not semantic read.

D. There is no raw write primitive.

E. The test type can implement get/slice/hash using its own representation
   without adding read methods to ValueRepresentationMethods.

F. jsonb later can use the same model:
     jsonb owns key/path/jsonpath/subscripting read semantics;
     VR owns lifecycle;
     vr_body_read only fetches raw representation bytes.

G. The API must not contain jsonb-specific, bytea-specific or vector-specific
   parameters.
```

## 20. SciDB, multidimensional arrays and the proof type

SciDB is a useful historical precedent because it treated multidimensional arrays as a native database model for scientific workloads. Value Representation does not try to turn Postgres into SciDB.

The lesson is narrower:

```text
large array-like values are important enough that their physical
representation should not always be an opaque varlena byte string
```

For Postgres, the right boundary is value-level:

```text
SciDB:
  array as database model and execution storage layer

Postgres Value Representation:
  array-like value as one logical datum
  with type-defined persistent physical representation
  and ordinary Postgres lifecycle
```

This supports a fixed-width vector-array or ndarray-like contrib type as a strong proof. It does not require VR to define array algebra, vector search, indexing or a new storage engine.

## 21. ltree, trees and graphs

`ltree` is a useful existing Postgres example for the boundary between type semantics and value representation. The `ltree` extension defines a type for label paths in hierarchical tree-like data. The type owns labels, paths, ancestor/descendant operators, pattern matching and index support.

This is exactly the separation VR should preserve:

```text
type implementation:
  knows what path, ancestor, descendant or pattern match means

Value Representation:
  only preserves the persistent physical form of one value across lifecycle paths
```

`ltree` is not the first VR demonstrator because typical values are small and the main benefit comes from operators and indexes. But it is a good conceptual example.

The same rule applies to graphs:

```text
graph-as-one-value:
  possible future VR use case

graph spread across rows/tables:
  Table AM / executor / index / SQL/PGQ territory
```

## 22. Per-value dictionaries

A per-value dictionary is a representation pattern.

Some large values contain repeated keys, labels, strings, codes or symbols. A type-defined physical representation may store those repeated atoms once and refer to them from the encoded body.

```text
logical value:
  unchanged type value

physical representation:
  dictionary / symbol table
  encoded body using dictionary ids
  optional blocks/directories

flatten:
  reconstructs the ordinary logical value
```

This is VR territory only when the dictionary belongs to one logical value.

```text
in scope:
  per-value dictionary

out of scope:
  dictionary shared across rows
  dictionary table
  cross-row columnar dictionary encoding
```

Rule:

```text
If the dictionary is part of one value's physical form, it is VR.
If it is shared across rows or tables, it is Table AM / catalog /
application schema territory.
```

## 23. Large Objects as cautionary precedent

Large Objects have attractive properties:

```text
persistent chunked representation
partial read/write API
large-value use case
physical storage outside the heap tuple
```

But they are not a model for VR. A Large Object is not an ordinary logical column value. It is addressed through an OID handle and stored through `pg_largeobject` chunks. A single physical chunk is not a portable logical value.

In VR terminology:

```text
Large Objects:
  persistent physical representation exists
  logical value as portability unit is missing

Value Representation:
  logical value remains the portability unit
  physical representation is below that value
```

VR does not fix the Large Object subsystem. VR prevents the Large Object failure mode from reappearing for new value representations.

A `bytea` value with VR-backed block representation could provide some LO-like benefits while remaining an ordinary logical column value:

```text
dump/restore:
  works through the logical bytea value

logical replication:
  sees an ordinary table column value

MVCC:
  stays attached to the row

schema:
  no OID handle
  no separate LO API
  no pg_largeobject dependency
```

## 24. Boundary and non-goals

### VOPS / packed tiles

VOPS shows demand for packed/vector-friendly representation, but cross-row columnar grouping is Table AM territory.

```text
value-level packing:
  in scope for VR

cross-row columnar grouping:
  Table AM territory
```

### PostGIS geometry/raster

PostGIS is strong ecosystem evidence that large structured values need specialized physical treatment. It is not the v1 commitfest demonstrator.

### pgvector

Sparse vectors may be a plausible value-level case because sparse representation can be lossless. Dense-vector quantization changes precision and belongs to the type system or an explicit conversion, not to storage representation for the same logical value. ANN search belongs to Index AM.

The vector-array test type is not a pgvector replacement. It is a simple fixed-width numeric value used to verify lifecycle mechanics.

### Index AM boundary

If the optimization answers “which rows match?”, it belongs to Index AM.

If the optimization answers “how do we read, update, rewrite or preserve this value once the row is available?”, it belongs to the type implementation plus VR lifecycle.



## 24a. Why VR is not a flag in varatt_external

A natural question is why VR needs a distinct external tag and `varatt_vr`, instead of a flag inside ordinary `varatt_external`.

```text
varatt_external means ordinary ONDISK TOAST pointer to flat logical varlena bytes.

varatt_vr means pointer to a type-defined representation body whose bytes are
not the flat logical value.
```

Those are different interfaces.

Ordinary `varatt_external` carries ordinary TOAST ONDISK semantics:

```text
external bytes are the flat varlena value
ordinary detoast can reconstruct the flat value by fetching chunks
ordinary TOAST copy/delete/share paths may interpret the pointer as
  varatt_external
```

VR needs a different header:

```text
VrKind:
  dispatch to type-owned representation methods

version:
  representation body format version within the kind

logical_size:
  size of the flattened logical value

body_size:
  size of the representation body byte stream

storage/value locator:
  private to the TOAST-backed VR storage helpers
```

For VR, the body bytes are not the flat logical value. They are a type-owned representation body. A `jsonb` VR body, for example, may contain directories, hot/cold layout metadata and cold payload bytes. Ordinary detoast must not fetch those bytes and return them as flat `jsonb`.

Reusing `varatt_external` plus a flag would blur two assumptions:

```text
ordinary ONDISK:
  external body is flat logical varlena bytes

VR:
  external body is type-defined representation bytes
```

That would let existing ONDISK paths believe they can detoast, copy, delete or compare the value under ordinary TOAST assumptions.

A distinct external tag is intentional. It forces explicit branches wherever the assumptions differ.

```text
VARATT_IS_EXTERNAL():
  true for VR

VARATT_IS_EXTERNAL_ONDISK():
  false for VR

VR-aware lifecycle paths:
  explicit branch, explicit ERROR, or proven-safe generic external handling
```

This keeps ordinary ONDISK semantics clean and prevents accidental `varatt_external` casts from becoming part of the VR API.


## 24b. v3.1.4 implementation clarifications

### Replace accepts flat or VR old value

`replace()` receives the previous stored datum and the new full logical datum.

```text
old_stored_value:
  ordinary flat or VR-backed

new_logical_value:
  full new logical value
```

If `old_stored_value` is ordinary flat, there is no previous representation body to reuse. v1 behavior is equivalent to `make(new_logical_value)`, except that the representation-policy decision still belongs to the method/type side.

The caller must not split policy as:

```text
old flat -> make()
old VR   -> replace()
```

because the flat/VR transition policy belongs to the type/VR method.

### Inline budget is advisory

`max_inline_size` / `inline_budget` is an advisory inline-budget hint from heap/TOAST sizing context. It is not a semantic threshold and not a hard VR limit. The method may use it, clamp it or ignore it. If the header name is still open, prefer `inline_budget` over `max_inline_size`.

### Persistent `vr_flags` are reserved-must-be-zero in v1

```text
v1 writers set vr_flags = 0.

Readers must ERROR on unknown persistent vr_flags bits.

validate must report unknown persistent flags.

Future flags may only be introduced together with explicit read/rewrite/
validate semantics and compatibility rules.
```

### WAL / redo

The representation body is WAL-logged as ordinary TOAST data.

The inline `varatt_vr` pointer/header is WAL-logged as part of the heap tuple, like any other varlena external pointer stored in a tuple.

Redo does not interpret `VrKind` or VR body semantics here. It replays heap tuple changes, TOAST relation changes and page records. Therefore v1 does not need special WAL/redo records for VR.

The path that must interpret VR at the logical level is logical decoding, not redo.

### Logical decoding and transient VR

The transient VR form is publisher/decode-side only.

It is used while logical decoding reconstructs the old value from decoded TOAST data and flattens it before the output plugin emits the logical change.

The subscriber receives ordinary logical values. If the subscriber stores the value into a VR-enabled column, representation is built again through `make()` on apply. The subscriber does not receive or store the transient VR form.

### `VARTAG_VR_INMEM` storage invariant

`VARTAG_VR_INMEM` is runtime-only. It must never reach heap storage, TOAST save, rewrite output, WAL as tuple data, or logical output as stored datum.

`varatt_vr` and `varatt_vr_inmem` may share a fixed header prefix: kind, version, flags, logical_size, body_size. Only this prefix is consumed by `vr_header_info()`. The locator/body part is tag-specific. Do not rely on full-structure layout identity.

### Physical equality is not logical equality

VR pointer byte equality is physical equality, not logical equality. The same logical value may be represented by a different `valueid` after rewrite, copy or rebuild. v1 accepts possible loss of HOT opportunities or extra WAL unless a specific path incorrectly assumes physical pointer equality as logical equality.

### Hot recognition predicates

Hot recognition predicates should be `static inline` where practical. This is an implementation style preference, not an architectural requirement.
## 25. Suggested patch decomposition

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

### Patch 0002: varlena/ordinary TOAST storage

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
small flat value
large VR-backed value
flatten hash
replace
unchanged-attribute UPDATE sharing
VACUUM FULL
CLUSTER
ALTER TABLE rewrite
abort insert
abort update
delete/vacuum
logical decoding safety
validate corruption cases
```

### Patch 0005: jsonb integration sketch

Only after lifecycle boundary is proven by the contrib.

## 26. Commitfest-facing story

```text
Motivation:
  type-defined persistent representation needs lifecycle support.

Ship first as proof:
  vr_test_vectors contrib.

Production target:
  jsonb.

Why jsonb:
  high-value structured demonstrator
  hot metadata / cold payload
  partial read and localized update owned by jsonb
  lifecycle owned by VR

Why vector-array contrib:
  simple fixed-width type
  non-jsonb proof
  lifecycle tests are clean
  no jsonpath, no index AM, no semantic API pressure

Why TOAST-backed:
  safest existing value-level durability storage layer

Argue generality:
  bytea, arrays, per-value dictionaries, tree-like values

Mark boundaries:
  Large Objects:
    cautionary precedent

  VOPS / columnar:
    Table AM boundary

  ANN/search:
    Index AM boundary

  SciDB:
    array model precedent, not scope

  ltree:
    type-owned tree semantics precedent, not first VR target
```

The important message:

```text
Value Representation is justified by lifecycle correctness for type-defined
persistent representation.

Performance comes later through type-owned read paths.
```

## 27. Final compact thesis

```text
Berkeley POSTGRES started from broad extensibility:
  data types, operators, access methods, complex objects and storage choices.

Modern Postgres preserved much of type extensibility:
  I/O, functions, operators, opclasses, statistics, expanded datums and
  subscripting.

Modern Postgres restored relation-level storage extensibility through Table AM.

The remaining value-level edge is persistent physical representation lifecycle:
  how a type-defined physical form of one logical value survives Postgres
  lifecycle.

The interface is Value Representation.

v1 uses in-core ValueRepresentationMethods:
  no handler
  no SQL DDL
  no new catalog object
  static tag/header -> VrKind -> methods dispatch

v1 is TOAST-backed but not TOAST-limited:
  TOAST is the first durability storage layer
  VR is the value-level lifecycle boundary

The vtable is lifecycle-only:
  flatten
  make
  replace
  rewrite
  cleanup
  validate

Mechanical access is separate:
  vr_header_info
  vr_body_size
  vr_body_read

Type semantics remain in the type:
  jsonb keys/paths
  vector slices
  byte ranges
  tree paths
  graph traversal

The first proof should be a simple contrib:
  vr_test_vectors / vr_vector_array

The first intended production client is jsonb cold-payload.

The guiding invariant:
  logical value remains the unit of portability
  physical representation is lossless, recomputable and lifecycle-safe
```


---



## Appendix B: VR-aware varlena/TOAST audit points

A path touching external varlena values must be classified as one of:

```text
ordinary ONDISK only and impossible to reach with VR
explicit VR branch
explicit ERROR for VR
safe generic external handling
```

`VARTAG_SIZE` default `Assert(false)` is useful but not sufficient coverage. It catches only paths that call `VARTAG_SIZE(tag)`.

It does not catch ONDISK-special paths that skip VR because `VARATT_IS_EXTERNAL_ONDISK` is false, or paths that assert in cassert builds and then read the datum as `varatt_external` in production builds.

The two dangerous classes are:

```text
silent skip:
  path uses VARATT_IS_EXTERNAL_ONDISK as a guard and returns if false.
  For VR this may silently skip cleanup/copy/share handling.

wrong-layout read:
  path asserts ONDISK in cassert builds but then reads the datum as
  varatt_external. In production builds this may misinterpret varatt_vr bytes.
```

Blocking audit points before any production-usable VR kind:

```text
1. external tag recognition and VARTAG_SIZE
2. detoast / flatten dispatch
3. vr_body_size() / vr_body_read()
4. heap_toast_insert_or_update ownership/share detection
5. delete/update cleanup of old external values
6. save/share paths and varatt_external layout assumptions
7. rewrite paths: VACUUM FULL, CLUSTER, ALTER TABLE rewrite, repack-like copy
8. logical decoding / ReorderBufferToastReplace / decode-local transient VR
9. pg_upgrade pre-flight scan for unknown VrKind
10. validation/check paths: missing body, bad sizes, unknown version/flags
11. transient reject paths: VARTAG_VR_INMEM must not reach storage
12. tuple equality / HOT behavior: VR pointer equality is physical equality
```

Concrete red-team examples:

```text
toast_delete_datum:
  if guarded only by !VARATT_IS_EXTERNAL_ONDISK(attr), VR may be silently
  skipped and its body leaked.

toast_save_datum:
  if it assumes oldexternal is varatt_external, production builds may read
  varatt_vr bytes under the wrong layout.

detoast_external_attr / detoast_attr:
  VR must not fall through into a path that copies external pointer bytes as if
  they were flat varlena.

logical decoding:
  VR must be reconstructed from decoded TOAST data as a decode-local transient
  value and flattened before output.

heaptoast paths through detoast_attr:
  become safe only after the detoast funnel is VR-aware.
```


# Appendix A: Value Representation v1 header sketch

This appendix is a design skeleton, not a commit-ready header.

It fixes the v1 boundary:

```text
ValueRepresentationMethods:
  lifecycle only

Mechanical access:
  vr_header_info()
  vr_body_size()
  vr_body_read()

Type implementation:
  owns semantics and read path

v1 rewrite:
  per-tuple copy / rebuild / explicit degrade

Future:
  relation-level preserve/swap hook, separate from v1
```

There is no generic semantic read API, no raw write API, no handler, no DDL, no catalog object and no extension-level provider registration.

```c
/*-------------------------------------------------------------------------
 *
 * value_representation.h
 *    Type-Aware Persistent Value Representation (VR) — in-core interface.
 *
 * DRAFT / DESIGN SKELETON — for review, not for commit.
 *
 * VR lets selected built-in types define a persistent, type-aware physical
 * representation of a single logical value, while the logical value remains
 * the unit of portability: dump/restore, logical replication, fallback
 * flattening.
 *
 * This is the persistent counterpart of expanded datums:
 *
 *   expanded datums:
 *     type-defined in-memory representation, flattened before storage.
 *
 *   Value Representation:
 *     type-defined persistent physical representation, flattenable back
 *     to the logical value and managed across Postgres lifecycle paths.
 *
 * v1 is in-core only:
 *
 *   - no handler
 *   - no SQL DDL
 *   - no new catalog object
 *   - no extension-level provider registration
 *
 * Dispatch is datum/header based and context-safe:
 *
 *   datum tag/header -> VrKind -> static ValueRepresentationMethods
 *
 * The first storage layer is ordinary TOAST relations, but the public-to-type
 * VR boundary deliberately does not expose TOAST names.
 *
 * Core boundary:
 *
 *   ValueRepresentationMethods:
 *     lifecycle only
 *
 *   Mechanical access:
 *     vr_header_info()
 *     vr_body_size()
 *     vr_body_read()
 *
 *   Type implementation:
 *     owns semantics and read path
 *
 * There are no generic semantic read methods:
 *
 *   - no lookup_key()
 *   - no extract_part()
 *   - no read_slice()
 *   - no read_block()
 *   - no get_range()
 *
 * There is no raw write primitive. Writes affect ownership and must go
 * through make / replace / rewrite.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VALUE_REPRESENTATION_H
#define VALUE_REPRESENTATION_H

#include "access/attnum.h"
#include "postgres.h"
#include "utils/memutils.h"
#include "utils/relcache.h"

/*
 * Kind and version
 *
 * VrKind selects which in-core representation implementation owns the value.
 * It is stored in the VR pointer/header and indexes a static methods table.
 *
 * Values are on-disk-stable. Append only; never reuse a value.
 *
 * Version is separate from kind. Each kind owns its own physical format
 * version sequence. Kind chooses the interpreter. Version chooses the body
 * format generation inside that interpreter.
 */
typedef enum VrKind
{
    VR_KIND_INVALID          = 0,

    /*
     * Production target.
     *
     * jsonb owns JSON semantics: keys, paths, jsonpath, subscripting,
     * jsonb_set and any jsonb-specific read helpers.
     *
     * VR owns only lifecycle of the persistent representation.
     */
    VR_KIND_JSONB_COLD       = 1,

    /*
     * Test/proof contrib.
     *
     * A fixed-width vector-array value used to verify lifecycle mechanics
     * without jsonb semantics.
     */
    VR_KIND_TEST_VECTORS     = 2,

    /*
     * Possible later demonstrator.
     *
     * bytea owns byte offsets/ranges. VR owns only lifecycle of the
     * persistent representation.
     */
    VR_KIND_BYTEA_BLOCK      = 3,

    /* append only; values are permanent */
    VR_KIND__COUNT
} VrKind;

/*
 * On-disk pointer/header sketch.
 *
 * Final home is varatt.h, alongside varatt_external. Adding a VR external
 * tag touches VARTAG_SIZE(), VARSIZE_EXTERNAL(), VARATT_IS_EXTERNAL dispatch
 * and external-datum walkers. Numeric tag assignment is intentionally not
 * fixed in this draft.
 *
 * The pointer/header is not the representation body.
 *
 *   pointer/header coordinates:
 *     kind, version, persistent pointer flags, flattened logical size,
 *     storage layer-private locator
 *
 *   body coordinates:
 *     representation body byte stream addressed by vr_body_read()
 *
 * The public header accessor exposes kind/version/flags/logical_size only.
 * It does not expose storage OIDs or value IDs to type code.
 */
typedef struct varatt_vr
{
    uint8   vr_kind;          /* VrKind */
    uint8   vr_version;       /* format version within this kind */
    uint16  vr_flags;         /* persistent on-disk VR pointer flags */

    /*
     * Size of the ordinary flattened logical value.
     *
     * This is not the body size.
     */
    int32   vr_logical_size;

    /*
     * Size of the representation body byte stream exposed by vr_body_read().
     *
     * This is not necessarily the number of physical TOAST bytes in any future
     * storage layer. In v1, the body is stored as an opaque TOAST-uncompressed
     * stream from the storage layer perspective.
     */
    int32   vr_body_size;

    /*
     * v1 storage layer-private locator.
     *
     * These fields are not exposed through vr_header_info().
     */
    Oid     vr_storage_oid;
    Oid     vr_valueid;
} varatt_vr;

/*
 * Mechanical pointer/header accessor.
 *
 * This is catalog-free and locator-free.
 *
 * It reads only the inline VR pointer/header and returns false for non-VR
 * values. It does not expose storage_oid/valueid or any other storage layer
 * locator. It does not define type semantics.
 *
 * Note the distinction:
 *
 *   VrHeaderInfo.flags:
 *     persistent on-disk VR pointer flags
 *
 *   VrRewriteContext.flags:
 *     per-call rewrite policy flags
 */
typedef struct VrHeaderInfo
{
    VrKind  kind;
    uint8   version;
    uint16  flags;          /* persistent on-disk pointer flags */
    Size    logical_size;   /* flattened logical size, not body size */
} VrHeaderInfo;

extern bool vr_header_info(Datum stored_value, VrHeaderInfo *out);

/*
 * Transient in-memory VR body form.
 *
 * Logical decoding may construct a runtime-only VR datum whose body is an
 * in-memory byte buffer captured from the decoded stream. This is not a
 * persistent vr_flags bit and must never be stored in heap or TOAST.
 *
 * vr_header_info() and vr_body_read() work on both persistent on-disk VR
 * values and transient in-memory VR values.
 *
 * make / replace / rewrite / save paths must reject transient VR values.
 */
extern bool vr_is_transient(Datum stored_value);

/*
 * Mechanical representation body access.
 *
 * These functions are not ValueRepresentationMethods methods.
 * They are not a semantic read API.
 *
 * Coordinate space:
 *
 *   offset/len address the representation BODY byte stream,
 *   not the flattened logical value.
 *
 *   body coordinates != logical coordinates
 *   vr_body_size() != VrHeaderInfo.logical_size
 *
 * The type owns the body layout, computes offsets and interprets bytes.
 * The storage layer only returns raw bytes.
 *
 * The body is presented as one contiguous byte stream. The storage layer hides
 * physical chunking. In v1, this delegates to ordinary TOAST chunk storage.
 *
 * Reads must lie within:
 *
 *   [0, vr_body_size(stored_value))
 *
 * buf is caller-allocated and at least len bytes.
 *
 * There is intentionally no raw write primitive.
 */
extern Size vr_body_size(Datum stored_value);

extern void vr_body_read(Datum stored_value,
                         Size offset,
                         Size len,
                         void *buf);

/*
 * Lifecycle contexts.
 *
 * These contexts are purpose-specific and small. They should not become a
 * generic storage-provider API.
 */

typedef struct VrMakeContext
{
    MemoryContext   mcxt;

    /*
     * Heap-side budget / representation policy input.
     *
     * Exact field names and contents are draft-only. v1 should keep this
     * minimal and avoid a new catalog object.
     */
    Size            max_inline_size;
} VrMakeContext;

typedef struct VrReplaceContext
{
    MemoryContext   mcxt;
    Size            max_inline_size;
} VrReplaceContext;

/*
 * Per-tuple rewrite policy flags.
 *
 * These are not persistent on-disk flags.
 *
 * There is no VR_REWRITE_ALLOW_PRESERVE in v1.
 *
 * Reason:
 *
 *   v1 rewrite() is called per tuple during rewrite/copy.
 *   It can copy, rebuild or explicitly degrade the value.
 *   It cannot perform relation-level physical preserve/swap.
 *
 * A future relation-level preserve/swap hook is a separate lifecycle hook,
 * not part of v1 ValueRepresentationMethods.
 */
#define VR_REWRITE_ALLOW_REBUILD   0x0001  /* may flatten -> make */
#define VR_REWRITE_ALLOW_DEGRADE   0x0002  /* may return ordinary flat */
#define VR_REWRITE_VALIDATE        0x0004  /* validate during rewrite */

typedef struct VrRewriteContext
{
    Relation        old_rel;    /* source relation being rewritten */
    Relation        new_rel;    /* target relation / new physical home */
    AttrNumber      attnum;

    uint32          flags;      /* per-call VR_REWRITE_* policy flags */

    MemoryContext   mcxt;
} VrRewriteContext;

typedef enum VrCleanupReason
{
    VR_CLEANUP_DELETE,          /* owning tuple deleted / vacuumed */
    VR_CLEANUP_ABORT            /* creating/updating transaction aborted */
} VrCleanupReason;

typedef struct VrCleanupContext
{
    MemoryContext   mcxt;
} VrCleanupContext;

typedef struct VrValidateContext
{
    int             elevel;

    void            (*report)(void *arg, const char *detail);
    void            *report_arg;

    MemoryContext   mcxt;
} VrValidateContext;

/*
 * ValueRepresentationMethods
 *
 * Lifecycle-only vtable.
 *
 * This vtable is the type -> core lifecycle boundary for persistent physical
 * representation. It is not a generic read interface.
 *
 * Type-owned read paths may use vr_header_info(), vr_body_size() and
 * vr_body_read() to access their own representation body, but all semantic
 * interpretation remains in the type implementation.
 */
typedef struct ValueRepresentationMethods
{
    VrKind  kind;

    /*
     * Physical format version written by this implementation.
     *
     * This is not "max supported version". Older supported read versions are
     * internal to flatten/validate/rewrite for this kind.
     */
    uint8   write_version;

    /*
     * flatten:
     *
     * Convert a VR-backed stored value into the ordinary flat logical datum.
     *
     * Required for fallback paths, dump/logical paths, operators that do not
     * understand the representation, validation and rebuild.
     */
    Datum (*flatten)(Datum stored_value,
                     MemoryContext cxt);

    /*
     * make:
     *
     * Build initial persistent representation from a logical value.
     *
     * May decline and return the ordinary value unchanged, for example for
     * small values.
     */
    Datum (*make)(Relation rel,
                  AttrNumber attnum,
                  Datum logical_value,
                  const VrMakeContext *ctx);

    /*
     * replace:
     *
     * Build the new stored representation using the old stored value and
     * the new logical value.
     *
     * old_stored_value:
     *   current stored value, possibly VR-backed.
     *
     * new_logical_value:
     *   requested new logical value.
     *
     * Type-level update semantics remain in the type. This method owns only
     * persistent representation lifecycle.
     */
    Datum (*replace)(Relation rel,
                     AttrNumber attnum,
                     Datum old_stored_value,
                     Datum new_logical_value,
                     const VrReplaceContext *ctx);

    /*
     * rewrite:
     *
     * Build a stored datum suitable for the rewritten tuple in ctx->new_rel.
     *
     * Input:
     *   old_stored_value belongs to the old physical home.
     *
     * Output:
     *   returned Datum belongs to the target physical home implied by
     *   ctx->new_rel and is safe to store in the rewritten tuple.
     *
     * The caller stores exactly the returned Datum in the new tuple.
     *
     * The returned Datum may be:
     *
     *   - VR-backed:
     *       representation body copied or rebuilt in the target physical home.
     *
     *   - ordinary flat:
     *       explicit method decision to degrade/decline, only if allowed.
     *
     *   - ERROR:
     *       if no safe result can be produced.
     *
     * The caller must not depend on whether the method copied or rebuilt the
     * representation body. Those are storage layer decisions.
     *
     * v1 rewrite is per-tuple. It does not perform relation-level physical
     * preserve/swap. Relation-level preserve/swap is future work and requires
     * a separate lifecycle hook.
     */
    Datum (*rewrite)(Datum old_stored_value,
                     const VrRewriteContext *ctx);

    /*
     * cleanup:
     *
     * Per-value cleanup / ownership release.
     *
     * v1 may mostly rely on ordinary TOAST MVCC and relation cleanup, but the
     * lifecycle obligation is explicit.
     */
    void (*cleanup)(Datum stored_value,
                    VrCleanupReason reason,
                    const VrCleanupContext *ctx);

    /*
     * validate:
     *
     * Explicit check/maintenance validation.
     *
     * Not a normal read hot-path method.
     */
    bool (*validate)(Relation rel,
                     AttrNumber attnum,
                     Datum stored_value,
                     const VrValidateContext *ctx);
} ValueRepresentationMethods;

/*
 * Static dispatch.
 *
 * No handler, no DDL, no catalog object, no extension registration.
 */
extern const ValueRepresentationMethods *vr_lookup_methods(VrKind kind);

#endif /* VALUE_REPRESENTATION_H */
```

## Appendix A.1: storage-layer internal helpers

Type code must not include `vr_toast.h` or call `toast_save_datum`.

The ordinary TOAST storage may expose internal helpers such as:

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

## Appendix A.2: boundary checklist

```text
In v1:

  ValueRepresentationMethods is lifecycle-only.

  vr_header_info() is mechanical pointer/header access.

  vr_body_size() and vr_body_read() are mechanical body access.

  Type code owns semantics and read path.

  There is no raw write.

  There is no generic partial-read method.

  There is no relation-level preserve/swap in v1 rewrite.

  rewrite() is per-tuple:
    copy / rebuild / explicit degrade / ERROR.

  relation-level preserve/swap is future work and needs a separate hook.

  VR is always external.

  VR has a distinct external tag.

  VR is not globally folded into VARATT_IS_EXTERNAL_ONDISK.

  update-time ownership/share detection must account for VR external bodies.

  transient in-memory VR form is runtime-only and invalid for heap storage.

  unknown kind is hard ERROR.

  pg_upgrade pre-flight must reject unknown on-disk kinds before production use.
```

## Appendix A.3: contrib proof target

The first proof contrib should be:

```text
contrib/vr_test_vectors
```

SQL type:

```text
vr_vector_array
```

Logical value:

```text
N fixed-width float4 vectors of dimension D
```

The type may implement:

```text
vr_vector_array_get()
vr_vector_array_slice()
vr_vector_array_hash()
vr_vector_array_validate()
vr_vector_array_storage_info()
```

These are type functions, not VR methods.

They may use:

```text
vr_header_info()
vr_body_size()
vr_body_read()
```

but they must not add semantic read methods to `ValueRepresentationMethods`.

The contrib should prove lifecycle:

```text
small value may stay flat;
large value becomes VR-backed;
flatten reconstructs exact logical value;
replace preserves logical value;
VACUUM FULL rewrites safely;
CLUSTER rewrites safely;
ALTER TABLE rewrite rewrites safely;
abort insert/update does not leave corrupt reachable body;
delete/vacuum does not leave owned live representation reachable from no tuple;
validate detects bad version/body/directory/block/checksum/logical-size mismatch.
```
