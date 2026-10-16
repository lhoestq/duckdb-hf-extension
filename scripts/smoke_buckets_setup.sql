-- Fixtures for scripts/smoke_buckets.sh: data written by the extension itself, into a staging dataset repo.
-- A first write to a missing repository has to bootstrap it, so this file doubles as the create-on-write test.
SET hf_create_repo = true;
-- the suite is idempotent: the fixtures are rewritten each run and the repo may or may not exist yet
SELECT 'reset staging repo' AS t, count(*) AS deleted_before FROM hf_delete('hf://datasets/lhoestq/hf-smoke-staging/data', missing_ok := true);
COPY (SELECT 42 AS answer UNION ALL SELECT 43) TO 'hf://datasets/lhoestq/hf-smoke-staging/data/one.parquet' (FORMAT PARQUET);
COPY (SELECT 7 AS answer) TO 'hf://datasets/lhoestq/hf-smoke-staging/data/two.parquet' (FORMAT PARQUET);
COPY (SELECT 1 AS a, 'x' AS b) TO 'hf://datasets/lhoestq/hf-smoke-staging/data/small.csv' (FORMAT CSV);
SELECT 'setup wrote the fixtures' AS t,
       count(*) = 3 AS assert_three_files,
       sum(size) > 0 AS assert_files_have_bytes
FROM hf_ls('hf://datasets/lhoestq/hf-smoke-staging/data');
SELECT 'the repository is readable right away' AS t, count(*) = 3 AS assert_reads_back, sum(answer) = 92 AS assert_values
FROM read_parquet('hf://datasets/lhoestq/hf-smoke-staging/data/*.parquet');
