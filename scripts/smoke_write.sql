-- Write-side smoke test against the scratch repositories. Run with scripts/smoke_write.sh, which builds the
-- local fixtures this file uploads. Everything it writes is deleted again, and the last three statements
-- check that nothing stayed behind.
COPY (SELECT * FROM range(5) t(i)) TO 'hf://datasets/lhoestq/playground/duckdb/w.parquet' (FORMAT PARQUET);
SELECT 'r1 parquet round trip' AS t, count(*) = 5 AS assert_rows, sum(i) = 10 AS assert_values
FROM 'hf://datasets/lhoestq/playground/duckdb/w.parquet';
COPY (SELECT 1 AS a, 'x' AS b) TO 'hf://datasets/lhoestq/playground/duckdb/w.csv' (FORMAT CSV);
SELECT 'r2 csv' AS t, count(*) = 1 AS assert_rows, max(b) = 'x' AS assert_text
FROM 'hf://datasets/lhoestq/playground/duckdb/w.csv';
COPY (SELECT * FROM range(3) t(i)) TO 'hf://datasets/lhoestq/playground/duckdb/w.json' (FORMAT JSON, ARRAY true);
SELECT 'r3 json' AS t, count(*) = 3 AS assert_rows FROM read_json('hf://datasets/lhoestq/playground/duckdb/w.json');
COPY (SELECT * FROM range(9) t(i)) TO 'hf://datasets/lhoestq/playground/duckdb/w.parquet' (FORMAT PARQUET);
SELECT 'r4 overwrite replaces the file' AS t, count(*) = 9 AS assert_new_row_count, sum(i) = 36 AS assert_new_values
FROM 'hf://datasets/lhoestq/playground/duckdb/w.parquet';
COPY (SELECT 1 i WHERE false) TO 'hf://datasets/lhoestq/playground/duckdb/empty.parquet' (FORMAT PARQUET);
SELECT 'r5 an empty result still writes a readable file' AS t, count(*) = 0 AS assert_no_rows
FROM 'hf://datasets/lhoestq/playground/duckdb/empty.parquet';
COPY (SELECT i, i % 3 AS g FROM range(9) t(i)) TO 'hf://datasets/lhoestq/playground/duckdb/wpart'
  (FORMAT PARQUET, PARTITION_BY (g));
SELECT 'r6 partitioned output' AS t, count(*) = 9 AS assert_rows, count(DISTINCT g) = 3 AS assert_three_partitions,
       count(DISTINCT filename) = 3 AS assert_three_files
FROM read_parquet('hf://datasets/lhoestq/playground/duckdb/wpart/**/*.parquet', filename := true);
COPY (SELECT 1 AS a) TO 'hf://datasets/lhoestq/playground/duckdb/wéird name.parquet' (FORMAT PARQUET);
SELECT 'r7 spaces and unicode in the path' AS t, count(*) = 1 AS assert_row
FROM 'hf://datasets/lhoestq/playground/duckdb/wéird name.parquet';
COPY (SELECT 42 AS answer) TO 'hf://models/lhoestq/playground/duckdb/m.parquet' (FORMAT PARQUET);
SELECT 'r8 a model repo takes the same write' AS t, answer = 42 AS assert_value
FROM 'hf://models/lhoestq/playground/duckdb/m.parquet';
COPY (SELECT * FROM range(100) t(i)) TO 'hf://datasets/lhoestq/playground/duckdb/zstd.parquet'
  (FORMAT PARQUET, COMPRESSION zstd);
SELECT 'r10 zstd parquet' AS t, count(*) = 100 AS assert_rows FROM 'hf://datasets/lhoestq/playground/duckdb/zstd.parquet';

-- refusals: nothing is created, and the message says why
-- expect-fail: Repository not found
COPY (SELECT 1 i) TO 'hf://datasets/lhoestq/definitely-not-a-repo-xyz/a.parquet';
-- expect-fail: Authorization error
COPY (SELECT 1 i) TO 'hf://datasets/wikimedia/wikipedia/nope.parquet';
-- expect-fail: Writing to Hugging Face buckets is not supported
COPY (SELECT 1 i) TO 'hf://buckets/lhoestq/playground/nope.parquet';
-- expect-fail: hf_create_repo
SELECT * FROM hf_upload('/tmp/hf_write_fixture.bin', 'hf://datasets/lhoestq/definitely-not-a-repo-xyz/b.parquet');

-- create-on-write, which is the same write with permission to bootstrap the repository. Datasets, models and
-- spaces all take it; scripts/smoke_write.sh deletes the three probe repositories again.
SET hf_create_repo = true;
COPY (SELECT 'bootstrapped' AS a) TO 'hf://datasets/lhoestq/hf-ext-write-probe/x.parquet' (FORMAT PARQUET);
SELECT 'w1 a dataset repository appeared with the file' AS t, a = 'bootstrapped' AS assert_value
FROM 'hf://datasets/lhoestq/hf-ext-write-probe/x.parquet';
COPY (SELECT 'bootstrapped too' AS a) TO 'hf://models/lhoestq/hf-ext-write-probe/y.parquet' (FORMAT PARQUET);
SELECT 'w2 the same for a model repository' AS t, a = 'bootstrapped too' AS assert_value
FROM 'hf://models/lhoestq/hf-ext-write-probe/y.parquet';
COPY (SELECT 1 AS a) TO 'hf://spaces/lhoestq/hf-ext-write-probe/s.parquet' (FORMAT PARQUET);
SELECT 'w3 and for a space repository' AS t, a = 1 AS assert_value
FROM 'hf://spaces/lhoestq/hf-ext-write-probe/s.parquet';
RESET hf_create_repo;

