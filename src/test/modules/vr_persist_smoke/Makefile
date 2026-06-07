# src/test/modules/vr_persist_smoke/Makefile

MODULE_big = vr_persist_smoke
OBJS = \
	$(WIN32RES) \
	vr_persist_smoke.o
PGFILEDESC = "vr_persist_smoke - VR persistence backend v0 smoke"

EXTENSION = vr_persist_smoke
DATA = vr_persist_smoke--1.0.sql

REGRESS = vr_persist_smoke

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/vr_persist_smoke
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
