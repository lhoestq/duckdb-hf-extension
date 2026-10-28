# Reading a dataset as a table

`hf_dataset('namespace/name')` answers a DuckDB query with the rows of a Hub dataset. It is not the same
decision as `SELECT * FROM 'hf://datasets/me/data/parquet/*.parquet'`, which reads *files someone chose*.
Here the choice is made by the Hub's own conversion service, and the table function is the client of that
service. The whole design follows from that.

## The route, and what it promises

```
GET {hf_datasets_server_endpoint}/parquet?dataset=<ns/name>[&config=<config>][&split=<split>][&revision=<rev>]
```

is documented at <https://huggingface.co/docs/hub/en/datasets-automatic-parquet-conversion> as the *full*
conversion of a dataset: one parquet file per shard of each split, all of them - and the body says whether
that is what it gave (`partial`, plus `pending` and `failed` shard lists). The body is

```json
{ "parquet_files": [ { "dataset", "config", "split", "url", "filename", "size" } ],
  "pending": [...], "failed": [...], "partial": false }
```

`url` is a `https://huggingface.co/datasets/<ns>/<name>/resolve/refs%2Fconvert%2Fparquet/<config>/<split>/…​.parquet`
address. Three things follow from reading that shape rather than trusting it.

## The answer is re-filtered here

A `split=` the route does not recognise is **ignored, not refused**: asking `lhoestq/demo1` for `split=nope`
answers 200 with the files of every split (`demo1` is small enough to sit in one shard per split, so what
comes back is the whole dataset). `config=` is refused, but with a `404 {"error":"Not found."}` that says no
more than "something in this request was wrong". So neither filter is taken on trust: the requested config
and split are re-applied to the parsed answer, and a request that filtered down to nothing says which
configs or splits the dataset actually has. Without the re-filter a typo would quietly read a whole dataset.

## The urls are converted, not passed through

`read_parquet('https://huggingface.co/…')` goes over httpfs: no token, no range-request tuning, no shared
cache. A gated or private dataset answers it with a 403. The rewrite

```
https://huggingface.co/datasets/<ns>/<name>/resolve/refs%2Fconvert%2Fparquet/<config>/<split>/0000.parquet
hf://datasets/<ns>/<name>@refs%2Fconvert%2Fparquet/<config>/<split>/0000.parquet
```

puts the same bytes through this extension's read path with the token from `HF_TOKEN` or
`CREATE SECRET (TYPE HUGGINGFACE, …)`. Verified against the live Hub, in both directions:

| url | result |
|---|---|
| `@refs%2Fconvert%2Fparquet` (percent-encoded, as answered) | reads |
| `@refs/convert/parquet` (decoded) | **404** |

So the revision is kept *as the service spelled it*. `HFUrl::Parse` reads `@refs%2Fconvert%2Fparquet` as one
literal revision, and `HFHFFile::BuildRequestUrl` only encodes `%` → `%25` in the *path*, never in the
revision, so the url leaves this extension the way it arrived. A file path, by contrast, is decoded — a
filename containing `%20` is a file named with a real space, and `/resolve/` encodes it again on the way out.

A url that is not a Hub `/resolve/` url is passed through unchanged: a conversion service can point
anywhere, and `read_parquet` reads an https url over httpfs, unauthenticated, which is all such a url can
promise. `hf_dataset_files` exists so those urls can be inspected before a read is asked of them.

## `partial` is refused, not summarised

`read_parquet` of a converted-list answers with the rows it finds. When `partial` is true, those rows are
**a subset of the dataset, with nothing in the result saying so** — which for `COUNT(*)`, for a join, or for
a model's training set is worse than an error. So `hf_dataset` refuses, names how much is missing (files
ready, shards pending, shards failed), and points at the way to accept it:

```sql
SELECT count(*) FROM hf_dataset('Salesforce/wikitext', config := 'wikitext-103-raw-v1');  -- refuses if half converted
SELECT count(*) FROM hf_dataset(…, allow_partial := true);                                -- reads what is converted
```

`allow_partial` is opt-in per call, has no setting, and is not accepted by `hf_dataset_files` (which lists
`partial` as a column instead).

## One config at a time

Different configs of a dataset have different schemas. `union_by_name` over two of them answers a question
nobody asked with a wide, half-null table, so more than one config is refused with the list of what exists —
unless a `config` was asked for, which is the answer to that question.

`config` and `split` are still *also* sent to the route as query parameters. Not to trust its filtering (the
answer is re-filtered anyway) but because `partial`, `pending` and `failed` are scoped to what was asked:
with one of a dataset's seven configs converted and six not, asking for the converted one must read, while
asking for the whole dataset must not read one config and call it the dataset.

## Telling the two 404s apart

`404 {"error":"Not found."}` is what the route answers both for a config the dataset does not have and for a
dataset that does not exist - and the second is the more likely mistake. Asking once more without the
filters tells them apart: a second failure means the name was the problem, and the Hub's own reason is what
the caller sees (`The dataset does not exist, or is not accessible with the current credentials (private or
gated)…`); a second success means the name is fine and the `config` was not, and the error names the configs
that do exist. The retry is what turns a typo in `config :=` into something to type instead of a bare
`Not found`.

## Refusals that arrive as a 200

For a conversion too large to enumerate, the route answers **200** with `{"error": "The computed response
content exceeds the supported size in bytes (10000000)."}` instead of a status code. An empty `parquet_files`
would read as "0 rows", so a body carrying `error` is thrown with the service's own text. This is the one
case a status-code check cannot see.

Similarly, a 404 with a *body* (`{"error": "The dataset you requested has not been processed yet: we
couldn't find any files matching your request."}`) says the dataset exists but this config/split does not —
the reason is the Hub's own words, so it is quoted in the error rather than paraphrased. A 404 with no
reason gets the generic "the hub has not converted it to parquet" message, which is the honest sentence for
what is actually known then.

## Reading a private or gated dataset

Nothing special: the listing request carries the same token as a read (`HFResolveTarget` + `HFMakeClient`,
no `anonymous: true`), so a dataset the token can see is a dataset `hf_dataset` can list and read. A dataset
the token cannot see answers the listing the same way it answers a made-up name - `HTTP 404 …: The dataset
does not exist, or is not accessible with the current credentials (private or gated)…`, quoted through.
`hf_status()` answers who the token is. A public dataset needs no token for either the listing or the read.

## What `revision` means here

`revision` is the *repository* revision whose conversion is read (`main`, a branch, a commit sha, or a `refs/…`
as the Hub spells it), not the conversion ref. Asking for `main` is equivalent to asking for nothing, and is
not forwarded, so the answer is not labelled with a ref the service would not have chosen. `hf_dataset` reads
one conversion of one config, and `hf_dataset_files` shows the `config`, `split` and `partial` of every file
it could read.

## The endpoint is a setting

`hf_datasets_server_endpoint` (default `https://datasets-server.huggingface.co`) is how a private mirror, or
a test suite that answers with a canned listing, is addressed. Normalized like `hf_endpoint` (scheme added,
trailing slash removed). Global: the value picks a *service*, and the files it lists are then read from
wherever its answer points.

The tests for "half a conversion", "queued, nothing to list" and "refused with a 200" are necessarily against
`tools/hf_datasets_server_fixture.py` — a dataset is half converted for exactly as long as it takes to
convert, so a suite that asked the production service would pass or fail depending on the hour. The fixture
answers three made-up dataset names from fixed bodies and hands everything else to the production service,
so the files it lists are the real ones and the reads still go over `hf://` with the token. What is canned is
the listing, never the bytes.
