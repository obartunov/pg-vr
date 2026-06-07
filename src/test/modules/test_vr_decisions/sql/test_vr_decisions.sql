CREATE EXTENSION test_vr_decisions;

-- Dispatch decision: number of defined VrKind values (VR_KIND__COUNT).
SELECT vr_decision_kind_count();

-- Dispatch decision: VR_KIND_JSONB_COLD (1) is the first in-core implemented
-- kind, so it resolves to non-NULL methods; VR_KIND_INVALID (0), out-of-range
-- values, and the not-yet-implemented kinds resolve to NULL methods.  A kind
-- being implemented does not by itself construct anything: construction still
-- requires a selector to opt a value in, preserving recognition-before-
-- construction.
SELECT k, vr_decision_kind_implemented(k) AS implemented
FROM generate_series(-1, vr_decision_kind_count()) AS k
ORDER BY k;

-- Recognition decision: an ordinary value is never recognized as a VR datum
-- (short, long, and ordinary varlena alike), because nothing constructs one.
SELECT vr_decision_recognize('\x'::bytea)                  AS empty,
       vr_decision_recognize('\xdeadbeef'::bytea)          AS short_bytes,
       vr_decision_recognize(decode(repeat('ab', 4000), 'hex')) AS large_value;

DROP EXTENSION test_vr_decisions;
