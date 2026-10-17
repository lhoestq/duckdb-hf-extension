-- Bucket behaviour through the hf extension. Run with scripts/smoke_buckets.sh, which puts the fixtures in a
-- staging dataset repository first. The two statements carrying an `expect-fail` marker are the direct bucket
-- write paths, which need a Xet client to compute a chunk hash - try.py counts those as expected failures.
SELECT 'clean slate' AS t, count(*) = 0 AS assert_nothing_to_clean
FROM hf_delete('hf://buckets/lhoestq/playground/duckdb', missing_ok := true);
SELECT 'copy one file onto an exact name' AS t,
       path = 'duckdb/copied.parquet' AS assert_path,
       size > 0 AS assert_size,
       status = 'copied' AS assert_copied,
       xet_hash <> '' AS assert_hash_is_shown
FROM hf_bucket_copy('hf://datasets/lhoestq/hf-smoke-staging/data/one.parquet',
                    'hf://buckets/lhoestq/playground/duckdb/copied.parquet', hash_wait := 90);
SELECT 'the copy reads back' AS t, count(*) = 2 AS assert_rows, sum(answer) = 85 AS assert_values
FROM read_parquet('hf://buckets/lhoestq/playground/duckdb/copied.parquet');
SELECT 'copy a folder, structure mirrored under it' AS t,
       count(*) = 3 AS assert_one_row_per_source,
       sum(CASE WHEN path = 'duckdb/mirrored/one.parquet' THEN 1 ELSE 0 END) = 1 AS assert_relative_to_the_source,
       sum(CASE WHEN path LIKE '%.gitattributes' THEN 1 ELSE 0 END) = 0 AS assert_no_git_bookkeeping,
       sum(CASE WHEN status = 'copied' THEN 1 ELSE 0 END) = 2 AS assert_two_copied
FROM hf_bucket_copy('hf://datasets/lhoestq/hf-smoke-staging/data', 'hf://buckets/lhoestq/playground/duckdb/mirrored/');
SELECT 'a file the hub keeps outside Xet is skipped, not an error' AS t,
       count(*) = 1 AS assert_one_row,
       max(status) LIKE 'skipped%' AS assert_skipped,
       max(status) LIKE '%Xet%' AS assert_says_why
FROM hf_bucket_copy('hf://datasets/lhoestq/hf-smoke-staging/data/small.csv',
                    'hf://buckets/lhoestq/playground/duckdb/never/');
SELECT 'the mirrored copies read back' AS t, count(DISTINCT filename) = 2 AS assert_two_files,
       count(*) = 3 AS assert_three_rows, sum(answer) = 92 AS assert_values
FROM read_parquet('hf://buckets/lhoestq/playground/duckdb/mirrored/*.parquet', filename := true);
SELECT 'copy through a glob' AS t, count(*) = 2 AS assert_two_parquet_files, max(status) = 'copied' AS assert_copied
FROM hf_bucket_copy('hf://datasets/lhoestq/hf-smoke-staging/data/*.parquet',
                    'hf://buckets/lhoestq/playground/duckdb/globbed/');
SELECT 'a destination without a slash is a directory when one lives there' AS t,
       path = 'duckdb/globbed/two.parquet' AS assert_landed_inside
FROM hf_bucket_copy('hf://datasets/lhoestq/hf-smoke-staging/data/two.parquet',
                    'hf://buckets/lhoestq/playground/duckdb/globbed');
SELECT 'a bucket can copy from another bucket' AS t,
       status = 'copied' AS assert_copied,
       path = 'duckdb/second-copy.parquet' AS assert_path
FROM hf_bucket_copy('hf://buckets/lhoestq/playground/duckdb/copied.parquet',
                    'hf://buckets/lhoestq/playground/duckdb/second-copy.parquet');
SELECT 'that copy reads back' AS t, sum(answer) = 85 AS assert_values
FROM read_parquet('hf://buckets/lhoestq/playground/duckdb/second-copy.parquet');
SELECT 'everything landed in the bucket' AS t, count(*) = 6 AS assert_six_files
FROM hf_ls('hf://buckets/lhoestq/playground/duckdb', recursive := true);
SELECT 'copies are visible without any reload' AS t, count(DISTINCT filename) = 6 AS assert_six_parquet,
       sum(answer) = 354 AS assert_values
FROM read_parquet('hf://buckets/lhoestq/playground/duckdb/**/*.parquet', filename := true);
SELECT 'deleting a copy works' AS t, count(*) = 1 AS assert_deleted
FROM hf_delete('hf://buckets/lhoestq/playground/duckdb/second-copy.parquet');
SELECT 'and it is gone' AS t, exists = false AS assert_gone FROM hf_meta('hf://buckets/lhoestq/playground/duckdb/second-copy.parquet');
-- expected failures: direct byte writes into a bucket need a Xet xetHash, see the note at the top
-- expect-fail: is not supported
COPY (SELECT 1 i) TO 'hf://buckets/lhoestq/playground/duckdb/nope.parquet';
-- expect-fail: is not supported
SELECT * FROM hf_upload('/tmp/hf_bucket_probe.csv', 'hf://buckets/lhoestq/playground/duckdb/uploaded.csv');
SELECT 'cleanup' AS t, count(*) >= 1 AS assert_deleted FROM hf_delete('hf://buckets/lhoestq/playground/duckdb');
SELECT 'nothing left behind' AS t, count(*) = 0 AS assert_empty
FROM hf_ls('hf://buckets/lhoestq/playground', recursive := true) WHERE path LIKE 'duckdb%';

-- the hub decides a file's xetHash after the commit returns, so a copy of content written seconds ago is
-- a race the caller has to be allowed to wait out; refusing to wait is a bug, waiting forever is one too
SELECT 'hash_wait is a bounded knob' AS t,
       count(*) = 1 AS assert_copied_with_a_short_wait
FROM hf_bucket_copy('hf://datasets/lhoestq/hf-smoke-staging/data/two.parquet',
                    'hf://buckets/lhoestq/playground/duckdb/waited.parquet', hash_wait := 0);
-- expect-fail: non-negative
SELECT * FROM hf_bucket_copy('hf://datasets/lhoestq/hf-smoke-staging/data/two.parquet',
                             'hf://buckets/lhoestq/playground/duckdb/nope.parquet', hash_wait := -1);
SELECT 'the wait test cleans up after itself' AS t, count(*) >= 1 AS assert_deleted
FROM hf_delete('hf://buckets/lhoestq/playground/duckdb', missing_ok := true);
