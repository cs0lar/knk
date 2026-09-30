#!/usr/bin/env python3
"""Reference reader for a knk spilled query result (see docs/storage_format.md).

The point of this file is that it is short. A spill is one fixed-width, native-endian file per column
with no headers, fully described by `descriptor.json`, precisely so that reading it needs no library and
no knk code -- about twenty lines of the real work below. knk writes its own format rather than Arrow IPC
because Arrow's metadata is a FlatBuffer that could not be produced correctly without an Arrow
implementation to verify against, which neither the kernel's build nor its CI has; converting to Arrow
here, where pyarrow is installed and tested, is a few lines.

    python3 tools/read_spill.py /path/to/spills/result-1                 # summary + first rows
    python3 tools/read_spill.py /path/to/spills/result-1 --arrow         # to a pyarrow Table
"""

import argparse
import json
import pathlib
import struct
import sys

# descriptor "type" -> (struct format character, size). Native-endian little, as the descriptor states.
FORMATS = {"uint64": ("<Q", 8), "int64": ("<q", 8), "double": ("<d", 8), "uint8": ("<B", 1)}

STATUS_NAMES = ["Active", "Superseded", "Retracted", "Retraction", "Hypothesis"]


def read_spill(directory):
    """Returns (descriptor, {column_name: [values]}, dictionary)."""
    directory = pathlib.Path(directory)
    descriptor = json.loads((directory / "descriptor.json").read_text())

    if descriptor.get("format") != "knk-columnar-result":
        raise ValueError(f"not a knk spill: {descriptor.get('format')!r}")

    columns = {}
    for column in descriptor["columns"]:
        code, size = FORMATS[column["type"]]
        raw = (directory / column["file"]).read_bytes()
        expected = descriptor["rows"] * size
        if len(raw) != expected:
            raise ValueError(f"{column['file']}: expected {expected} bytes, found {len(raw)}")
        columns[column["name"]] = [value[0] for value in struct.iter_unpack(code, raw)]

    dictionary = {}
    dictionary_file = descriptor.get("dictionary")
    if dictionary_file and (directory / dictionary_file).exists():
        dictionary = json.loads((directory / dictionary_file).read_text())

    return descriptor, columns, dictionary


def to_arrow(descriptor, columns):
    """The whole Arrow story, on the consumer's side where a real Arrow implementation exists."""
    import pyarrow  # noqa: F401  (imported lazily: the reader above needs nothing installed)

    return pyarrow.table({name: values for name, values in columns.items()})


def label(dictionary, kind, identifier):
    entry = dictionary.get(kind, {}).get(str(identifier))
    if entry is None:
        return str(identifier)
    if isinstance(entry, dict):  # an entity: a tagged {kind, value}
        return str(entry.get("value"))
    return str(entry)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("directory", help="a spill directory, containing descriptor.json")
    parser.add_argument("--rows", type=int, default=10, help="how many rows to print (default 10)")
    parser.add_argument("--arrow", action="store_true", help="load into a pyarrow Table and print its schema")
    arguments = parser.parse_args()

    descriptor, columns, dictionary = read_spill(arguments.directory)

    print(f"{descriptor['rows']} rows, {len(descriptor['columns'])} columns, token {descriptor['token']}")

    if arguments.arrow:
        table = to_arrow(descriptor, columns)
        print(table.schema)
        return 0

    print(f"{'subject':<20} {'predicate':<16} {'object':<20} {'status':<12} confidence")
    for row in range(min(arguments.rows, descriptor["rows"])):
        print(
            f"{label(dictionary, 'entities', columns['subject'][row]):<20} "
            f"{label(dictionary, 'predicates', columns['predicate'][row]):<16} "
            f"{label(dictionary, 'entities', columns['object'][row]):<20} "
            f"{STATUS_NAMES[columns['status'][row]]:<12} "
            f"{columns['confidence'][row]:.2f}"
        )

    return 0


if __name__ == "__main__":
    sys.exit(main())
