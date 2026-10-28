-- hf_dataset() / hf_dataset_files(). Run with scripts/smoke_datasets.sh, which starts the datasets-server
-- fixture this suite needs and replaces @PORT@ with the port it landed on.
--
-- hf_dataset() asks the Hub's datasets-server which parquet files a dataset has been converted to, turns
-- the urls of the answer into hf:// ones, and hands them to read_parquet. What matters here is the whole
-- chain: the listing, the conversion of its urls, reading them through the extension (token included), and
-- the refusals - a dataset read silently in halves is indistinguishable from one read whole, so the
-- refusals are the part that has to be nailed down.
--
-- Row counts belong to datasets that have been frozen for years. What the conversion looks like today -
-- how many shards a split is spread over, which configs a dataset has - is asserted where that is the
-- point, and is the part that would move if the Hub ever re-converted them.

-- === the setting ===

SELECT 'endpoint default' AS t,
       current_setting('hf_datasets_server_endpoint') = 'https://datasets-server.huggingface.co' AS assert_default;

-- a host typed without a scheme, and a host with a trailing slash, are both ways a url gets written by
-- hand, and both have to concatenate correctly on the way to the route
SET hf_datasets_server_endpoint = 'datasets-server.huggingface.co/';
SELECT 'endpoint normalized' AS t,
       current_setting('hf_datasets_server_endpoint') = 'https://datasets-server.huggingface.co' AS assert_normalized;
SELECT 'endpoint is described, and global' AS t,
       count(*) FILTER (WHERE description <> '') = 1 AS assert_described,
       count(*) FILTER (WHERE scope = 'GLOBAL') = 1 AS assert_global,
       count(*) FILTER (WHERE value = current_setting('hf_datasets_server_endpoint')) = 1 AS assert_value_reported
FROM duckdb_settings() WHERE name = 'hf_datasets_server_endpoint';
RESET hf_datasets_server_endpoint;
SELECT 'endpoint reset' AS t,
       current_setting('hf_datasets_server_endpoint') = 'https://datasets-server.huggingface.co' AS assert_back_to_default;

-- === the listing ===

SELECT 'files: what demo1 has' AS t,
       count(*) = 2 AS assert_two_shards,
       count(DISTINCT config) = 1 AS assert_one_config,
       count(DISTINCT split) = 2 AS assert_two_splits,
       bool_and(dataset = 'lhoestq/demo1') AS assert_dataset_column,
       bool_and(filename LIKE '%.parquet') AS assert_filenames,
       bool_and(size > 0) AS assert_sizes_reported,
       NOT bool_or(partial) AS assert_not_partial
FROM hf_dataset_files('lhoestq/demo1');

-- The answer's urls are https ones, which httpfs would fetch anonymously and a gated dataset would
-- refuse. They have to arrive as hf:// urls - and with the conversion revision still percent-encoded,
-- because the Hub only serves `refs%2Fconvert%2Fparquet` and answers 404 for the same url written plain.
SELECT 'files: the urls are hf urls' AS t,
       bool_and(starts_with(url, 'hf://datasets/lhoestq/demo1@refs%2Fconvert%2Fparquet/default/')) AS assert_hf_urls,
       bool_and(NOT starts_with(url, 'https://')) AS assert_not_left_on_https,
       bool_and(contains(url, '/train/') OR contains(url, '/test/')) AS assert_split_in_the_path
FROM hf_dataset_files('lhoestq/demo1');

-- which is to say: the listed url is readable, and by this extension
SELECT 'files: a listed url reads as listed' AS t,
       count(*) = 5 AS assert_the_train_shard
FROM read_parquet('hf://datasets/lhoestq/demo1@refs%2Fconvert%2Fparquet/default/train/0000.parquet');

SELECT 'files: the configs and splits of a dataset that has several' AS t,
       count(DISTINCT config) = 2 AS assert_two_configs,
       count(DISTINCT split) = 4 AS assert_four_splits,
       count(*) FILTER (WHERE config = 'full' AND split = 'train') = 1 AS assert_one_shard_of_one_split
