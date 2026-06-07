CREATE EXTENSION vr_persist_smoke;

CREATE TABLE t (id int, b bytea);
-- size-driven externalization, no compression (deterministic)
ALTER TABLE t ALTER COLUMN b SET STORAGE EXTERNAL;
SELECT vr_persist_arm('t');

-- big value (>= MIN_BODY) is born as persistent VARTAG_VR;
-- mid value is externalized but make() declines -> ordinary ONDISK TOAST.
INSERT INTO t VALUES (1, decode(repeat('ab', 5000), 'hex'));  -- 5000 bytes -> VR
INSERT INTO t VALUES (2, decode(repeat('cd', 3000), 'hex'));  -- 3000 bytes -> ONDISK

-- heap really stores VARTAG_VR for the big row, ONDISK for the declined row
SELECT vr_persist_probe('t');

-- SELECT/flatten returns the original bytes through the VR method contract
SELECT id, b = decode(repeat('ab', 5000), 'hex') AS rt FROM t WHERE id = 1;
SELECT id, b = decode(repeat('cd', 3000), 'hex') AS rt FROM t WHERE id = 2;

-- UPDATE the VR column to a new large value: old body reclaimed, new is VR
UPDATE t SET b = decode(repeat('ef', 6000), 'hex') WHERE id = 1;
SELECT b = decode(repeat('ef', 6000), 'hex') AS rt FROM t WHERE id = 1;
SELECT vr_persist_probe('t');

-- DELETE + VACUUM leaves no orphaned VR body in TOAST
DELETE FROM t;
VACUUM t;
SELECT vr_persist_toast_count('t') AS toast_chunks_after_cleanup;

-- rewrite / cross-relation behaviour (explicit ERROR or proven safe)
INSERT INTO t VALUES (3, decode(repeat('ab', 5000), 'hex'));
SELECT vr_persist_probe('t');
VACUUM FULL t;
SELECT vr_persist_probe('t');
CREATE TABLE t2 (id int, b bytea);
INSERT INTO t2 SELECT id, b FROM t;
SELECT b = decode(repeat('ab', 5000), 'hex') AS rt FROM t2 WHERE id = 3;

SELECT vr_persist_disarm();
DROP TABLE t, t2;
DROP EXTENSION vr_persist_smoke;
