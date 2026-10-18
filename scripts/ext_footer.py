#!/usr/bin/env python3
"""Read the metadata footer DuckDB appends to a built extension, and print it.

    scripts/ext_footer.py --version   path/to/hf.duckdb_extension   # -> v2.0.0-alpha47600
    scripts/ext_footer.py --platform  path/to/hf.duckdb_extension   # -> windows_amd64_mingw
    scripts/ext_footer.py --extension-version ...                    # -> 0.0.2-pre5
    scripts/ext_footer.py --all ...    # platform, duckdb version, extension version, tab separated

Why this exists instead of `strings | grep '^v[0-9]'`: the version identifier *does* appear in the
footer, but it also appears anywhere a binary happens to contain a string that starts like one. The
first time this script's predecessor ran over a windows artifact, `strings` found `v0.1H` - some
fragment of mingw's metadata, which sorts before `v2.0.0-alpha47600` and matched first - and the
extension was filed under a directory no client will ever ask for. A footer is a structure, so it is
read as one.

The layout is defined by DuckDB's scripts/append_metadata.cmake: a WebAssembly-style custom section
appended to the end of the file, 534 bytes in total -

    1 byte   0            (custom section id)
    3 bytes  varint 147 4 16   (payload length 531, name length 16)
   16 bytes  b"duckdb_signature"
    2 bytes  varint 128 4      (payload length 512)
    8 x 32   NUL-padded metadata fields, appended in REVERSE order:
             field1 "4", field2 platform, field3 duckdb version, field4 extension version,
             field5 ABI type ("CPP"), fields 6-8 empty
  256 bytes  the signature slot, zero-filled until someone signs the file

so everything is at a fixed distance from the end of the file, and the marker is checked rather than
assumed. Field order is why `--platform` is field 2 counted from the signature backwards.
"""

import argparse
import sys

SIG_SIZE = 256
FIELD_SIZE = 32
FIELD_COUNT = 8
SECTION_SIZE = 1 + 3 + 16 + 2 + FIELD_SIZE * FIELD_COUNT + SIG_SIZE
MARKER = b"duckdb_signature"

# counted from the end of the file, in the order the cmake script appends them
FIELDS = {
    "meta1": 1,
    "platform": 2,
    "version": 3,
    "extension-version": 4,
    "abi": 5,
}


class FooterError(Exception):
    pass


def read_footer(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) < SECTION_SIZE:
        raise FooterError(f"{path} is {len(data)} bytes, shorter than the {SECTION_SIZE}-byte footer")
    tail = data[-SECTION_SIZE:]
    if tail[0] != 0 or tail[4:20] != MARKER:
        raise FooterError(
            f"{path} does not end with a duckdb_signature section (found {tail[:24]!r}); "
            "is this a built extension rather than a raw shared library?"
        )
    fields = {}
    for name, index in FIELDS.items():
        # field index 1 is the 32 bytes immediately before the signature, 2 the 32 before that, ... -
        # the cmake script appends META8..META1, so the useful fields arrive backwards
        offset = len(tail) - SIG_SIZE - FIELD_SIZE * index
        raw = tail[offset : offset + FIELD_SIZE]
        fields[name] = raw.split(b"\x00", 1)[0].decode("utf-8", "replace")
    fields["signature"] = data[-SIG_SIZE:]
    fields["signed"] = any(b != 0 for b in fields["signature"])
    return fields


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("path")
    parser.add_argument("--platform", action="store_true")
    parser.add_argument("--version", action="store_true")
    parser.add_argument("--extension-version", action="store_true")
    parser.add_argument("--abi", action="store_true")
    parser.add_argument("--signed", action="store_true", help="exit 0 when the signature slot is not empty")
    parser.add_argument("--all", action="store_true", help="platform, version, extension version (tab separated)")
    args = parser.parse_args()

    try:
        fields = read_footer(args.path)
    except FooterError as exc:
        print(f"ext_footer: {exc}", file=sys.stderr)
        return 1

    if args.signed:
        return 0 if fields["signed"] else 1
    if args.platform:
        print(fields["platform"])
    elif args.version:
        print(fields["version"])
    elif args.extension_version:
        print(fields["extension-version"])
    elif args.abi:
        print(fields["abi"])
    elif args.all:
        print("\t".join([fields["platform"], fields["version"], fields["extension-version"]]))
    else:
        for name in ("platform", "version", "extension-version", "abi", "signed"):
            print(f"{name}\t{fields[name]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
