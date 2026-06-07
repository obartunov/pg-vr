/* src/test/modules/vr_persist_smoke/vr_persist_smoke--1.0.sql */
\echo Use "CREATE EXTENSION vr_persist_smoke" to load this file. \quit

CREATE FUNCTION vr_persist_arm(rel regclass) RETURNS void
AS 'MODULE_PATHNAME', 'vr_persist_arm' LANGUAGE C STRICT;

CREATE FUNCTION vr_persist_disarm() RETURNS void
AS 'MODULE_PATHNAME', 'vr_persist_disarm' LANGUAGE C;

CREATE FUNCTION vr_persist_probe(rel regclass) RETURNS text
AS 'MODULE_PATHNAME', 'vr_persist_probe' LANGUAGE C STRICT;

CREATE FUNCTION vr_persist_toast_count(rel regclass) RETURNS bigint
AS 'MODULE_PATHNAME', 'vr_persist_toast_count' LANGUAGE C STRICT;
