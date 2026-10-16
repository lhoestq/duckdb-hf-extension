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
