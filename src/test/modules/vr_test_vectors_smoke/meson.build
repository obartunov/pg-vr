# Copyright (c) 2022-2026, PostgreSQL Global Development Group

vr_test_vectors_smoke_sources = files(
  'vr_test_vectors_smoke.c',
)

if host_system == 'windows'
  vr_test_vectors_smoke_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'vr_test_vectors_smoke',
    '--FILEDESC', 'vr_test_vectors_smoke - substrate-free simple VR kind smoke',])
endif

vr_test_vectors_smoke = shared_module('vr_test_vectors_smoke',
  vr_test_vectors_smoke_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += vr_test_vectors_smoke

test_install_data += files(
  'vr_test_vectors_smoke.control',
  'vr_test_vectors_smoke--1.0.sql',
)

tests += {
  'name': 'vr_test_vectors_smoke',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'vr_test_vectors_smoke',
    ],
  },
}
