# Copyright (c) 2022-2026, PostgreSQL Global Development Group

vr_core_smoke_sources = files(
  'vr_core_smoke.c',
)

if host_system == 'windows'
  vr_core_smoke_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'vr_core_smoke',
    '--FILEDESC', 'vr_core_smoke - storage-independent VR representation-contract smoke',])
endif

vr_core_smoke = shared_module('vr_core_smoke',
  vr_core_smoke_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += vr_core_smoke

test_install_data += files(
  'vr_core_smoke.control',
  'vr_core_smoke--1.0.sql',
)

tests += {
  'name': 'vr_core_smoke',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'vr_core_smoke',
    ],
  },
}
