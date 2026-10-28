#!/usr/bin/env python3
"""A datasets-server `/parquet` route that answers with canned bodies, for scripts/smoke_datasets.sql.

What hf_dataset() refuses to do is about the state a dataset is in on the Hub - a conversion that covers
part of it, shards still pending, shards that failed - and none of that can be provoked on demand: a
dataset stays half converted for exactly as long as it takes to convert, and then never again. A suite
that tested those branches against the production service would pass or fail depending on the hour. This
answers the same route with fixed bodies instead.

The file urls inside those bodies are fetched from the production service and passed through unchanged, so
the reads that a fixture enables go on fetching real files through the real `hf://` path. What is canned is
the listing, never the bytes.

  python3 tools/hf_datasets_server_fixture.py --port 8780
  curl 'http://127.0.0.1:8780/parquet?dataset=fixture/partial'
"""

import argparse
import json
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer

UPSTREAM = "https://datasets-server.huggingface.co/parquet"


def listing(dataset):
    """What the production service says about `dataset`, so that the urls a fixture hands out resolve."""
    with urllib.request.urlopen(UPSTREAM + "?dataset=" + urllib.parse.quote(dataset)) as response:
        return json.load(response)


def partial(body):
    """Half a conversion: two shards listed, one still pending, one that failed."""
    shards = body["parquet_files"]
    body["parquet_files"] = shards[:2]
    body["partial"] = True
    # the two missing shards, in the shape the service itself uses for them: `pending` is a shard the
    # conversion has not produced yet, `failed` one that did not convert. Only the counts are asserted,
    # so they are cloned from a real shard rather than spelled out
    missing = shards[2] if len(shards) > 2 else dict(shards[0])
    body["pending"] = [dict(missing, split="pending-split")]
    body["failed"] = [dict(missing, split="failed-split")]
    return body


def external(body):
    """A dataset whose files do not live on the Hub: the url is whatever the answer says it is."""
    body["parquet_files"] = body["parquet_files"][:1]
    body["parquet_files"][0]["url"] = "https://files.example.invalid/converted/train/0000.parquet"
    return body


def refused():
    """A refusal the service delivers as a 200, with the reason in the body."""
    return {"error": "the conversion of this dataset is queued and has not produced any file yet"}


#: dataset name -> what to answer for it. Names with no entry fall through to the production service.
FIXTURES = {
    "fixture/partial": lambda: partial(listing("lhoestq/demo1")),
    "fixture/external": lambda: external(listing("lhoestq/demo1")),
    "fixture/refused": refused,
}


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/health":
            # the harness asks this once to know the fixture is listening, rather than sleeping and hoping
            self.reply(200, b"{}")
            return
        query = urllib.parse.parse_qs(parsed.query)
        dataset = (query.get("dataset") or [""])[0]
        make = FIXTURES.get(dataset)
        try:
            body = make() if make else listing(dataset)
        except urllib.error.HTTPError as error:
            # what the production service answers for that dataset, answered here - the suites get to see
            # the same 404 for an unknown dataset whether or not a fixture is standing in for the service
            self.reply(error.code, error.read())
            return
        except Exception as error:  # a broken fixture has to read as a 500, not as a dropped connection
            self.send_json(500, {"error": f"fixture: {type(error).__name__}: {error}"})
            return
        self.send_json(200, body)

    def reply(self, status, payload):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def send_json(self, status, body):
        # compact, the way the service writes it. HFJson reads both spellings, so this is fidelity rather
        # than necessity - but a fixture that answers differently from the thing it stands in for teaches
        # the wrong lesson
        self.reply(status, json.dumps(body, separators=(",", ":")).encode())

    def log_message(self, *args):
        pass  # the suite's transcript is the log


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    arguments = parser.parse_args()
    server = HTTPServer(("127.0.0.1", arguments.port), Handler)
    print(f"datasets-server fixture on http://127.0.0.1:{arguments.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
