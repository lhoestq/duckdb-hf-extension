-- The Xet upload path, against the real CAS.
--
-- hf_xet_upload chunks a file the way Xet does and puts each xorb where the CAS stores it, addressed
-- by the hash of its chunks. A successful upload is therefore two checks at once: the CAS recomputes
-- the hash from the bytes it received and refuses a mismatch, and it reports whether the content was
-- new. Nothing here is stubbed or simulated, and nothing here can pass with a writer that produces
-- bytes the real service cannot read (docs/XET.md, M3 + M4).
--
-- What it does not do is make a file readable: content only becomes a file when a metadata shard
-- refers to it, which is the next milestone. So this suite deletes its git-side fixture afterwards -
-- the chunks stay in the CAS, which is what content-addressed storage is for.

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
