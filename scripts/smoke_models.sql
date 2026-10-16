-- Model repositories, read and written back through the extension. Run with scripts/smoke_models.sh, which
-- builds the two local fixtures. Model repos address files without the `models/` prefix in the underlying
-- git url, which is what the nested-path and byte-identity checks below are really about.
CREATE TABLE pts AS SELECT i AS i, 'name_' || (i % 7) AS name FROM range(5000) t(i);
COPY (SELECT 'hello model' AS a) TO 'hf://models/lhoestq/playground/duckdb/m1.parquet' (FORMAT PARQUET);
SELECT 'm1 parquet' AS t, a = 'hello model' AS assert_value FROM 'hf://models/lhoestq/playground/duckdb/m1.parquet';
COPY pts TO 'hf://models/lhoestq/playground/duckdb/nested/deep/part-0.parquet' (FORMAT PARQUET, COMPRESSION zstd);
SELECT 'm2 a path nested three deep' AS t, count(*) = 5000 AS assert_rows, count(DISTINCT name) = 7 AS assert_names
FROM read_parquet('hf://models/lhoestq/playground/duckdb/nested/deep/*.parquet');
COPY (SELECT 1 AS a, 'x' AS b) TO 'hf://models/lhoestq/playground/duckdb/m1.csv' (FORMAT CSV);
SELECT 'm3 csv' AS t, count(*) = 1 AS assert_rows FROM 'hf://models/lhoestq/playground/duckdb/m1.csv';
COPY (SELECT 'json' AS k) TO 'hf://models/lhoestq/playground/duckdb/m1.json';
SELECT 'm4 json' AS t, k = 'json' AS assert_value FROM 'hf://models/lhoestq/playground/duckdb/m1.json';
COPY (SELECT 'git tracked text' AS t) TO 'hf://models/lhoestq/playground/duckdb/m1.txt';
SELECT 'm5 a small text file goes in as a git blob' AS t, t = 'git tracked text' AS assert_value
FROM 'hf://models/lhoestq/playground/duckdb/m1.txt';
SELECT 'm6 ** globbing over a model repo' AS t, count(*) >= 2 AS assert_found
FROM glob('hf://models/lhoestq/playground/duckdb/**/*.parquet');
SELECT 'm7 listing, flat and recursive' AS t,
       count(*) = 5 AS assert_five_entries_flat,
       sum(CASE WHEN is_dir THEN 1 ELSE 0 END) > 0 AS assert_directories_show_up
FROM hf_ls('hf://models/lhoestq/playground/duckdb');
SELECT 'm8 recursive listing sees the nesting' AS t,
       sum(CASE WHEN path = 'duckdb/nested/deep/part-0.parquet' THEN 1 ELSE 0 END) = 1 AS assert_deep_file,
       count(*) >= 6 AS assert_everything
FROM hf_ls('hf://models/lhoestq/playground/duckdb', recursive := true);
SELECT 'm9 hf_meta' AS t, exists AS assert_exists, size > 0 AS assert_size, repo_type = 'models' AS assert_repo_type
FROM hf_meta('hf://models/lhoestq/playground/duckdb/m1.parquet');
COPY (SELECT 9 AS v) TO 'hf://models/lhoestq/playground/duckdb/m1.parquet' (FORMAT PARQUET);
SELECT 'm10 overwrite' AS t, v = 9 AS assert_new_value FROM 'hf://models/lhoestq/playground/duckdb/m1.parquet';
SELECT 'm11 an LFS-tracked blob' AS t, size > 0 AS assert_size
FROM hf_upload('/tmp/hf_model_blob.bin', 'hf://models/lhoestq/playground/duckdb/blob.bin');
SELECT 'm12 a git-tracked text file' AS t, size > 0 AS assert_size
FROM hf_upload('/tmp/hf_model_text.txt', 'hf://models/lhoestq/playground/duckdb/m2.txt');
SELECT 'm13 both read back byte identical' AS t,
       (SELECT md5(content) FROM read_text('hf://models/lhoestq/playground/duckdb/m2.txt')) =
       (SELECT md5(content) FROM read_text('/tmp/hf_model_text.txt')) AS assert_git_blob_matches,
       (SELECT md5(content) FROM read_blob('hf://models/lhoestq/playground/duckdb/blob.bin')) =
       (SELECT md5(content) FROM read_blob('/tmp/hf_model_blob.bin')) AS assert_lfs_blob_matches,
       (SELECT size FROM hf_meta('hf://models/lhoestq/playground/duckdb/blob.bin')) > 0 AS assert_meta_size
FROM hf_meta('hf://models/lhoestq/playground/duckdb/blob.bin');
SELECT 'm14 the commits are on the repository' AS t, count(*) > 1 AS assert_several
FROM hf_list_revisions('hf://models/lhoestq/playground');
SELECT 'm15 branches and tags are readable' AS t, count(*) > 0 AS assert_refs
FROM hf_list_refs('hf://models/lhoestq/playground');
SELECT 'm16 cleanup' AS t, count(*) > 1 AS assert_deleted FROM hf_delete('hf://models/lhoestq/playground/duckdb');
SELECT 'm17 nothing left behind' AS t, count(*) = 0 AS assert_empty
FROM hf_ls('hf://models/lhoestq/playground', recursive := true) WHERE path LIKE 'duckdb%';
