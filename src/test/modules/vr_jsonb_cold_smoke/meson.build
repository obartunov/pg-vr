# Copyright (c) 2022-2026, PostgreSQL Global Development Group

vr_jsonb_cold_smoke_sources = files(
  'vr_jsonb_cold_smoke.c',
)

if host_system == 'windows'
  vr_jsonb_cold_smoke_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'vr_jsonb_cold_smoke',
    '--FILEDESC', 'vr_jsonb_cold_smoke - first real in-core VR user (jsonb cold) smoke',])
endif

vr_jsonb_cold_smoke = shared_module('vr_jsonb_cold_smoke',
  vr_jsonb_cold_smoke_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += vr_jsonb_cold_smoke

test_install_data += files(
  'vr_jsonb_cold_smoke.control',
  'vr_jsonb_cold_smoke--1.0.sql',
)

tests += {
  'name': 'vr_jsonb_cold_smoke',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'vr_jsonb_cold_smoke',
    ],
  },
}
