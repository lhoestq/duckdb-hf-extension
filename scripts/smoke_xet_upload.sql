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
       -- Chunks are LZ4-compressed by default (docs/XET.md, M8), so the serialized xorb is *smaller*
       -- than the content it carries - the framing it adds is 8 bytes per chunk, and what the frames
       -- saved on this CSV is far more than that. `uncompressed_bytes` stays the size the merkle tree
       -- is built over, which is why the two can move apart without anything about the content moving.
       sum(bytes) < sum(uncompressed_bytes)           AS assert_lz4_shrinks_the_wire,
       bool_and(bytes > 0)                               AS assert_serialized_is_positive,
       count(DISTINCT xorb) = count(*)                AS assert_each_xorb_is_distinct,
       -- Dedup is on by default, so this upload asked: a file's first chunk is always worth a query. It
       -- found nothing, and that is three separate things to check - that a shard this build cannot read
       -- is not silently the same as a miss, and that nothing was pointed at content that is not there.
       max(dedup_queries) >= 1                        AS assert_dedup_was_asked,
       max(dedup_shard_failures) = 0                  AS assert_dedup_answers_are_readable,
       max(deduped_chunks) = 0                        AS assert_nothing_was_pointed_at
FROM hf_xet_upload('hf://datasets/lhoestq/hf-smoke-staging/xet_upload_probe.csv');

-- The same content again: the CAS holds it already, and says so. Same layout too, which is the
-- chunker being deterministic rather than getting lucky.
--
-- Dedup is switched off for this and the repeats below, which is the subject rather than a hedge: those
-- tests are about which *xorbs* an upload asks the CAS for, and with dedup on that question is never
-- reached - the file is answered out of the index before an object is posted. An upload with nothing to
-- post is a real outcome and is tested for on its own, further down.
SET hf_xet_upload_dedup = false;

SELECT bool_and(NOT inserted)                            AS assert_repeat_is_already_stored,
       sum(bytes) = (SELECT sum(bytes)
                    FROM hf_xet_upload('hf://datasets/lhoestq/hf-smoke-staging/xet_upload_probe.csv'))
                                                     AS assert_layout_is_deterministic
FROM hf_xet_upload('hf://datasets/lhoestq/hf-smoke-staging/xet_upload_probe.csv');

-- The same file uploaded again with compression switched off. An xorb's hash is the merkle root of the
-- *uncompressed* chunk hashes, so switching codecs cannot change which objects the CAS is asked for -
-- and the CAS, which stores bytes without interpreting them, must recognize every one of them. This is
-- the strongest available live proof that compression is a wire decision and not a content decision, and
-- it costs no new storage. The uncompressed uploads are also where "the serialized form is larger than
-- the content, by the framing" is a true statement: with LZ4 it is the other way round.
SET hf_xet_compression = 'none';

SELECT bool_and(NOT inserted)                            AS assert_uncompressed_is_the_same_content,
       bool_and(bytes > uncompressed_bytes)              AS assert_uncompressed_carries_framing,
       count(*) >= 1                                     AS assert_uncompressed_uploaded
FROM hf_xet_upload('hf://datasets/lhoestq/hf-smoke-staging/xet_upload_probe.csv');

RESET hf_xet_compression;

-- A CAS token is scoped to a repository, so uploading needs one even when the bytes are not in a
-- repository at all - which is how a COPY will do it: DuckDB fills a local temp file, and the target
-- repo names the scope its content goes up against.
COPY (SELECT md5((i * random())::VARCHAR) AS a, random() AS b FROM range(9000) t(i))
     TO '/tmp/hf_xet_upload_probe.csv' (FORMAT CSV);

-- A git-backed shard also has to name the file's sha256, because that is the id its commit's pointer will
-- use and the shard is the only place the two are stated together. The CAS refuses the upload when the
-- field is missing, and accepts it when the field holds some other file's digest - so the digest is
-- returned, and compared here against sha256() computed by duckdb itself rather than by this extension.
SELECT bool_and(inserted) AS assert_local_content_is_inserted, count(*) >= 1 AS assert_uploaded,
       bool_and(sha256 = (SELECT sha256(content) FROM read_blob('/tmp/hf_xet_upload_probe.csv')))
           AS assert_the_shard_names_this_files_sha256
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
--
-- Dedup stays off through p1-p6 as well: what they check is that the *shard* for given content is the
-- same shard a second time (`shard_result = 'exists'`), and a deduplicated upload legitimately writes a
-- different shard - one whose segments point at content already stored, and which therefore carries no
-- xorb tables of its own.
SET hf_xet_upload_dedup = false;

-- random() again, so that the *first* publish of a run really inserts and the second really does not
COPY (SELECT md5((i * random())::VARCHAR) AS a, random()::VARCHAR AS b FROM range(5000) t(i))
     TO '/tmp/hf_xet_bucket_probe.csv' (FORMAT CSV);

SELECT bool_and(published) AS assert_published,
       bool_and(len(xet_hash) = 64) AS assert_hash_is_a_xet_hash,
       bool_and(inserted) AS assert_content_is_new,
       bool_and(shard_bytes > 0) AS assert_shard_was_uploaded,
       bool_and(shard_result IN ('exists', 'sync performed')) AS assert_shard_answered,
       -- a bucket file *is* its content, addressed by its xet hash: there is no pointer and no sha256 for
       -- the shard to name, so the digest a git-backed upload reports is absent here by design
       bool_and(sha256 IS NULL) AS assert_a_bucket_upload_names_no_sha256
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

--===----------------------------------------------------------------------===
-- Cross-repository dedup: the half a test of your own uploads cannot see
--===----------------------------------------------------------------------===#