FROM hf_dataset_files('google-research-datasets/mbpp');

-- the listing is fetched while the function runs, not while the plan is made
EXPLAIN SELECT * FROM hf_dataset_files('lhoestq/demo1');

-- === reading ===

SELECT 'read every split of a config' AS t, count(*) = 10 AS assert_ten_rows FROM hf_dataset('lhoestq/demo1');
SELECT 'read one split' AS t, count(*) = 5 AS assert_five_rows FROM hf_dataset('lhoestq/demo1', split := 'train');
SELECT 'read a second time' AS t, count(*) = 5 AS assert_five_rows FROM hf_dataset('lhoestq/demo1', split := 'train');
-- the columns are the parquet file's own, not something hf_dataset invented on the way
SELECT 'the schema is the parquet schema' AS t,
       typeof(star) = 'BIGINT' AS assert_star_bigint,
       typeof(review) = 'VARCHAR' AS assert_review_varchar,
       review IS NOT NULL AS assert_rows_came_by
FROM hf_dataset('lhoestq/demo1', split := 'train') LIMIT 1;

-- several shards and four splits in one scan, aligned by name rather than by position
SELECT 'every split of a config at once' AS t, count(*) = 974 AS assert_974_rows
FROM hf_dataset('google-research-datasets/mbpp', config := 'full');
SELECT 'and that is the sum of its splits' AS t,
       (SELECT count(*) FROM hf_dataset('google-research-datasets/mbpp', config := 'full', split := 'train')) = 374 AS assert_train,
       (SELECT count(*) FROM hf_dataset('google-research-datasets/mbpp', config := 'full', split := 'test')) = 500 AS assert_test,
       (SELECT count(*) FROM hf_dataset('google-research-datasets/mbpp', config := 'full', split := 'validation')) = 90 AS assert_validation,
       (SELECT count(*) FROM hf_dataset('google-research-datasets/mbpp', config := 'full', split := 'prompt')) = 10 AS assert_prompt;

-- a dataset of seven configs, read down to one split of one of them
SELECT 'one split of one config of a big one' AS t, count(*) = 5010 AS assert_5010
FROM hf_dataset('facebook/xnli', config := 'ar', split := 'test');

-- and a split too big for one parquet file, which is read as the several files the answer says it is
SELECT 'a split spread over several shards' AS t,
       (SELECT count(*) FILTER (WHERE config = 'wikitext-103-raw-v1' AND split = 'train')
        FROM hf_dataset_files('Salesforce/wikitext')) = 2 AS assert_two_shards_offered,
       count(*) = 1801350 AS assert_all_of_them_read
FROM hf_dataset('Salesforce/wikitext', config := 'wikitext-103-raw-v1', split := 'train');

-- two calls are two scans, and they can be joined like any two tables
SELECT 'two calls join' AS t, count(*) = 5 AS assert_five
FROM hf_dataset('lhoestq/demo1') a JOIN hf_dataset('lhoestq/demo1', split := 'test') b USING (id);

-- === how the dataset may be named ===

SELECT 'as an hf:// url' AS t, count(*) = 10 AS assert_ten FROM hf_dataset('hf://datasets/lhoestq/demo1');
SELECT 'as a short hf:// url' AS t, count(*) = 10 AS assert_ten FROM hf_dataset('hf://lhoestq/demo1');
SELECT 'as an hf:// url with a revision' AS t, count(*) = 5 AS assert_five FROM hf_dataset('hf://datasets/lhoestq/demo1@main', split := 'train');
SELECT 'as a hub page url' AS t, count(*) = 10 AS assert_ten FROM hf_dataset('https://huggingface.co/datasets/lhoestq/demo1');
SELECT 'as a hub page url pointing into the repo' AS t, count(*) = 10 AS assert_ten
FROM hf_dataset('https://huggingface.co/datasets/lhoestq/demo1/tree/main/data');
SELECT 'with a revision asked for' AS t, count(*) = 10 AS assert_ten FROM hf_dataset('lhoestq/demo1', revision := 'main');

