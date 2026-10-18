#!/usr/bin/env python3
"""Verify `INSTALL hf; LOAD hf;` against a locally built extension repository.

    python3 scripts/test_install.py dist/repo                      # unsigned build
    python3 scripts/test_install.py dist/repo-signed --signed      # signature is required
    python3 scripts/test_install.py 'https://huggingface.co/datasets/me/repo/resolve/main/unsigned/'
    python3 scripts/test_install.py 'https://huggingface.co/datasets/me/repo/resolve/main/' --signed

Every run uses a throw-away home directory, so it never touches ~/.duckdb. For signed repositories
the test also flips a byte in the *installed* binary and checks that DuckDB refuses to load it -
otherwise "we signed it" would just mean "we appended 256 bytes somewhere". That refusal is required
of both ways a repository can be registered: with the key pinned in the statement, and keyless - the form
the README gives, where DuckDB reads the key from the repository's own .well-known file.
"""
import json
import os
import subprocess
import sys
import tempfile

repo_arg = sys.argv[1] if len(sys.argv) > 1 else "dist/repo"
signed = "--signed" in sys.argv
# A published repository is verified the same way as a local one - which is the point: the tree that
# passes here is the tree a client will actually fetch. Only the tamper test stays local, since it has
# to modify the served bytes to prove anything.
remote = repo_arg.startswith("http://") or repo_arg.startswith("https://")
repo = repo_arg if remote else os.path.abspath(repo_arg)
# DuckDB appends "/<version>/<platform>/hf.duckdb_extension" itself, so the prefix must not end in a
# slash here: file:// tolerates the doubled one, an https host (rightly) answers 404
prefix = repo.rstrip("/") if remote else "file://" + repo.rstrip("/") + "/"

import duckdb  # noqa: E402

if "--tamper-child" in sys.argv:
    # Runs in its own process (and its own home): install from a repository whose binary was modified
    # after signing, and require DuckDB to refuse it. Never tamper with a *loaded* extension file -
    # truncating a file DuckDB has mapped takes the whole process down with it.
    tampered_home = sys.argv[sys.argv.index("--tamper-child") + 1]
    tampered_repo = sys.argv[sys.argv.index("--tamper-child") + 2]
    # "keyless" reproduces the short form the README tells people to type: CREATE EXTENSION REPOSITORY
    # without USING PUBLIC KEY(S), where DuckDB reads the key from the repository's own .well-known file.
    # It is the form most users will use, so it is the one that has to be shown to refuse a tampered
    # binary - not only the pinned-key form nobody types by hand.
    keyless = len(sys.argv) > sys.argv.index("--tamper-child") + 3
    os.makedirs(os.path.join(tampered_home, ".duckdb"), exist_ok=True)
    with open(os.path.join(tampered_repo, ".well-known/duckdb-extension-repo.json")) as f:
        tampered_keys = json.load(f)["signature_keys"]
    key = "".join(l for l in tampered_keys[0].splitlines() if not l.startswith("-----")).strip()
    kid = duckdb.connect(config={"home_directory": tampered_home, "allow_extension_repositories": "allowed"})
    if keyless:
        kid.execute(f"CREATE EXTENSION REPOSITORY hf_local WITH PREFIX 'file://{tampered_repo}/'")
    else:
        kid.execute(f"CREATE EXTENSION REPOSITORY hf_local WITH PREFIX 'file://{tampered_repo}/' "
                    f"USING PUBLIC KEYS '{key}'")
    kid.execute("INSTALL hf FROM hf_local")
    kid.execute("LOAD hf FROM hf_local")
    # Getting here means the tampered bytes were accepted. Exit 0, deliberately: the parent asserts on a
    # zero return code, and a nonzero one now means only "something refused" - which the parent then
    # checks is actually about the signature. (This used to be SystemExit(<message>), which exits 1, and
    # the parent read any nonzero as a refusal: a child that died of an unrelated cause passed, and one
    # that loaded the tampered file passed too, printing its own accusation as the refusal message.)
    print("LOADED DESPITE TAMPERING")
    sys.exit(0)


version = duckdb.sql("PRAGMA version").fetchall()[0][0]
home = tempfile.mkdtemp(prefix="hf-ext-home-")
os.makedirs(os.path.join(home, ".duckdb"), exist_ok=True)  # DuckDB will not create its home itself

# httpfs is the https transport this extension rides on, so installing known extensions stays enabled
common = {"home_directory": home, "autoinstall_known_extensions": True}

if signed:
    if remote:
        import urllib.request
        with urllib.request.urlopen(prefix.rstrip("/") + "/.well-known/duckdb-extension-repo.json") as f:
            public_keys = json.load(f)["signature_keys"]
    else:
        with open(os.path.join(repo, ".well-known", "duckdb-extension-repo.json")) as f:
            public_keys = json.load(f)["signature_keys"]
    # deliberately NO allow_unsigned_extensions: the signature has to carry the load
    con = duckdb.connect(config={**common, "allow_extension_repositories": "allowed"})
    # the statement is parsed literally (no bind parameters), List(StringLiteral) is a bare comma
    # separated list without parentheses, and DuckDB wants compact keys: the base64 DER
    # SubjectPublicKeyInfo, i.e. the PEM body without its armor.
    compact = lambda pem: "".join(l for l in pem.splitlines() if not l.startswith("-----")).strip()
    key_sql = ", ".join("'" + compact(k) + "'" for k in public_keys)
    con.execute(f"CREATE EXTENSION REPOSITORY hf_local WITH PREFIX '{prefix}' USING PUBLIC KEYS {key_sql}")
    con.execute("INSTALL hf FROM hf_local")
    con.execute("LOAD hf FROM hf_local")
    install_mode = "FROM a trusted repository"
