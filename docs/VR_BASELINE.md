# VR baseline

Repository:

```text
obartunov/pg-vr
```

Branch:

```text
value-representation-v0
```

Base:

```text
upstream PostgreSQL master
89eafad297a9b01ad77cfc1ab93a433e0af894b0
```

Scope:

```text
Type-Aware Persistent Value Representation v0.

This branch starts from clean upstream PostgreSQL master.

It must not depend on:
  jbtl-public-clean
  w3-hybrid-v0
  jsonb_toaster_lite
  old W2/W3 branches

v1 scope:
  in-core only
  fixed VrKind enum
  no handler
  no DDL
  no catalog object
  no extension registration
  external-only VR
  ordinary TOAST substrate
  lifecycle-only ValueRepresentationMethods
  mechanical vr_header_info / vr_body_size / vr_body_read
  no semantic read API
  no raw write
  no relation-level preserve/swap
  no inline VR
  no non-transactional own-GC substrate
```
