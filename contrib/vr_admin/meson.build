# Copyright (c) 2022-2026, PostgreSQL Global Development Group

vr_admin_sources = files(
  'vr_admin.c',
)

if host_system == 'windows'
  vr_admin_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'vr_admin',
    '--FILEDESC', 'vr_admin - admin inspection and re-representation for in-core VR',])
endif

vr_admin = shared_module('vr_admin',
  vr_admin_sources,
  c_pch: pch_postgres_h,
  kwargs: contrib_mod_args,
)
contrib_targets += vr_admin

install_data(
  'vr_admin--1.0.sql',
  'vr_admin.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'vr_admin',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_vr_inspect.pl',
    ],
  },
}