-- Everything above asks the CAS about content this same run uploaded. That proves the bytes and the
-- determinism, and it cannot prove the thing dedup is for: pointing at content *someone else* stored,
-- and a shard whose segments name xorbs this upload never posted.
--
-- So: content stored against one repository and never published anywhere, then the same bytes published
-- from another. VARCHAR columns throughout, because the edited file below is rebuilt by reading the
-- first one back - a float that round-trips into different text is different bytes, and different bytes
-- are different content, and the test would quietly be about nothing.
SET hf_xet_upload_dedup = true;

COPY (SELECT md5((i * random())::VARCHAR) AS a, md5(random()::VARCHAR) AS b FROM range(60000) t(i))
     TO '/tmp/hf_xet_dedup_src.csv' (FORMAT CSV);

SELECT count(*) >= 1 AS assert_stored_by_the_first_repository,
       bool_and(inserted) AS assert_the_first_upload_is_new
FROM hf_xet_upload('/tmp/hf_xet_dedup_src.csv', scope := 'hf://datasets/lhoestq/hf-smoke-staging');

-- One row with no xorb in it is what an upload with nothing left to do looks like: the whole file was
-- answered by the shard the upload above left in the index, and what this one wrote is a shard of
-- pointers into xorbs that a *different* repository posted.
SELECT count(*) = 1                                     AS assert_one_row,
       bool_and(xorb IS NULL)                           AS assert_nothing_was_uploaded,
       max(deduped_chunks) > 0                          AS assert_chunks_were_pointed_at,
       max(dedup_queries) >= 1                          AS assert_the_cas_was_asked,
       max(dedup_hits) >= 1                             AS assert_the_cas_answered,
       max(dedup_shard_failures) = 0                    AS assert_the_answers_parsed,
       bool_and(published)                              AS assert_the_file_was_still_published,
       bool_and(len(shard_hash) = 64)                   AS assert_a_shard_was_named
FROM hf_xet_upload('/tmp/hf_xet_dedup_src.csv',
                   scope := 'hf://buckets/lhoestq/playground/duckdb/xet_dedup.csv', publish := true);

-- The claim being settled: a dedup pointer is not a dangling reference. The Hub serves a file published
-- from one repository out of content another repository stored.
SELECT 'p7 a file deduplicated against another repository reads back' AS t,
       md5(content) = (SELECT md5(content) FROM read_blob('/tmp/hf_xet_dedup_src.csv')) AS assert_same_bytes
FROM read_blob('hf://buckets/lhoestq/playground/duckdb/xet_dedup.csv');

-- The case the format exists for, which is not an identical file but a changed one. Chunk boundaries are
-- content-defined and depend only on the bytes before them, so appending at the end leaves every
-- boundary ahead of the change exactly where it was: the tail is new, everything before it is not.
COPY (SELECT * FROM read_csv('/tmp/hf_xet_dedup_src.csv', columns := {'a': 'VARCHAR', 'b': 'VARCHAR'})
      UNION ALL SELECT 'appended at the end', 'a changed tail')
     TO '/tmp/hf_xet_dedup_edited.csv' (FORMAT CSV);

SELECT count(*) >= 1                                    AS assert_the_change_was_uploaded,
       max(deduped_chunks) >= 1                         AS assert_the_rest_was_not,
       -- The file's chunks tile it, so the sum over uploaded xorbs is exactly the part that had to go
       -- up: strictly less than the file's own size means bytes were skipped rather than resent.
       sum(uncompressed_bytes) < (SELECT octet_length(content)
                                  FROM read_blob('/tmp/hf_xet_dedup_edited.csv'))
                                                      AS assert_less_than_the_whole_file,
       bool_and(published)                              AS assert_published
FROM hf_xet_upload('/tmp/hf_xet_dedup_edited.csv',
                   scope := 'hf://buckets/lhoestq/playground/duckdb/xet_dedup_edited.csv', publish := true);

-- A shard mixing pointers and new xorbs is the harder thing for a reader to assemble, and the more
-- ordinary file. It is read back through `hf://` rather than trusted because the upload was accepted.
SELECT 'p8 a shard of pointers and new xorbs reads back' AS t,
       md5(content) = (SELECT md5(content) FROM read_blob('/tmp/hf_xet_dedup_edited.csv')) AS assert_same_bytes
FROM read_blob('hf://buckets/lhoestq/playground/duckdb/xet_dedup_edited.csv');

-- expected failure: the setting is a boolean, and dedup with a threshold would be a different feature
-- expect-fail: Could not convert string 'sometimes' to BOOL
SET hf_xet_upload_dedup = 'sometimes';

SELECT count(*) AS cleaned FROM hf_delete('hf://buckets/lhoestq/playground/duckdb/xet_upload.csv');
SELECT count(*) AS cleaned_again FROM hf_delete('hf://buckets/lhoestq/playground/duckdb/xet_upload_twice.csv');
SELECT count(*) AS cleaned_again FROM hf_delete('hf://buckets/lhoestq/playground/duckdb/xet_copy.csv');
SELECT count(*) AS cleaned_again FROM hf_delete('hf://buckets/lhoestq/playground/duckdb/xet_big.parquet');
SELECT count(*) AS cleaned_again FROM hf_delete('hf://buckets/lhoestq/playground/duckdb/xet_copied.csv');
SELECT count(*) AS cleaned_again FROM hf_delete('hf://buckets/lhoestq/playground/duckdb/xet_dedup.csv');
SELECT count(*) AS cleaned_again FROM hf_delete('hf://buckets/lhoestq/playground/duckdb/xet_dedup_edited.csv');