-- expect-fail: names a repository of type 'models'
SELECT 'a model is not a dataset' AS t, count(*) FROM hf_dataset('hf://models/google-bert/bert-base-uncased');
-- expect-fail: not a path inside one
SELECT 'a path inside a dataset is not a dataset' AS t, count(*)
FROM hf_dataset('hf://datasets/lhoestq/demo1/data/train.csv');
-- expect-fail: <namespace>/<name>
SELECT 'a name without a namespace' AS t, count(*) FROM hf_dataset('demo1');
-- expect-fail: pass it as 'namespace/name'
SELECT 'an url that is not a dataset page' AS t, count(*) FROM hf_dataset('https://example.com/somewhere/else');

-- === what it refuses ===

-- the hub answers 404 for a dataset it has never heard of, and this says so with the hub's own reason
-- expect-fail: does not exist
SELECT 'no such dataset' AS t, count(*) FROM hf_dataset('hf-datasets-no-such-dataset/nope');
-- an unknown split is answered with every split by the route, so refusing means the answer was filtered
-- here rather than trusted
-- expect-fail: no split named 'nope', its splits are test, train
SELECT 'no such split' AS t, count(*) FROM hf_dataset('lhoestq/demo1', split := 'nope');
-- an unknown config is answered with a bare 404 by the route, indistinguishable from a dataset that does
-- not exist: the second ask is what tells the two apart
-- expect-fail: no config named 'nope', its configs are full, sanitized
SELECT 'no such config' AS t, count(*) FROM hf_dataset('google-research-datasets/mbpp', config := 'nope');
-- two configs are two schemas. Union-ing them by name would answer a question nobody asked with a wide,
-- half-null table, so the call asks for the decision instead
-- expect-fail: has 2 configs (full, sanitized)
SELECT 'a dataset with more than one config' AS t, count(*) FROM hf_dataset('google-research-datasets/mbpp');

-- === a datasets-server that is not complete, and not the production one ===
-- Whether a dataset is half converted is a fact about a moment on the Hub, and cannot be provoked on
-- demand. The fixture answers these dataset names with fixed bodies; the parquet files it lists are the
-- real ones, so everything below still fetches over hf:// with the token.

SET hf_datasets_server_endpoint = 'http://127.0.0.1:@PORT@';

SELECT 'the fixture is what answers now' AS t, count(*) = 2 AS assert_two_shards, bool_and(partial) AS assert_partial
FROM hf_dataset_files('fixture/partial');

-- expect-fail: converted only part of dataset 'fixture/partial'
SELECT 'a partial conversion is not an answer' AS t, count(*) FROM hf_dataset('fixture/partial');
-- expect-fail: allow_partial := true
SELECT 'and it says what to do about it' AS t, count(*) FROM hf_dataset('fixture/partial');

-- what is converted is still readable, and the count is what the same files give when the whole dataset is
-- converted (the fixture hands everything else to the production service)
SELECT 'allow_partial reads what exists' AS t,
       count(*) = (SELECT count(*) FROM hf_dataset('lhoestq/demo1')) AS assert_demo1_is_fully_converted
FROM hf_dataset('fixture/partial', allow_partial := true);

-- a url that is not the Hub's is left alone: it is all such a url can promise, and read_parquet reads it
-- over httpfs
SELECT 'a file that lives elsewhere is passed through' AS t,
       url = 'https://files.example.invalid/converted/train/0000.parquet' AS assert_untouched
FROM hf_dataset_files('fixture/external');

-- the route answers some of its refusals as a 200 with the reason in the body
-- expect-fail: the conversion of this dataset is queued
SELECT 'a refusal that arrives as a 200' AS t, count(*) FROM hf_dataset('fixture/refused');
