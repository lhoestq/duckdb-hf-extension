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


def queued(body):
    """Nothing converted yet, and the answer says why.

    Measured on a dataset created for the purpose: `parquet_files` empty, `pending` naming the step in
    flight, `partial` still false. The distinction is worth a route because the right response to it is to
    wait, while the right response to "this dataset has no parquet" is to go and check the name.
    """
    shard = body["parquet_files"][0]
    body["parquet_files"] = []
    body["partial"] = False
    body["pending"] = [{"kind": "config-parquet", "dataset": shard["dataset"], "config": shard["config"],
                        "split": None}]
    body["failed"] = []
    return body


def refused():
    """A refusal the service delivers as a 200, with the reason in the body."""
    return {"error": "the conversion of this dataset is queued and has not produced any file yet"}


class Refusal(Exception):
    """Answer this status and body instead of a listing."""

    def __init__(self, status, body):
        super().__init__(str(body))
        self.status = status
        self.body = body


def private(headers):
    """A dataset that only a token can list.

    The one fixture that tests the *client*: the listing request has to carry the token as a bearer token,
    since that is the only way a private or a gated dataset lists at all. The production service draws the
    same distinction for real - anonymous is a 401 ("not accessible without authentication"), a token that
    cannot see the repo is a 404 ("not accessible with the current credentials"), both measured - so this
    refuses the anonymous case and hands the authenticated one a real listing.
    """
    authorization = headers.get("Authorization") or ""
    if not authorization.startswith("Bearer ") or len(authorization) <= len("Bearer "):
        raise Refusal(401, {"error": "Credentials are required to access this dataset."})
    return listing("lhoestq/demo1")


#: dataset name -> what to answer for it, given the request headers. Names with no entry fall through to
#: the production service.
FIXTURES = {
    "fixture/partial": lambda headers: partial(listing("lhoestq/demo1")),
    "fixture/queued": lambda headers: queued(listing("lhoestq/demo1")),
    "fixture/external": lambda headers: external(listing("lhoestq/demo1")),
    "fixture/refused": lambda headers: refused(),
    "fixture/private": private,
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
            body = make(self.headers) if make else listing(dataset)
        except Refusal as refusal:
            self.send_json(refusal.status, refusal.body)
            return
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
