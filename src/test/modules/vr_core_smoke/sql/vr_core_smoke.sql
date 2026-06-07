CREATE EXTENSION vr_core_smoke;

-- construct -> recognize -> header -> size invariants -> flatten -> compare
SELECT vr_core_roundtrip('\xdeadbeef0011223344556677'::bytea);
SELECT vr_core_roundtrip('\x2a'::bytea);
SELECT vr_core_roundtrip(''::bytea);
SELECT vr_core_roundtrip(decode(repeat('ab', 2000), 'hex'));
-- binary-safe body: all 256 byte values 0x00..0xff
SELECT vr_core_roundtrip(decode(string_agg(lpad(to_hex(g), 2, '0'), '' ORDER BY g), 'hex'))
  FROM generate_series(0, 255) g;

-- recognition must not false-positive on an ordinary varlena
SELECT vr_core_isvr('\xdeadbeef'::bytea) AS ordinary_is_vr;

-- error boundaries
SELECT vr_core_badkind();      -- unknown / unregistered kind
SELECT vr_core_badflags();     -- unknown persistent flag bit
SELECT vr_core_badregister();  -- invalid-kind registration must be rejected
SELECT vr_core_dupregister();  -- duplicate registration must be rejected

-- the rejected wrong-kind registration must not corrupt the slot
SELECT vr_core_roundtrip('\x99'::bytea);

DROP EXTENSION vr_core_smoke;