else:
    con = duckdb.connect(config={**common, "allow_unsigned_extensions": True})
    # INSTALL ... FROM '<prefix>' keeps the official repositories in place, so the httpfs transport can
    # still be auto-installed. Setting custom_extension_repository instead would redirect *every*
    # autoinstall to this repository (including httpfs) and break that bootstrap.
    con.execute(f"INSTALL hf FROM '{prefix}'")
    con.execute("LOAD hf")
    install_mode = "unsigned, INSTALL hf FROM '<repo prefix>'"

print("duckdb", version, "|", install_mode)
print("INSTALL/LOAD ok ->", con.execute("SELECT duckdb_version, endpoint, http_transport FROM hf_status()").fetchone())
row = con.execute("SELECT install_mode, installed_from, signature_key_fingerprint FROM duckdb_extensions()"
                  " WHERE extension_name = 'hf'").fetchone()
print("metadata        ->", row)
assert row is not None and row[0] == "REPOSITORY", f"unexpected install metadata: {row}"
if signed:
    # duckdb_extensions().signature_key_fingerprint is not filled for named repositories, so compare
    # against the fingerprint the repository itself pinned (that is the key LOAD verifies against)
    import base64, hashlib
    expected = "sha256:" + hashlib.sha256(base64.b64decode(compact(public_keys[0]))).hexdigest()
    pinned = con.execute("SELECT key_fingerprints FROM duckdb_extension_repositories()"
                         " WHERE repository_name = 'hf_local'").fetchone()[0]
    assert pinned == [expected], f"pinned keys {pinned} != our signing key {expected}"
    # What this line does NOT prove is that a signature was checked - it proves the repository is pinned to
    # our key, which is what makes a signature mean something. That the check happens is what the tamper
    # test below shows, by corrupting the bytes and requiring DuckDB to refuse them.
    print("trusted key     -> repository pins", expected[:24] + "...")
assert con.execute("SELECT count(*) FROM hf_ls('hf://datasets/lhoestq/demo1')").fetchone()[0] > 0
print("usable          -> hf_ls('hf://datasets/lhoestq/demo1') works")

found = subprocess.run(["find", home, "-name", "hf.duckdb_extension"], capture_output=True, text=True).stdout.split()
assert found, "nothing was installed"
installed = found[0].replace(home + "/", "")
assert f"extensions/{version}/" in installed, "installed under the wrong version directory"
print("installed at    ->", installed)

if signed and not remote:
    # A signature that is never verified is worse than none: copy the repository, flip one byte in the
    # copied binary, and require a fresh DuckDB process to refuse it.
    tampered = tempfile.mkdtemp(prefix="hf-ext-tampered-")
    subprocess.run(["cp", "-r", repo, tampered + "/repo"], check=True)
    tampered_repo = tampered + "/repo"
    # Every platform directory, not the first one: this machine installs the binary under its own
    # architecture, and a three-platform tree whose first entry happens to be linux_amd64 used to leave
    # the arm64 bytes - the ones actually installed here - untouched. The test then "passed", and the
    # message it printed was the child's own accusation that nothing was refused.
    corrupted = 0
    for platform in sorted(os.listdir(os.path.join(tampered_repo, version))):
        served = os.path.join(tampered_repo, version, platform)
        if not os.path.isdir(served):
            continue
        # both the plain and the gzipped copy have to be corrupted: DuckDB 2.0 sniffs gzip and may pick
        # either, so tampering only one of them proves nothing
        for name in ("hf.duckdb_extension", "hf.duckdb_extension.gz"):
            path = os.path.join(served, name)
            if not os.path.isfile(path):
                continue
            data = bytearray(open(path, "rb").read())
            data[len(data) // 3] ^= 0xFF
            open(path, "wb").write(bytes(data))
            corrupted += 1
    assert corrupted, f"nothing to tamper with under {tampered_repo}/{version}"
    # Both ways of registering a repository, since they reach the key differently: one pins it in the
    # statement, the other lets DuckDB fetch it from the prefix. Acceptance in one and refusal in the
    # other would make the README's short form the weaker one, quietly.
    for mode in ("pinned", "keyless"):
        tampered_home = tempfile.mkdtemp(prefix="hf-ext-tamper-home-")
        argv = [sys.executable, __file__, tampered_repo, "--signed", "--tamper-child", tampered_home,
                tampered_repo] + ([mode] if mode == "keyless" else [])
        child = subprocess.run(argv, capture_output=True, text=True)
        if child.returncode == 0:
            raise AssertionError(f"a tampered extension loaded cleanly ({mode} repository):\n"
                                 + child.stdout + child.stderr)
        # "something went wrong" is not the same statement as "the signature was the thing that went wrong"
        if "signature" not in child.stderr.lower():
            raise AssertionError(f"the tampered install failed ({mode}) for a reason unrelated to the "
                                 "signature, so it proves nothing:\n" + child.stderr)
        reason = next((line for line in child.stderr.splitlines() if "signature" in line.lower()), "?")
        print(f"tamper check ({mode:7}) -> refused ({corrupted} corrupted):", reason.strip()[:95])

print("OK")
