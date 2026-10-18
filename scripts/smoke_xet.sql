-- Xet hashing and chunking, checked against reference vectors generated from xet-core.
--
-- Every row of hf_xet_selftest() is a promise: assert_passed has to be true for all of them. This
-- suite needs no token, no network and no fixtures - the vectors are compiled into the extension -
-- so it runs anywhere the extension loads, which is the point: a port that only verifies on the
-- machine that generated its vectors verifies nothing.
--
-- See docs/XET.md for what is being reproduced, and scripts/xet_oracle.py for where the vectors come
-- from. Regenerate them when xet-core moves; scripts/xet_oracle.py --check says whether this has
-- drifted.

-- What a failure looks like when there is one: expected vs got, by name. Deliberately empty
-- otherwise; the aggregate statements below are what grade the run, since a query returning no rows
-- would otherwise grade nothing.
SELECT check_name, expected, got FROM hf_xet_selftest() WHERE NOT assert_passed ORDER BY check_name;

-- A floor rather than an exact count, so adding vectors is never a reason to edit this file. The
-- breakdown is: leaf/file/xorb hashes per input vector, chunk boundaries per input, mean-tree
-- vectors from xet-core's own correctness table, interior node hashes, hex/base64 renderings, the
-- serialized xorb objects the reference writer produces, and the session metadata shards that make
-- those xorbs reachable.
SELECT count(*) >= 360 AS assert_vectors_run,
       count(*) FILTER (WHERE NOT assert_passed) = 0 AS assert_nothing_failed,
       count(*) FILTER (WHERE check_name LIKE '%chunk boundaries%') >= 8 AS assert_chunking_covered,
       count(*) FILTER (WHERE check_name LIKE '%file hash%') >= 40 AS assert_tree_covered,
       -- every xorb we would upload, pinned byte for byte against the reference writer's output
       count(*) FILTER (WHERE check_name LIKE '%xorb bytes%') >= 7 AS assert_xorb_serialization_covered,
       -- whole uploaded shards, byte for byte, including ones with several segments and xorbs
       count(*) FILTER (WHERE check_name LIKE '%shard bytes%') >= 7 AS assert_shard_serialization_covered,
       count(*) FILTER (WHERE check_name LIKE '%shard segments%') >= 7 AS assert_shard_segments_covered
FROM hf_xet_selftest();

-- the self-test is pure computation, so it must be safe to call repeatedly, and must coexist with
-- the rest of the extension's surface
SELECT count(*) = (SELECT count(*) FROM hf_xet_selftest()) AS assert_repeatable,
       count(*) FILTER (WHERE NOT assert_passed) = 0 AS assert_still_clean
FROM hf_xet_selftest();

SELECT (SELECT count(*) FROM hf_xet_selftest() WHERE NOT assert_passed) = 0 AS assert_still_clean,
       (SELECT hf_version FROM hf_status() LIMIT 1) <> '' AS assert_version_reported;
