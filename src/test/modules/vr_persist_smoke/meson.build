# Copyright (c) 2022-2026, PostgreSQL Global Development Group

vr_persist_smoke_sources = files(
  'vr_persist_smoke.c',
)

if host_system == 'windows'
  vr_persist_smoke_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'vr_persist_smoke',
    '--FILEDESC', 'vr_persist_smoke - VR persistence backend v0 smoke',])
endif

vr_persist_smoke = shared_module('vr_persist_smoke',
  vr_persist_smoke_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += vr_persist_smoke

test_install_data += files(
  'vr_persist_smoke.control',
  'vr_persist_smoke--1.0.sql',
)

tests += {
  'name': 'vr_persist_smoke',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'vr_persist_smoke',
    ],
  },
}
