-- read-side smoke test. Run with:
--   timeout 900 python3 -u scripts/try.py < scripts/smoke_read.sql
SELECT 'ls flat' AS t, count(*) AS n FROM hf_ls('hf://datasets/lhoestq/demo1');
SELECT 'ls recursive' AS t, count(*) AS n FROM hf_ls('hf://datasets/wikimedia/wikipedia', recursive := true);
SELECT 'ls short form' AS t, count(*) AS n FROM hf_ls('hf://lhoestq/demo1');
SELECT 'meta' AS t, size > 0, repo_type, repository FROM hf_meta('hf://datasets/lhoestq/demo1/README.md');
SELECT 'meta missing' AS t, exists FROM hf_meta('hf://datasets/lhoestq/demo1/nope.parquet');
SELECT 'glob one level' AS t, count(*) AS n FROM glob('hf://datasets/lhoestq/demo1/data/*.csv');
SELECT 'glob double star' AS t, count(*) AS n FROM glob('hf://datasets/wikimedia/wikipedia/**/*.parquet');
SELECT 'raw read' AS t, length(content) AS n FROM read_text('hf://datasets/lhoestq/demo1/README.md');
SELECT 'csv glob' AS t, count(*) AS n FROM read_csv('hf://datasets/lhoestq/demo1/data/*.csv');
SELECT 'csv direct' AS t, count(*) AS n FROM 'hf://datasets/lhoestq/demo1/data/train.csv';
SELECT 'parquet direct' AS t, count(*) AS n FROM 'hf://datasets/wikimedia/wikipedia/20231101.ab/train-00000-of-00001.parquet';
SELECT 'parquet twice' AS t, count(*) AS n FROM 'hf://datasets/wikimedia/wikipedia/20231101.ab/train-00000-of-00001.parquet';
SELECT 'parquet limit' AS t, count(*) AS n FROM (SELECT * FROM 'hf://datasets/wikimedia/wikipedia/20231101.ab/train-00000-of-00001.parquet' LIMIT 10);
SELECT 'parquet metadata' AS t, count(*) AS n FROM parquet_metadata('hf://datasets/wikimedia/wikipedia/20231101.ab/train-00000-of-00001.parquet');
SELECT 'parquet schema' AS t, count(*) AS n FROM parquet_schema('hf://datasets/wikimedia/wikipedia/20231101.ab/train-00000-of-00001.parquet');
SELECT 'multi file scan' AS t, count(*) AS n FROM read_parquet('hf://datasets/wikimedia/wikipedia/20231101.ab/*.parquet');
SELECT 'footer read' AS t, sum(total_compressed_size) > 0 AS ok FROM parquet_metadata('hf://datasets/wikimedia/wikipedia/20231101.ab/*.parquet');
SELECT 'revision read' AS t, count(*) AS n FROM 'hf://datasets/lhoestq/demo1@main/data/train.csv';
SELECT 'glob model' AS t, count(*) AS n FROM glob('hf://models/lhoestq/playground/duckdb/*');
SELECT 'glob one level' AS t2, count(*) AS n2 FROM glob('hf://datasets/wikimedia/wikipedia/20231101.ab/*');
SELECT 'ls space' AS t, count(*) > 0 AS n FROM hf_ls('hf://spaces/dalle-mini/dalle-mini');
SELECT 'ls kernel' AS t, count(*) > 0 AS n FROM hf_ls('hf://kernels/kernels-community/flash-attn3');
SELECT 'kernel file' AS t, length(content) > 0 AS n FROM read_text('hf://kernels/kernels-community/flash-attn3/README.md');
SELECT 'model config' AS t, length(content) > 10 AS n FROM read_text('hf://models/google-bert/bert-base-uncased/config.json');
SELECT 'model glob' AS t, count(*) > 0 AS n FROM glob('hf://models/google-bert/bert-base-uncased/*.json');
SELECT 'refs' AS t, kind, name FROM hf_list_refs('hf://datasets/lhoestq/demo1');
SELECT 'revisions' AS t, count(*) > 0 AS ok FROM hf_list_revisions('hf://datasets/lhoestq/demo1', max_revisions := 3);
SELECT 'status' AS t, endpoint LIKE 'https://%', token_source <> 'none', whoami_error = '' FROM hf_status();
-- Read path behaviour: what a fetch costs, what the retained cache saves, and how one missing window is
-- split over range requests. Columns named assert_* are promises that scripts/try.py checks.
-- DuckDB 2.0 caches remote file blocks itself (`enable_external_file_cache`, in memory, on top of any
-- filesystem), so it is switched off here: these checks are about what *this* extension fetches.
SET enable_external_file_cache = false;
SELECT 'p1 reset the counters' AS t, count(*) = 1 AS ok FROM hf_read_stats(reset := true);
SELECT 'p2 counters start at zero' AS t, bytes_fetched = 0 AS assert_reset_zeroed, window_fetches = 0 AS assert_no_windows FROM hf_read_stats();
SELECT 'p3 cold read of a file' AS t, octet_length(content) > 0 AS ok FROM read_blob('hf://models/google-bert/bert-base-uncased/tokenizer.json');
SELECT 'p4 it fetched whole windows, covering the file' AS t, bytes_fetched >= (SELECT size FROM hf_meta('hf://models/google-bert/bert-base-uncased/tokenizer.json')) AS assert_covered_the_file, window_fetches > 0 AS assert_fetched_windows, part_requests >= window_fetches AS assert_at_least_one_part_per_window FROM hf_read_stats();
SELECT 'p5 reset' AS t, count(*) = 1 AS ok FROM hf_read_stats(reset := true);
SELECT 'p6 read the same file again' AS t, octet_length(content) > 0 AS ok FROM read_blob('hf://models/google-bert/bert-base-uncased/tokenizer.json');
SELECT 'p7 the cache kept across the close served it' AS t, bytes_fetched = 0 AS assert_nothing_downloaded, retained_bytes > 0 AS assert_still_retained FROM hf_read_stats();
SELECT 'p8 the cache can be dropped on purpose' AS t, cleared_bytes > 0 AS assert_cleared_bytes, cleared_files > 0 AS assert_cleared_files FROM hf_read_cache_clear();
SELECT 'p9 reset' AS t, count(*) = 1 AS ok FROM hf_read_stats(reset := true);
SELECT 'p10 so the next read goes back to the network' AS t, octet_length(content) > 0 AS ok FROM read_blob('hf://models/google-bert/bert-base-uncased/tokenizer.json');
SELECT 'p11 and it fetched again' AS t, bytes_fetched > 0 AS assert_fetched_again, retained_bytes > 0 AS assert_retained_again FROM hf_read_stats();
-- a 64KiB grid over a 1.2MB parquet: several windows, each pulled apart into parallel range parts, and
-- still exactly one copy of every byte of the file on the wire
SELECT 'p12 reset and start from an empty cache' AS t, count(*) = 1 AS ok FROM hf_read_stats(reset := true);
SELECT 'p13 cleared' AS t, cleared_files >= 0 AS ok FROM hf_read_cache_clear();
SET hf_read_buffer_size = 65536;
SELECT 'p14 full scan with 64KiB windows' AS t, count(*) AS n, sum(len(text)) AS chars FROM 'hf://datasets/wikimedia/wikipedia/20231101.ab/train-00000-of-00001.parquet';
SELECT 'p15 no byte was fetched twice, windows were split' AS t, bytes_fetched = (SELECT size FROM hf_meta('hf://datasets/wikimedia/wikipedia/20231101.ab/train-00000-of-00001.parquet')) AS assert_fetched_the_file_exactly_once, window_fetches > 1 AS assert_several_windows, part_requests > window_fetches AS assert_fanned_out, straddling_fetches = 0 AS assert_stayed_on_the_grid FROM hf_read_stats();
SET VARIABLE fanout_bytes = (SELECT bytes_fetched FROM hf_read_stats());
SELECT 'p16 reset and clear' AS t, count(*) = 1 AS ok FROM hf_read_stats(reset := true);
SELECT 'p17 cleared' AS t, cleared_bytes > 0 AS assert_cleared FROM hf_read_cache_clear();
SET hf_read_max_parallel_requests = 1;
SELECT 'p18 same scan, one request per window' AS t, count(*) AS n, sum(len(text)) AS chars FROM 'hf://datasets/wikimedia/wikipedia/20231101.ab/train-00000-of-00001.parquet';
SELECT 'p19 one part per window, same bytes on the wire' AS t, part_requests = window_fetches AS assert_no_fanout, bytes_fetched = getvariable('fanout_bytes') AS assert_identical_bytes, bytes_fetched = (SELECT size FROM hf_meta('hf://datasets/wikimedia/wikipedia/20231101.ab/train-00000-of-00001.parquet')) AS assert_fetched_the_file_exactly_once FROM hf_read_stats();
RESET hf_read_max_parallel_requests;
RESET hf_read_buffer_size;
RESET enable_external_file_cache;
SELECT 'p20 settings restored' AS t, hf_read_max_parallel_requests = 4 AS assert_parallel_restored, hf_read_buffer_size = 8388608 AS assert_buffer_restored FROM hf_status();
