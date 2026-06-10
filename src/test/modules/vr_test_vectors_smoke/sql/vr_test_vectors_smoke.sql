CREATE EXTENSION vr_test_vectors_smoke;

-- Substrate-free simple kind: store small inline VR values built through the
-- sanctioned core seam vr_make_inline(); prove round-trip + heap rewrite with
-- no external/TOAST body.
CREATE TABLE tv (id int, b bytea);

INSERT INTO tv VALUES
  (1, vr_tv_make('tv', '\x01020304')),
  (2, vr_tv_make('tv', '\xdeadbeefcafef00d')),   -- 8 bytes = inline capacity
  (3, vr_tv_make('tv', '\x'));                    -- 0 bytes (empty payload)

-- Value fidelity (flatten via ordinary bytea output).
SELECT id, b FROM tv ORDER BY id;

-- Physical representation: every row is an inline VR; no external body exists.
SELECT vr_tv_probe('tv');
SELECT vr_tv_toast_chunks('tv') AS toast_chunks;

-- Heap rewrite #1: VACUUM FULL must preserve value AND inline representation,
-- and create no TOAST body.
VACUUM FULL tv;
SELECT id, b FROM tv ORDER BY id;
SELECT vr_tv_probe('tv');
SELECT vr_tv_toast_chunks('tv') AS toast_chunks;

-- Heap rewrite #2: CLUSTER (second rewrite path).
CREATE INDEX tv_id ON tv(id);
CLUSTER tv USING tv_id;
SELECT id, b FROM tv ORDER BY id;
SELECT vr_tv_probe('tv');
SELECT vr_tv_toast_chunks('tv') AS toast_chunks;

-- UPDATE boundary: an inline VR must not be silently flattened/degraded.
-- (a) replace the inline VR column with a different inline VR value;
UPDATE tv SET b = vr_tv_make('tv', '\xaabbccdd') WHERE id = 1;
-- (b) preserve the inline VR column unchanged (b = b);
UPDATE tv SET b = b WHERE id = 2;
-- (c) update a different column, leaving the inline VR column untouched.
UPDATE tv SET id = id WHERE id = 3;
SELECT id, b FROM tv ORDER BY id;
SELECT vr_tv_probe('tv');                          -- still all inline VR
SELECT vr_tv_toast_chunks('tv') AS toast_chunks;   -- 0

-- DELETE a row holding an inline VR: no out-of-line body to reclaim, no error,
-- no orphan.
DELETE FROM tv WHERE id = 3;
SELECT vr_tv_probe('tv');                          -- rows=2, all inline VR
SELECT vr_tv_toast_chunks('tv') AS toast_chunks;   -- 0

-- Heap rewrite after UPDATE/DELETE still preserves value + inline representation.
VACUUM FULL tv;
SELECT id, b FROM tv ORDER BY id;
SELECT vr_tv_probe('tv');
SELECT vr_tv_toast_chunks('tv') AS toast_chunks;

-- Deterministic ERROR: payload larger than the inline capacity (no unsafe
-- access; refused at construction).
SELECT vr_tv_make('tv', '\x0102030405060708090a');  -- 10 bytes > 8

-- Selector + make() decline path: a large bytea is opted into TEST_VECTORS but
-- declines (exceeds inline capacity) and falls back to ordinary TOAST.
CREATE TABLE tv_big (id int, b bytea);
ALTER TABLE tv_big ALTER COLUMN b SET STORAGE EXTERNAL;
SELECT vr_tv_arm('tv_big');
INSERT INTO tv_big VALUES (1, repeat('x', 6000)::bytea);
SELECT vr_tv_disarm();
SELECT vr_tv_probe('tv_big');                        -- vr=0; ondisk=1 (TOAST)
SELECT vr_tv_toast_chunks('tv_big') > 0 AS has_external_body;
SELECT length(b) AS big_len FROM tv_big;             -- value still correct

DROP TABLE tv;
DROP TABLE tv_big;
DROP EXTENSION vr_test_vectors_smoke;
