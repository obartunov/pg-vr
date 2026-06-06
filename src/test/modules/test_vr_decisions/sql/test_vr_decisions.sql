CREATE EXTENSION test_vr_decisions;

-- Dispatch decision: number of defined VrKind values (VR_KIND__COUNT).
SELECT vr_decision_kind_count();

-- Dispatch decision: in v1 no kind is implemented, so every kind -- including
-- VR_KIND_INVALID (0) and out-of-range values -- resolves to NULL methods,
-- i.e. not implemented.  This locks the recognition-before-construction
-- invariant: nothing can yet construct a persistent VR datum.
SELECT k, vr_decision_kind_implemented(k) AS implemented
FROM generate_series(-1, vr_decision_kind_count()) AS k
ORDER BY k;

-- Recognition decision: an ordinary value is never recognized as a VR datum
-- (short, long, and ordinary varlena alike), because nothing constructs one.
SELECT vr_decision_recognize('\x'::bytea)                  AS empty,
       vr_decision_recognize('\xdeadbeef'::bytea)          AS short_bytes,
       vr_decision_recognize(decode(repeat('ab', 4000), 'hex')) AS large_value;

DROP EXTENSION test_vr_decisions;
