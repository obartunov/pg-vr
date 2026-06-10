# src/test/modules/vr_test_vectors_smoke/Makefile

MODULE_big = vr_test_vectors_smoke
OBJS = \
	$(WIN32RES) \
	vr_test_vectors_smoke.o
PGFILEDESC = "vr_test_vectors_smoke - substrate-free simple VR kind smoke"

EXTENSION = vr_test_vectors_smoke
DATA = vr_test_vectors_smoke--1.0.sql

REGRESS = vr_test_vectors_smoke

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/vr_test_vectors_smoke
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
