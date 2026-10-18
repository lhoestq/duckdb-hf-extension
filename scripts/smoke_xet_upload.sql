-- The Xet upload path, against the real CAS.
--
-- hf_xet_upload chunks a file the way Xet does and puts each xorb where the CAS stores it, addressed
-- by the hash of its chunks. A successful upload is therefore two checks at once: the CAS recomputes
-- the hash from the bytes it received and refuses a mismatch, and it reports whether the content was
-- new. Nothing here is stubbed or simulated, and nothing here can pass with a writer that produces
-- bytes the real service cannot read (docs/XET.md, M3 + M4).
--
-- The suite also covers the other half: the metadata shard that turns chunks back into a file, and the
-- bucket batch call that names that file at a path. That combination is `COPY ... TO 'hf://buckets/...'`,
-- and it is checked the only way that settles the question - by reading the file back through `hf://`
-- and comparing bytes with what went in.
--
-- Everything here is deleted again at the end; the chunks themselves stay in the CAS, which is what
-- content addressable storage is for.

SET hf_create_repo = true;

-- Fresh content on every run, since "was this already stored?" is one of the two things being asked,
-- and a deterministic fixture would answer it the same way for the wrong reason.
COPY (SELECT md5((i * random())::VARCHAR) AS a, random() AS b, md5(random()::VARCHAR) AS c
      FROM range(20000) t(i))
     TO 'hf://datasets/lhoestq/hf-smoke-staging/xet_upload_probe.csv' (FORMAT CSV);

SELECT count(*) >= 1                                  AS assert_xorbs_uploaded,
       sum(num_chunks) >= 2                           AS assert_content_was_chunked,
       bool_and(inserted)                                AS assert_fresh_content_is_inserted,
       max(num_chunks) <= 8192                        AS assert_chunk_limit_respected,
       max(uncompressed_bytes) <= 67108864            AS assert_size_limit_respected,
       -- the footer and the chunk headers are the difference between the two byte counts
       bool_and(bytes > uncompressed_bytes)              AS assert_serialized_is_larger,
       count(DISTINCT xorb) = count(*)                AS assert_each_xorb_is_distinct
FROM hf_xet_upload('hf://datasets/lhoestq/hf-smoke-staging/xet_upload_probe.csv');

-- The same content again: the CAS holds it already, and says so. Same layout too, which is the
-- chunker being deterministic rather than getting lucky.
SELECT bool_and(NOT inserted)                            AS assert_repeat_is_already_stored,
       sum(bytes) = (SELECT sum(bytes)
                    FROM hf_xet_upload('hf://datasets/lhoestq/hf-smoke-staging/xet_upload_probe.csv'))
                                                     AS assert_layout_is_deterministic
FROM hf_xet_upload('hf://datasets/lhoestq/hf-smoke-staging/xet_upload_probe.csv');

-- A CAS token is scoped to a repository, so uploading needs one even when the bytes are not in a
-- repository at all - which is how a COPY will do it: DuckDB fills a local temp file, and the target
-- repo names the scope its content goes up against.
COPY (SELECT md5((i * random())::VARCHAR) AS a, random() AS b FROM range(9000) t(i))
     TO '/tmp/hf_xet_upload_probe.csv' (FORMAT CSV);

SELECT bool_and(inserted) AS assert_local_content_is_inserted, count(*) >= 1 AS assert_uploaded
FROM hf_xet_upload('/tmp/hf_xet_upload_probe.csv', scope := 'hf://datasets/lhoestq/hf-smoke-staging');

SELECT bool_and(NOT inserted) AS assert_local_repeat_is_already_stored
FROM hf_xet_upload('/tmp/hf_xet_upload_probe.csv', scope := 'hf://datasets/lhoestq/hf-smoke-staging');

-- expected failure: the scope names something that is not a repository, so no token can be asked for
-- expect-fail: scope must be an hf:// url
SELECT * FROM hf_xet_upload('/tmp/hf_xet_upload_probe.csv', scope := '/tmp');

SELECT count(*) AS deleted FROM hf_delete('hf://datasets/lhoestq/hf-smoke-staging/xet_upload_probe.csv',
                                          missing_ok := true);

-- Publishing: content in the CAS is addressable but nameless; a shard describes it as a file, and a
-- bucket's `addFile` gives that file a path.

-- random() again, so that the *first* publish of a run really inserts and the second really does not
COPY (SELECT md5((i * random())::VARCHAR) AS a, random()::VARCHAR AS b FROM range(5000) t(i))
     TO '/tmp/hf_xet_bucket_probe.csv' (FORMAT CSV);

SELECT bool_and(published) AS assert_published,
       bool_and(len(xet_hash) = 64) AS assert_hash_is_a_xet_hash,
       bool_and(inserted) AS assert_content_is_new,
       bool_and(shard_bytes > 0) AS assert_shard_was_uploaded,
       bool_and(shard_result IN ('exists', 'sync performed')) AS assert_shard_answered
