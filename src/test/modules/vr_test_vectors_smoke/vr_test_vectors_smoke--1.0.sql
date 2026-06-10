/* src/test/modules/vr_test_vectors_smoke/vr_test_vectors_smoke--1.0.sql */
\echo Use "CREATE EXTENSION vr_test_vectors_smoke" to load this file. \quit

CREATE FUNCTION vr_tv_arm(rel regclass) RETURNS void
AS 'MODULE_PATHNAME', 'vr_tv_arm' LANGUAGE C STRICT;

CREATE FUNCTION vr_tv_disarm() RETURNS void
AS 'MODULE_PATHNAME', 'vr_tv_disarm' LANGUAGE C;

CREATE FUNCTION vr_tv_make(rel regclass, payload bytea) RETURNS bytea
AS 'MODULE_PATHNAME', 'vr_tv_make' LANGUAGE C STRICT;

CREATE FUNCTION vr_tv_probe(rel regclass) RETURNS text
AS 'MODULE_PATHNAME', 'vr_tv_probe' LANGUAGE C STRICT;

CREATE FUNCTION vr_tv_toast_chunks(rel regclass) RETURNS bigint
AS 'MODULE_PATHNAME', 'vr_tv_toast_chunks' LANGUAGE C STRICT;
