# contrib/vr_admin/Makefile

MODULE_big = vr_admin
OBJS = \
	$(WIN32RES) \
	vr_admin.o
PGFILEDESC = "vr_admin - admin inspection and re-representation for in-core VR"

EXTENSION = vr_admin
DATA = vr_admin--1.0.sql


TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/vr_admin
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