FROM hf_xet_upload('/tmp/hf_xet_bucket_probe.csv',
                   scope := 'hf://buckets/lhoestq/playground/duckdb/xet_upload.csv', publish := true);

SELECT 'p1 a published file reads back byte for byte' AS t,
       md5(content) = (SELECT md5(content) FROM read_blob('/tmp/hf_xet_bucket_probe.csv')) AS assert_same_bytes
FROM read_blob('hf://buckets/lhoestq/playground/duckdb/xet_upload.csv');

-- The same content a second time must be refused work rather than duplicated, both in the chunks and in
-- the shard: this is the property that makes re-uploading a directory cheap.
SELECT bool_and(NOT inserted) AS assert_repeat_is_deduplicated,
       bool_and(shard_result = 'exists') AS assert_shard_is_already_there
FROM hf_xet_upload('/tmp/hf_xet_bucket_probe.csv',
                   scope := 'hf://buckets/lhoestq/playground/duckdb/xet_upload_twice.csv', publish := true);

-- `COPY ... TO 'hf://buckets/...'` is the same pipeline with DuckDB filling the temp file.
COPY (SELECT md5((i * random())::VARCHAR) AS a FROM range(7000) t(i))
     TO 'hf://buckets/lhoestq/playground/duckdb/xet_copy.csv' (FORMAT CSV);
SELECT 'p2 COPY TO a bucket writes through Xet' AS t, count(*) = 7000 AS assert_rows,
       count(DISTINCT a) = 7000 AS assert_content_is_there
FROM read_csv('hf://buckets/lhoestq/playground/duckdb/xet_copy.csv');

-- rewriting a path publishes the new content under the old name
COPY (SELECT i AS a FROM range(3) t(i))
     TO 'hf://buckets/lhoestq/playground/duckdb/xet_copy.csv' (FORMAT CSV, HEADER false);
SELECT 'p3 rewriting a bucket path replaces the file' AS t, count(*) = 3 AS assert_rows,
       min(a) = 0 AS assert_min, max(a) = 2 AS assert_max
FROM read_csv('hf://buckets/lhoestq/playground/duckdb/xet_copy.csv', header := false, columns := {'a': 'BIGINT'});

-- More than one xorb, which means the metadata shard has to describe a file whose segments live in
-- several objects - and the Hub has to agree, since it is the one assembling the read ranges.
COPY (SELECT repeat(md5((i * random())::VARCHAR), 12) AS b FROM range(200000) t(i))
     TO 'hf://buckets/lhoestq/playground/duckdb/xet_big.parquet' (FORMAT PARQUET, COMPRESSION UNCOMPRESSED);
SELECT 'p4 a file spanning several xorbs is rebuilt' AS t, count(*) = 200000 AS assert_rows,
       sum(length(b)) = 76800000 AS assert_bytes, count(DISTINCT b) > 190000 AS assert_no_corruption
FROM 'hf://buckets/lhoestq/playground/duckdb/xet_big.parquet';

-- A copy asks the Hub for the xetHash of a file *this extension* wrote: if the shard had described the
-- content in a way the Hub could not read back, the hash would never arrive.
SELECT 'p5 the hub sees a xet-written file as xet content' AS t,
       bool_and(status = 'copied') AS assert_copied,
       bool_and(len(xet_hash) = 64) AS assert_hashed
FROM hf_bucket_copy('hf://buckets/lhoestq/playground/duckdb/xet_upload.csv',
                    'hf://buckets/lhoestq/playground/duckdb/xet_copied.csv');

-- Several streams must not change what the content *is*. The file above was written with the default
-- concurrency (three xorbs in flight); re-uploading it with the uploads serialized has to find every
-- xorb already stored, since an xorb's url is its content hash and a different xorb order, or a xorb
-- built against a different boundary, would produce hashes the CAS has never seen and would answer
-- `inserted: true` to. Cheapest cross-check of the concurrent path that exists: it costs no new storage.
SET hf_write_max_parallel_uploads = 1;
SELECT 'p6 concurrency changes how xorbs travel, not what they are' AS t,
       count(*) >= 2 AS assert_several_xorbs,
       bool_and(NOT inserted) AS assert_every_xorb_already_stored,
       bool_and(shard_result = 'exists') AS assert_shard_already_stored
FROM hf_xet_upload('hf://buckets/lhoestq/playground/duckdb/xet_big.parquet');
RESET hf_write_max_parallel_uploads;

SELECT count(*) AS cleaned FROM hf_delete('hf://buckets/lhoestq/playground/duckdb/xet_upload.csv');
SELECT count(*) AS cleaned_again FROM hf_delete('hf://buckets/lhoestq/playground/duckdb/xet_upload_twice.csv');
SELECT count(*) AS cleaned_again FROM hf_delete('hf://buckets/lhoestq/playground/duckdb/xet_copy.csv');
SELECT count(*) AS cleaned_again FROM hf_delete('hf://buckets/lhoestq/playground/duckdb/xet_big.parquet');
SELECT count(*) AS cleaned_again FROM hf_delete('hf://buckets/lhoestq/playground/duckdb/xet_copied.csv');