-- hf_upload: a 20 MB blob goes over git LFS, which the hub serves as basic multipart above ~10 MB
SELECT 'i1 a 20MB blob uploads through LFS' AS t, size = 20000000 AS assert_size
FROM hf_upload('/tmp/hf_write_fixture.bin', 'hf://datasets/lhoestq/playground/duckdb/big.bin');
SELECT 'i2 and reads back byte identical' AS t,
       (SELECT md5(content) FROM read_blob('hf://datasets/lhoestq/playground/duckdb/big.bin')) =
       (SELECT md5(content) FROM read_blob('/tmp/hf_write_fixture.bin')) AS assert_same_bytes,
       (SELECT size FROM hf_meta('hf://datasets/lhoestq/playground/duckdb/big.bin')) = 20000000 AS assert_size;
SELECT 'i3 a per-call commit message' AS t, url LIKE '%/big2.bin' AS assert_url
FROM hf_upload('/tmp/hf_write_fixture.bin', 'hf://datasets/lhoestq/playground/duckdb/big2.bin',
               message := 'duckdb smoke: hf_upload with message');
SELECT 'i4 the message reached the hub as the commit title' AS t, count(*) = 1 AS assert_found
FROM hf_list_revisions('hf://datasets/lhoestq/playground', max_revisions := 5)
WHERE title = 'duckdb smoke: hf_upload with message';
SELECT 'i5 hf_status reads' AS t, endpoint LIKE 'https://%' AS assert_endpoint, user <> '' AS assert_user,
       whoami_error = '' AS assert_no_whoami_error, token_source <> 'none' AS assert_token,
       hf_version = '0.0.2-pre1' AS assert_version
FROM hf_status();

-- the read cache has to notice what the write changed
SELECT 'c0 reset the counters' AS t, count(*) = 1 AS ok FROM hf_read_stats(reset := true);
COPY (SELECT 'first' AS a) TO 'hf://datasets/lhoestq/playground/duckdb/stale.parquet' (FORMAT PARQUET);
SELECT 'c1 read what was just written' AS t, a = 'first' AS assert_first
FROM 'hf://datasets/lhoestq/playground/duckdb/stale.parquet';
COPY (SELECT 'second' AS a) TO 'hf://datasets/lhoestq/playground/duckdb/stale.parquet' (FORMAT PARQUET);
SELECT 'c2 the overwrite is visible immediately' AS t, a = 'second' AS assert_second
FROM 'hf://datasets/lhoestq/playground/duckdb/stale.parquet';
SELECT 'c3 because the commit invalidated the cached windows' AS t, invalidations > 0 AS assert_invalidated
FROM hf_read_stats();

SELECT 'i6 cleanup' AS t, count(*) = 2 AS assert_two_deleted
FROM hf_delete(['hf://datasets/lhoestq/playground/duckdb/big.bin', 'hf://datasets/lhoestq/playground/duckdb/big2.bin']);
SELECT 'd0 missing_ok makes cleanup idempotent' AS t, count(*) = 0 AS assert_nothing_reported
FROM hf_delete('hf://datasets/lhoestq/playground/duckdb/never-existed.parquet', missing_ok := true);
-- without the flag the same delete is an error rather than a silent no-op
-- expect-fail: Nothing to delete
SELECT * FROM hf_delete('hf://datasets/lhoestq/playground/duckdb/never-existed.parquet');
SELECT 'd1 a directory deletes with everything below it' AS t, count(*) = 3 AS assert_three_files
FROM hf_delete('hf://datasets/lhoestq/playground/duckdb/wpart');
SELECT 'd2 and they are really gone' AS t, count(*) = 0 AS assert_nothing_left
FROM hf_ls('hf://datasets/lhoestq/playground/duckdb', recursive := true) WHERE path LIKE '%wpart%';
SELECT 'd3 deletes take a mixed list' AS t, count(*) = 8 AS assert_eight_deleted
FROM hf_delete(['hf://datasets/lhoestq/playground/duckdb/w.parquet', 'hf://datasets/lhoestq/playground/duckdb/w.csv',
                'hf://datasets/lhoestq/playground/duckdb/w.json',
                'hf://datasets/lhoestq/playground/duckdb/empty.parquet',
                'hf://datasets/lhoestq/playground/duckdb/zstd.parquet',
                'hf://datasets/lhoestq/playground/duckdb/stale.parquet',
                'hf://datasets/lhoestq/playground/duckdb/wéird name.parquet',
                'hf://models/lhoestq/playground/duckdb/m.parquet']);

SELECT 'z1 nothing left in the dataset repo' AS t, count(*) = 0 AS assert_empty
FROM hf_ls('hf://datasets/lhoestq/playground', recursive := true) WHERE path LIKE 'duckdb%';
SELECT 'z2 nothing left in the model repo' AS t, count(*) = 0 AS assert_empty
FROM hf_ls('hf://models/lhoestq/playground', recursive := true) WHERE path LIKE 'duckdb%';
SELECT 'z3 nothing left in the bucket' AS t, count(*) = 0 AS assert_empty
FROM hf_ls('hf://buckets/lhoestq/playground', recursive := true) WHERE path LIKE 'duckdb%';
