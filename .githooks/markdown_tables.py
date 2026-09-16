#!/usr/bin/env python3
"""Checks and formats Markdown tables: pipes aligned by display width, and rows within the line limit.

markdown_tables.py --check [--lines "3 4 17"] FILE|-    report misaligned or overwide tables
markdown_tables.py --fix FILE...                        align tables in place, report overwide rows
"""

import argparse
import re
import sys
import unicodedata
from pathlib import Path

LINE_LIMIT = 120
SEPARATOR_CELL = re.compile(r":?-+:?")


def display_width(text: str) -> int:
    """Terminal columns the text occupies, counting wide characters twice and marks not at all."""
    columns = 0
    for character in text:
        if unicodedata.category(character) in ("Mn", "Me", "Cf"):
            continue
        columns += 2 if unicodedata.east_asian_width(character) in ("W", "F") else 1
    return columns


def split_row(line: str) -> list[str]:
    """Cells of a table row, splitting on pipes outside code spans and escapes."""
    body = line.strip()
    body = body.removeprefix("|")
    body = body[:-1] if body.endswith("|") and not body.endswith("\\|") else body
    cells, current, in_code, escaped = [], [], False, False
    for character in body:
        if escaped:
            current.append(character)
            escaped = False
            continue
        if character == "\\":
            current.append(character)
            escaped = True
            continue
        if character == "`":
            in_code = not in_code
        if character == "|" and not in_code:
            cells.append("".join(current).strip())
            current = []
            continue
        current.append(character)
    cells.append("".join(current).strip())
    return cells


def format_table(rows: list[str]) -> list[str]:
    """The rows of one table with every column padded to its widest cell."""
    parsed = [split_row(row) for row in rows]
    columns = max(len(row) for row in parsed)
    parsed = [row + [""] * (columns - len(row)) for row in parsed]
    has_separator = len(parsed) > 1 and all(SEPARATOR_CELL.fullmatch(cell) for cell in parsed[1])
    widths = [3] * columns
    for index, row in enumerate(parsed):
        if has_separator and index == 1:
            continue
        for column, cell in enumerate(row):
            widths[column] = max(widths[column], display_width(cell))
    formatted = []
    for index, row in enumerate(parsed):
        if has_separator and index == 1:
            cells = []
            for column, cell in enumerate(row):
                left, right = cell.startswith(":"), len(cell) > 1 and cell.endswith(":")
                cells.append((":" if left else "") + "-" * (widths[column] - left - right) + (":" if right else ""))
        else:
            cells = [cell + " " * (widths[column] - display_width(cell)) for column, cell in enumerate(row)]
        formatted.append("| " + " | ".join(cells) + " |")
    return formatted


def find_tables(lines: list[str]) -> list[tuple[int, int]]:
    """Half-open line ranges of tables outside fenced code blocks."""
    tables, fenced, start = [], False, None
    for index, line in enumerate(lines):
        if line.lstrip().startswith("```"):
            fenced = not fenced
        is_row = not fenced and line.startswith("|")
        if is_row and start is None:
            start = index
        if not is_row and start is not None:
            tables.append((start, index))
            start = None
    if start is not None:
        tables.append((start, len(lines)))
    return tables


def problems(lines: list[str], touched: set[int] | None) -> list[str]:
    """One message per misaligned table and per overwide row, limited to touched tables when given."""
    messages = []
    for start, end in find_tables(lines):
        if touched is not None and not any(start + 1 <= number <= end for number in touched):
            continue
        rows = lines[start:end]
        if format_table(rows) != rows:
            messages.append(f"line {start + 1}: table cells are not aligned")
        for offset, row in enumerate(format_table(rows)):
            if display_width(row) > LINE_LIMIT:
                messages.append(
                    f"line {start + offset + 1}: table row is {display_width(row)} columns, over {LINE_LIMIT}"
                )
    return messages


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="report problems instead of fixing")
    parser.add_argument("--fix", action="store_true", help="align tables in place")
    parser.add_argument("--lines", default=None, help="space-separated added line numbers to limit the check")
    parser.add_argument("paths", nargs="+")
    arguments = parser.parse_args()
    touched = {int(number) for number in arguments.lines.split()} if arguments.lines else None
    status = 0
    for path in arguments.paths:
        text = sys.stdin.read() if path == "-" else Path(path).read_text(encoding="utf-8")
        lines = text.split("\n")
        if arguments.fix and path != "-":
            for start, end in reversed(find_tables(lines)):
                lines[start:end] = format_table(lines[start:end])
            Path(path).write_text("\n".join(lines), encoding="utf-8")
        for message in problems(lines, touched):
            if arguments.fix and "not aligned" in message:
                continue
            print(f"{path}: {message}" if path != "-" else message)
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
