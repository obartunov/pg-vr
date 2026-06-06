# Copyright (c) 2022-2026, PostgreSQL Global Development Group

test_vr_decisions_sources = files(
  'test_vr_decisions.c',
)

if host_system == 'windows'
  test_vr_decisions_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_vr_decisions',
    '--FILEDESC', 'test_vr_decisions - decision probes for Value Representation',])
endif

test_vr_decisions = shared_module('test_vr_decisions',
  test_vr_decisions_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_vr_decisions

test_install_data += files(
  'test_vr_decisions.control',
  'test_vr_decisions--1.0.sql',
)

tests += {
  'name': 'test_vr_decisions',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_vr_decisions',
    ],
  },
}
