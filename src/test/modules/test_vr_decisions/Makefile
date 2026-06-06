# src/test/modules/test_vr_decisions/Makefile

MODULE_big = test_vr_decisions
OBJS = \
	$(WIN32RES) \
	test_vr_decisions.o
PGFILEDESC = "test_vr_decisions - decision probes for Value Representation"

EXTENSION = test_vr_decisions
DATA = test_vr_decisions--1.0.sql

REGRESS = test_vr_decisions

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_vr_decisions
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
