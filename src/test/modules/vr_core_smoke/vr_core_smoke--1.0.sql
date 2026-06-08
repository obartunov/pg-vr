/* src/test/modules/vr_core_smoke/vr_core_smoke--1.0.sql */

\echo Use "CREATE EXTENSION vr_core_smoke" to load this file. \quit

CREATE FUNCTION vr_core_roundtrip(payload bytea)
RETURNS text
AS 'MODULE_PATHNAME', 'vr_core_roundtrip'
LANGUAGE C STRICT;

CREATE FUNCTION vr_core_badkind()
RETURNS void
AS 'MODULE_PATHNAME', 'vr_core_badkind'
LANGUAGE C;

CREATE FUNCTION vr_core_badflags()
RETURNS void
AS 'MODULE_PATHNAME', 'vr_core_badflags'
LANGUAGE C;

CREATE FUNCTION vr_core_badmethod()
RETURNS void
AS 'MODULE_PATHNAME', 'vr_core_badmethod'
LANGUAGE C;

CREATE FUNCTION vr_core_badregister()
RETURNS void
AS 'MODULE_PATHNAME', 'vr_core_badregister'
LANGUAGE C;

CREATE FUNCTION vr_core_dupregister()
RETURNS void
AS 'MODULE_PATHNAME', 'vr_core_dupregister'
LANGUAGE C;

CREATE FUNCTION vr_core_isvr(payload bytea)
RETURNS bool
AS 'MODULE_PATHNAME', 'vr_core_isvr'
LANGUAGE C STRICT;
