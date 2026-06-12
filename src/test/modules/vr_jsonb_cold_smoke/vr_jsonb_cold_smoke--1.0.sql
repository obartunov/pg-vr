/* src/test/modules/vr_jsonb_cold_smoke/vr_jsonb_cold_smoke--1.0.sql */
\echo Use "CREATE EXTENSION vr_jsonb_cold_smoke" to load this file. \quit

CREATE FUNCTION vr_jsonb_cold_arm(rel regclass) RETURNS void
AS 'MODULE_PATHNAME', 'vr_jsonb_cold_arm' LANGUAGE C STRICT;

CREATE FUNCTION vr_jsonb_cold_disarm() RETURNS void
AS 'MODULE_PATHNAME', 'vr_jsonb_cold_disarm' LANGUAGE C;

CREATE FUNCTION vr_jsonb_cold_probe(rel regclass) RETURNS text
AS 'MODULE_PATHNAME', 'vr_jsonb_cold_probe' LANGUAGE C STRICT;

CREATE FUNCTION vr_jsonb_cold_capture(j jsonb) RETURNS jsonb
AS 'MODULE_PATHNAME', 'vr_jsonb_cold_capture' LANGUAGE C STRICT;

CREATE FUNCTION vr_jsonb_cold_forge_class_s(regclass, regclass, int, jsonb)
RETURNS oid AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION vr_jsonb_cold_forge_inmem_store(regclass, int)
RETURNS void AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION vr_jsonb_cold_forge_stray_chunk(regclass, oid, int, int)
RETURNS void AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION vr_jsonb_cold_forge_truncate_chunks(regclass, oid)
RETURNS void AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
