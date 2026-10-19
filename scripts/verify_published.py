#!/usr/bin/env python3
"""Verify that what a client downloads is byte-for-byte what CI built.

    HF_REPO=lhoestq/duckdb-hf-extension scripts/verify_published.py dist/repo-signed
    HF_REPO=... scripts/verify_published.py dist/repo-signed --unsigned-tree dist/repo

The argument list is deliberately the same as `scripts/publish_extension_repo.py`'s — same trees, same
`--unsigned-tree` prefix — so the verification command is the publish command with `verify_` in front of
it, and a release that was published cannot be verified against a different tree by accident.

Why this exists: a publish uploads files, and a Hub dataset serves files. Neither of those facts says
anything about whether the tree a client will walk is the tree CI produced, or whether a binary landed in
the directory its own footer names. Every check here is against the *served* bytes:

  * every file in each tree is fetched over https and its sha256 compared with the local file's, so an
    upload that silently truncated, skipped, or stale-served a binary fails here rather than on a client;
  * the footer of every served `hf.duckdb_extension` is parsed with `scripts/ext_footer.py` and must agree
    with the directory it is served from — platform from the path component, `duckdb_version` likewise —
    which is the failure `strings | grep '^v[0-9]'` once hid behind a mingw metadata fragment;
  * the `.well-known/duckdb-extension-repo.json` of the signed tree must be served too, since a client
    that pins a key needs to fetch it.

Nothing here installs anything. `scripts/test_install.py` does that, over the same url, and is the next
command after this one.
"""

import argparse
import hashlib
import os
import sys
import tempfile

import ext_footer

BASE = "https://huggingface.co/datasets/"


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch(url):
    """Download to a temp file. Returns (path, sha256, bytes) — the temp file exists because the
    footer lives at the end of the binary and ext_footer reads a path, not a buffer.

    The size is taken after the file is closed. Before that fix this function reported a 499-byte file as
    0 bytes — buffered writes had not reached the disk — and the first run of this script reported the
    signed tree's key file as a truncated upload. The hash was never wrong, because it is updated per
    block; only the stat was early.
    """
    import urllib.request

    digest = hashlib.sha256()
    with tempfile.NamedTemporaryFile(suffix=".duckdb_extension", delete=False) as out:
        path = out.name
        request = urllib.request.Request(url, headers={"User-Agent": "hf-extension-verify"})
        with urllib.request.urlopen(request, timeout=120) as response:
            for block in iter(lambda: response.read(1 << 20), b""):
                digest.update(block)
                out.write(block)
    return path, digest.hexdigest(), os.path.getsize(path)


def walk(tree):
    for directory, _dirs, files in os.walk(tree):
        for name in sorted(files):
            full = os.path.join(directory, name)
            yield os.path.relpath(full, tree).replace(os.sep, "/"), full


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("tree", help="the signed tree, as published at the repository root")
    parser.add_argument("--unsigned-tree", help="the same tree's unsigned copy, published under unsigned/")
    parser.add_argument("--repo", default=os.environ.get("HF_REPO"), help="target dataset repo id")
    args = parser.parse_args()
    if not args.repo:
        raise SystemExit("no target repository: set HF_REPO or pass --repo")

    root = BASE + args.repo + "/resolve/main/"
    failures = 0
    checked = 0
    for tree, remote_prefix in ((args.tree, ""), (args.unsigned_tree, "unsigned/")):
        if tree is None:
            continue
        if not os.path.isdir(tree):
            raise SystemExit("no such tree: %s" % tree)
        print("== %s -> %s%s" % (tree, root, remote_prefix))
        for rel, local in sorted(walk(tree)):
            url = root + remote_prefix + rel
            try:
                path, remote_sha, remote_size = fetch(url)
            except Exception as error:  # only a 404 is "absent"; anything else is reported as itself, so a
                # stalled download never reads as a missing file (the same reason HFClient::GetStatus exists)
                import urllib.error

                kind = "MISSING" if isinstance(error, urllib.error.HTTPError) and error.code == 404 else "FAILED"
                print("  %-8s %-58s %s" % (kind, remote_prefix + rel, error))
                failures += 1
                continue
            local_sha, local_size = sha256_of(local), os.path.getsize(local)
            if remote_sha != local_sha or remote_size != local_size:
                print("  MISMATCH %-58s local %s/%d served %s/%d"
                      % (remote_prefix + rel, local_sha[:12], local_size, remote_sha[:12], remote_size))
                failures += 1
            else:
                print("  ok       %-58s %10d bytes  %s" % (remote_prefix + rel, local_size, local_sha[:12]))
            checked += 1

            # A binary's directory is a promise about its footer, checked on the served bytes. The
            # key file is metadata, not an extension, so it is skipped here.
            if os.path.basename(local) == "hf.duckdb_extension":
                footer = ext_footer.read_footer(path)
                want_platform = rel.split("/")[-2]  # the platform directory, not the unsigned/ prefix
                want_version = rel.split("/")[-3]
                if footer["platform"] != want_platform or footer["version"] != want_version:
                    print("  FOOTER   %s serves platform=%s duckdb_version=%s, its directory says %s/%s"
                          % (remote_prefix + rel, footer["platform"], footer["version"], want_version, want_platform))
                    failures += 1
                else:
                    print("           footer: %s / %s / ext %s / signed=%s"
                          % (footer["platform"], footer["version"], footer["extension-version"], footer["signed"]))
            os.unlink(path)

    print("\n%d file(s) fetched, %d problem(s)" % (checked, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
