#!/usr/bin/env python3
"""Publish a built extension repository so that `INSTALL hf FROM '<url>'` works.

    HF_REPO=lhoestq/duckdb-hf-extension scripts/publish_extension_repo.py dist/repo-signed
    HF_REPO=... scripts/publish_extension_repo.py dist/repo-signed --unsigned-tree dist/repo
    HF_BUCKET=duckdb-extensions/builds scripts/publish_extension_repo.py dist/repo-signed --unsigned-tree dist/repo

The result is a plain static file tree — DuckDB's extension repository is only a url layout, so a dataset
repo, a bucket, or any web server all work:

    <root>/<duckdb_version>/<platform>/hf.duckdb_extension{,.gz}
    <root>/.well-known/duckdb-extension-repo.json            (the signing public key, when signed)
    <root>/unsigned/...                                      (optional second tree)

A dataset repo and a bucket are two ways of serving that layout, and the difference is plumbing, not
client: a dataset is a git repository, so its files are served under `/resolve/main/` (a revision), every
upload is a commit, and big files are stored as pointers; a bucket is object storage, served under
`/resolve/` with no revision at all, and `hf buckets sync` writes bytes with no history behind them. A
tree a client walks by name is closer to what a bucket is for, which is why it publishes to
<https://huggingface.co/buckets/duckdb-extensions/builds/resolve>. Publishing never removes anything, from
either target: the dataset repository still serves every file that went into it before the move.

The version directory is not passed in either: it is taken from the tree and then *checked* against
the footer of every binary in it. A repository whose directory does not equal the footer of the
extension it serves is a repository nobody can install from, and the failure is silent from the
builder's side - the first multi-platform tree this script was shown had a windows binary filed under
`v0.1H`, because the script that assembled it had grepped a `strings` listing for something shaped like
a version and found one in mingw metadata. Everything that run reported was green; `INSTALL hf` on
windows would simply have 404'd forever.

Uploading is idempotent (a dataset skips an unchanged commit; a bucket rewrites identical bytes). The
point of running this from CI is that the files below <root> are exactly what a client's `INSTALL` will
fetch, so the tree that is tested locally is the tree that ships.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

import ext_footer
from huggingface_hub import HfApi


def footer_version(tree: str) -> str:
    """The DuckDB version identifier a tree serves, confirmed against every binary inside it.

    Refuses a mixed tree, because `upload_folder` would happily publish both halves and a client only
    ever looks in one of them.
    """
    versions = set()
    for entry in sorted(os.listdir(tree)):
        path = os.path.join(tree, entry)
        if entry.startswith(".") or not os.path.isdir(path):
            continue
        for platform in sorted(os.listdir(path)):
            ext = os.path.join(path, platform, "hf.duckdb_extension")
            if not os.path.isfile(ext):
                continue
            footer = ext_footer.read_footer(ext)
            if footer["version"] != entry:
                raise SystemExit(
                    f"{ext} carries duckdb_version {footer['version']!r} but sits in a directory called "
                    f"{entry!r}; a client asks for the directory and refuses the binary, so this tree "
                    "cannot work - rebuild it with scripts/make_extension_repo.sh"
                )
            versions.add(entry)
    if not versions:
        raise SystemExit(f"no <version>/<platform>/hf.duckdb_extension under {tree} - run "
                         "scripts/make_extension_repo.sh first")
    if len(versions) > 1:
        raise SystemExit(f"{tree} holds binaries for more than one duckdb version: "
                         f"{', '.join(sorted(versions))}")
    return versions.pop()


def hf_cli() -> str:
    """The `hf` CLI, which is what talks to a bucket. A user install lands in ~/.local/bin; a missing one
    should fail with a pointer, not a FileNotFoundError from deep inside a publish."""
    found = shutil.which("hf") or os.path.expanduser("~/.local/bin/hf")
    if not os.path.isfile(found):
        raise SystemExit("publishing to a bucket needs the `hf` CLI: "
                         "https://huggingface.co/docs/huggingface_hub/guides/cli")
    return found


def sync_to_bucket(tree: str, bucket: str, subprefix: str) -> None:
    """`hf buckets sync` a tree into <bucket>[/<subprefix>].

    No `--delete`, and that is load-bearing rather than cautious: one repository can serve more than one
    duckdb version, and syncing a single identifier's tree with deletion would remove the binaries clients
    on another identifier are still downloading. What the bucket holds is its own list, not this tree.
    """
    hf = hf_cli()
    subprocess.run([hf, "buckets", "info", bucket, "--format", "quiet"], check=True, stdout=subprocess.DEVNULL)
    target = f"hf://buckets/{bucket}" + (f"/{subprefix}" if subprefix else "")
    subprocess.run([hf, "buckets", "sync", tree, target, "--format", "quiet"], check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("tree", help="a directory produced by scripts/make_extension_repo.sh")
    parser.add_argument("--unsigned-tree", help="also publish this tree under unsigned/ (no public key needed)")
    parser.add_argument("--repo", default=os.environ.get("HF_REPO"), help="target dataset repo id")
    parser.add_argument("--bucket", default=os.environ.get("HF_BUCKET"),
                        help="target bucket id, e.g. duckdb-extensions/builds — serves /resolve/, no revision")
    parser.add_argument("--private", action="store_true", help="create the repository private (dataset target)")
    parser.add_argument("--commit-message", default="Publish the hf extension build")
    parser.add_argument("--dry-run", action="store_true",
                        help="validate the tree and print the client recipes, upload nothing - the recipes are "
                             "what a release note is written from, and they are worth running before a publish")
    args = parser.parse_args()

    if args.repo and args.bucket:
        raise SystemExit("--repo and --bucket both name a target; pick one")
    if not args.repo and not args.bucket:
        raise SystemExit("no target: set HF_REPO or HF_BUCKET (or pass --repo / --bucket)")
    if args.repo and "/" not in args.repo:
        raise SystemExit(f"'{args.repo}' is not a <namespace>/<name> repository id")
    if args.bucket and "/" not in args.bucket:
        raise SystemExit(f"'{args.bucket}' is not a <namespace>/<name> bucket id")

    # before anything is created or uploaded: a tree that does not agree with its own binaries should
    # fail here rather than half-publish itself
    version = footer_version(args.tree)

    if args.dry_run:
        target = f"hf://buckets/{args.bucket}" if args.bucket else f"dataset {args.repo}"
        extra = f", plus {args.unsigned_tree} under unsigned/" if args.unsigned_tree else ""
        print(f"dry run: {args.tree} would go to {target}{extra}; nothing is uploaded\n")
    elif args.repo:
        api = HfApi()
        api.create_repo(args.repo, repo_type="dataset", private=args.private, exist_ok=True)
        api.upload_folder(
            folder_path=args.tree,
            repo_id=args.repo,
            repo_type="dataset",
            path_in_repo="",
            commit_message=args.commit_message,
        )
    else:
        # a bucket is never created here: a typo in a bucket id must not quietly invent a second one
        try:
            sync_to_bucket(args.tree, args.bucket, "")
        except subprocess.CalledProcessError:
            raise SystemExit(f"{args.bucket} is not readable/writable as you — check "
                             f"`hf buckets info {args.bucket}`, create one with `hf buckets create {args.bucket}`")

    platforms = sorted(
        p for p in os.listdir(os.path.join(args.tree, version)) if os.path.isdir(os.path.join(args.tree, version, p))
    )

    if args.unsigned_tree and not args.dry_run:
        if args.repo:
            api.upload_folder(
                folder_path=args.unsigned_tree,
                repo_id=args.repo,
                repo_type="dataset",
                path_in_repo="unsigned",
                commit_message=args.commit_message + " (unsigned tree)",
            )
        else:
            sync_to_bucket(args.unsigned_tree, args.bucket, "unsigned")

    # DuckDB pastes a '/' of its own between the prefix and <duckdb_version>/<platform>/hf.duckdb_extension,
    # so a prefix ending in a slash asks the hub for '//v2.0.0/...' and the hub answers that with a 404
    # (measured on this bucket: resolve//v2.0.0/linux_arm64/hf.duckdb_extension -> 404, the same URL with a
    # single slash -> 200). Nothing says so where it matters: the client reports only
    # `Failed to install remote extension "hf" from url "..."`, which reads like a key or permission problem
    # and sends someone looking in the wrong direction. The trap stayed invisible because a local file://
    # prefix swallows the doubled slash (scripts/test_install.py:28 has known this since it was written) - the
    # only prefix anyone copies is the https one, and until now the printed recipe was the one form that was
    # never run. So the prefix here is slash-free, and the unsigned tree hangs off it as '/unsigned'.
    if args.repo:
        home = f"https://huggingface.co/datasets/{args.repo}"
        prefix = f"{home}/resolve/main"
    else:
        home = f"https://huggingface.co/buckets/{args.bucket}"
        prefix = f"{home}/resolve"
    key_file = os.path.join(args.tree, ".well-known", "duckdb-extension-repo.json")
    signed = os.path.isfile(key_file)

    print(f"repository   {home}")
    print(f"version      {version}")
    print(f"platforms    {', '.join(platforms)}")
    print(f"signed       {'yes' if signed else 'no'}")
    print()
    for platform in platforms:
        print(f"client, {platform}:")
        if signed:
            compact = "".join(
                line for line in json.load(open(key_file))["signature_keys"][0].splitlines() if not line.startswith("-----")
            ).strip()
            # CREATE OR REPLACE, as the README already said: a repository definition is saved in the client's
            # own configuration (~/.duckdb/extension_repositories/<name>.duckdb_extension.repo.json), not in
            # the home directory, so a second CREATE of the same name fails even in a brand-new home - and the
            # error DuckDB answers with already says to use OR REPLACE.
            print("  SET allow_extension_repositories = 'allowed';")
            print(f"  CREATE OR REPLACE EXTENSION REPOSITORY hf_repo WITH PREFIX '{prefix}' USING PUBLIC KEY '{compact}';")
            print("  INSTALL hf FROM hf_repo; LOAD hf FROM hf_repo;")
        if args.unsigned_tree:
            # The unsigned copy needs unsigned extensions *allowed*, and that is decided when the database
            # opens: the CLI takes -unsigned, the API takes it as connect configuration. Writing it as a SET -
            # which is how it reads, and how this recipe used to read - is refused in a running database, with
            # an error (Cannot change allow_unsigned_extensions setting while database is running) that points
            # nowhere near the tree it is actually about.
            print("  -- unsigned copy, no pinned key: start the client with unsigned extensions allowed, i.e.")
            print("  --   duckdb -unsigned, or duckdb.connect(config={'allow_unsigned_extensions': True}))")
            print(f"  INSTALL hf FROM '{prefix}/unsigned'; LOAD hf;")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
