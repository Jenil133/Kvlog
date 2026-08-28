#!/usr/bin/env python3
"""Prints kvlog_bench growth CSV files as Markdown tables.

Usage: csv_to_markdown.py FILE.csv [FILE.csv ...] [--columns a,b,c]

One table per file, headed by the file name. Byte columns are shown in MiB
and rates as percentages; other values are printed as they appear in the CSV.
Standard library only.
"""

import argparse
import csv
import os
import sys

DEFAULT_COLUMNS = [
    "step", "dataset_bytes", "disk_bytes", "ops_per_sec", "p50_us", "p99_us",
    "cache_hit_rate", "bloom_useful_rate", "compactions",
]

HEADINGS = {
    "dataset_bytes": "dataset (MiB)",
    "disk_bytes": "on disk (MiB)",
    "block_cache_bytes": "block cache (MiB)",
    "ops_per_sec": "ops/s",
    "p50_us": "p50 (us)",
    "p99_us": "p99 (us)",
    "cache_hit_rate": "cache hits",
    "bloom_useful_rate": "filter skips",
}


def format_cell(column, text):
    if text == "":
        return ""
    try:
        if column.endswith("_bytes"):
            return f"{float(text) / (1 << 20):.1f}"
        if column.endswith("_rate"):
            return f"{100 * float(text):.1f}%"
        if column == "ops_per_sec":
            return f"{float(text):,.0f}"
    except ValueError:
        pass
    return text


def table(path, columns):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        return f"### {os.path.basename(path)}\n\n(no rows)\n"
    missing = [c for c in columns if c not in rows[0]]
    if missing:
        raise SystemExit(f"{path}: no column(s) {', '.join(missing)}")
    lines = [f"### {os.path.basename(path)}", ""]
    lines.append("| " + " | ".join(HEADINGS.get(c, c) for c in columns) + " |")
    lines.append("|" + "|".join("---:" for _ in columns) + "|")
    for row in rows:
        lines.append("| " + " | ".join(format_cell(c, row[c]) for c in columns) + " |")
    return "\n".join(lines) + "\n"


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("files", nargs="+", help="growth CSV files written by kvlog_bench")
    parser.add_argument("--columns", default=",".join(DEFAULT_COLUMNS),
                        help="comma-separated CSV columns to show")
    args = parser.parse_args(argv)
    columns = [c for c in args.columns.split(",") if c]
    print("\n".join(table(path, columns) for path in args.files), end="")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
