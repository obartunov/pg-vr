CREATE EXTENSION vr_jsonb_cold_smoke;

CREATE TABLE jc (id int, j jsonb);
SELECT vr_jsonb_cold_arm('jc');

-- A large jsonb (> 4096 logical bytes) so the in-core selector opts it into
-- VR_KIND_JSONB_COLD; the value is born as a persistent VARTAG_VR.
INSERT INTO jc
  VALUES (1, (SELECT jsonb_object_agg('k' || g, g) FROM generate_series(1, 2000) g));

-- stored as VR, homed in this relation's own TOAST
SELECT vr_jsonb_cold_probe('jc');

-- byte-identical: the VR-backed value equals a freshly built identical jsonb
SELECT j = (SELECT jsonb_object_agg('k' || g, g) FROM generate_series(1, 2000) g)
         AS roundtrip
  FROM jc WHERE id = 1;

-- ordinary jsonb operators work on the VR-backed value (flatten feeds them)
SELECT (j ->> 'k1')::int AS k1, (j ->> 'k2000')::int AS k2000 FROM jc WHERE id = 1;
SELECT jsonb_typeof(j) AS typ, (j ? 'k1') AS has_k1 FROM jc WHERE id = 1;

-- survives a heap rewrite: the real kind rehomes like any VR
VACUUM FULL jc;
SELECT 'after VACUUM FULL' AS step, vr_jsonb_cold_probe('jc');
SELECT j = (SELECT jsonb_object_agg('k' || g, g) FROM generate_series(1, 2000) g)
         AS roundtrip
  FROM jc WHERE id = 1;
SELECT (j ->> 'k1')::int AS k1 FROM jc WHERE id = 1;

-- ---- shared logical-value capture seam (vr_capture_logical_value) ----

-- capture of the stored VR value is byte-identical to the ordinary read
-- (the threshold is a PGC_SIGHUP bound; its refusal/recovery behaviour is
-- exercised in t/003_capture_limit.pl, which can reload the configuration)
SELECT vr_jsonb_cold_capture(j) = j AS capture_fidelity FROM jc WHERE id = 1;

SELECT vr_jsonb_cold_disarm();
DROP TABLE jc;
DROP EXTENSION vr_jsonb_cold_smoke;
