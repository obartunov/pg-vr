# src/test/modules/vr_jsonb_cold_smoke/Makefile

MODULE_big = vr_jsonb_cold_smoke
OBJS = \
	$(WIN32RES) \
	vr_jsonb_cold_smoke.o
PGFILEDESC = "vr_jsonb_cold_smoke - first real in-core VR user (jsonb cold) smoke"

EXTENSION = vr_jsonb_cold_smoke
DATA = vr_jsonb_cold_smoke--1.0.sql

REGRESS = vr_jsonb_cold_smoke

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/vr_jsonb_cold_smoke
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
