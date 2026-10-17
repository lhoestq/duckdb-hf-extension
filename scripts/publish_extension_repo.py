#!/usr/bin/env python3
"""Publish a built extension repository onto the Hub so that `INSTALL hf FROM '<url>'` works.

    HF_REPO=lhoestq/duckdb-hf-extension scripts/publish_extension_repo.py dist/repo-signed
    HF_REPO=... scripts/publish_extension_repo.py dist/repo-signed --unsigned-tree dist/repo

The result is a dataset repository holding a plain static file tree (DuckDB's extension repository is
just a url layout, so a dataset, a bucket, or any web server all work):

    <repo>/<duckdb_version>/<platform>/hf.duckdb_extension{,.gz}
    <repo>/.well-known/duckdb-extension-repo.json            (the signing public key, when signed)
    <repo>/unsigned/...                                      (optional second tree)

The version directory is read out of the binary rather than passed in: a repository whose directory
does not equal the footer of the extension it serves is a repository nobody can install from, and the
footer is the only copy of the truth.

Uploading is idempotent (same content, same commit). The point of running this from CI is that the
files below <repo> are exactly what a client's `INSTALL` will fetch, so the tree that is tested locally
is the tree that ships.
"""
import argparse
import json
import os
import sys

from huggingface_hub import HfApi


def footer_version(tree: str) -> str:
    """The DuckDB version identifier the tree is built for, from the binary's own footer."""
    root = os.path.join(tree, ".well-known")
    for entry in sorted(os.listdir(tree)):
        path = os.path.join(tree, entry)
        if entry.startswith("v") and os.path.isdir(path):
            return entry
    raise SystemExit(f"no <version>/ directory under {tree} - run scripts/make_extension_repo.sh first")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("tree", help="a directory produced by scripts/make_extension_repo.sh")
    parser.add_argument("--unsigned-tree", help="also publish this tree under unsigned/ (no public key needed)")
    parser.add_argument("--repo", default=os.environ.get("HF_REPO"), help="target dataset repo id")
    parser.add_argument("--private", action="store_true", help="create the repository private")
    parser.add_argument("--commit-message", default="Publish the hf extension build")
    args = parser.parse_args()

    if not args.repo:
        raise SystemExit("no target repository: set HF_REPO or pass --repo")
    if "/" not in args.repo:
        raise SystemExit(f"'{args.repo}' is not a <namespace>/<name> repository id")

    api = HfApi()
    api.create_repo(args.repo, repo_type="dataset", private=args.private, exist_ok=True)

    version = footer_version(args.tree)
    api.upload_folder(
        folder_path=args.tree,
        repo_id=args.repo,
        repo_type="dataset",
        path_in_repo="",
        commit_message=args.commit_message,
    )

    platforms = sorted(
        p for p in os.listdir(os.path.join(args.tree, version)) if os.path.isdir(os.path.join(args.tree, version, p))
    )

    if args.unsigned_tree:
        api.upload_folder(
            folder_path=args.unsigned_tree,
            repo_id=args.repo,
            repo_type="dataset",
            path_in_repo="unsigned",
            commit_message=args.commit_message + " (unsigned tree)",
        )

    prefix = f"https://huggingface.co/datasets/{args.repo}/resolve/main/"
    key_file = os.path.join(args.tree, ".well-known", "duckdb-extension-repo.json")
    signed = os.path.isfile(key_file)

    print(f"repository   https://huggingface.co/datasets/{args.repo}")
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
            print("  SET allow_extension_repositories = 'allowed';")
            print(f"  CREATE EXTENSION REPOSITORY hf_repo WITH PREFIX '{prefix}' USING PUBLIC KEY '{compact}';")
            print("  INSTALL hf FROM hf_repo; LOAD hf FROM hf_repo;")
        if args.unsigned_tree:
            print("  SET allow_unsigned_extensions = true;   -- the unsigned copy, no pinned key")
            print(f"  INSTALL hf FROM '{prefix}unsigned/'; LOAD hf;")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
