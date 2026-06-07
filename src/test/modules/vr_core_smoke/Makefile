# src/test/modules/vr_core_smoke/Makefile

MODULE_big = vr_core_smoke
OBJS = \
	$(WIN32RES) \
	vr_core_smoke.o
PGFILEDESC = "vr_core_smoke - storage-independent VR representation-contract smoke"

EXTENSION = vr_core_smoke
DATA = vr_core_smoke--1.0.sql

REGRESS = vr_core_smoke

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/vr_core_smoke
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
