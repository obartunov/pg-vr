# Frida runtime evidence — VR representation-dispatch (methods->flatten)

Runtime evidence attached to the VR lifecycle graph. This is evidence/tooling,
not mandatory CI, and not a new mechanism.

- Tree used: `6583c64d` (this series' 0002 tip), `-O0` postgres.
- Capture: Frida (`Interceptor.attach`) on a live backend; the calling seam is
  recovered from the return address. Runs where bpftrace/perf/Intel-PT are
  unavailable.

## Indirect provider dispatch through `methods->flatten`

Both lifecycle seams reach the *same* provider method (`vr_jsonb_cold_flatten`,
the `VR_KIND_JSONB_COLD` flattener) through the policy methods table. A static
call graph cannot resolve `methods->flatten(value, cxt)` (vr.c:597, detoast.c);
Frida resolves and attributes it at runtime:

| seam                          | edge | recovered edge                                   | count |
|-------------------------------|------|--------------------------------------------------|-------|
| read / detoast                | E3   | `vr_detoast_flatten -> vr_jsonb_cold_flatten`    | 3     |
| capture / decode              | E20  | `vr_capture_logical_value -> vr_jsonb_cold_flatten` | 3  |

Entry edges into the seams: `detoast_attr -> vr_detoast_flatten`,
`ExecInterpExpr -> vr_capture_logical_value`.

## Rewrite cross-check (corroborates VR_GRAPH_COMPILED_AUDIT_V0)

Frida reproduces the audit's gdb counts and names the dual call-site behind the
`vr_rewrite_action = 24`:

| function                         | count | call-site attribution                                   |
|----------------------------------|-------|---------------------------------------------------------|
| `vr_rewrite_action`              | 24    | `toast_tuple_init` x12 + `heap_toast_insert_or_update` x12 |
| `vr_toast_body_copy_to_relation` | 12    | `toast_tuple_init` x12                                   |
| `raw_heap_insert`                | 9     | `rewrite_heap_tuple` x9 (VACUUM FULL/CLUSTER/REPACK; ALTER excluded) |

## Interpretation (scope)

- Confirms read/detoast flatten and capture/decode flatten reach the same
  provider method through different lifecycle seams (E3 and E20 via the P1
  policy methods table).
- Supports E20 scoping around reorderbuffer/capture and provider flatten
  dispatch, but does **not** make E20 pass — E20 remains `partial`.
- Does not touch E25.
- Evidence/tooling only; no Frida CI requirement.
