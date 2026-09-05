#!/usr/bin/env python3
"""Check the documented action surface without requiring an editor.

Recognizes backticked namespace.action (also namespace_query.action), backticked
names in Action table columns, and namespace_query("action") / object-style
query examples. Bare table names are checked against all namespaces; qualify a
name to also check its namespace. Other inline code can be parameters, types,
CVars or paths, so it is not assumed to be an action. This checks names, not
parameter schemas or availability of optional plugins in a particular editor.
"""

from __future__ import annotations

import argparse
from bisect import bisect_right
from pathlib import Path
import re
import sys


IDENTIFIER = r"[A-Za-z_][A-Za-z0-9_]*"
CPP_TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.S)
REGISTER_START = re.compile(r"\bRegisterAction\s*\(")
REGISTER_NAME = re.compile(
    rf'\s*TEXT\s*\(\s*"({IDENTIFIER})"\s*\)\s*,\s*'
    rf'(?:TEXT\s*\(\s*"({IDENTIFIER})"\s*\)|({IDENTIFIER})\s*::\s*GetName\s*\(\s*\))\s*,')
QUALIFIED = re.compile(rf"`([a-z][a-z0-9_]*(?:_query)?)\.({IDENTIFIER})`")
BARE = re.compile(rf"`({IDENTIFIER})`")
QUERY_CALL = re.compile(
    rf'\b([a-z][a-z0-9_]*)_query\s*\(\s*'
    rf'(?:\{{\s*(?:["\']action["\']|action)\s*:\s*)?["\']({IDENTIFIER})["\']')


def strip_comments(source: str) -> str:
    """Preserve quoted strings and line numbers while removing C++ comments."""
    return CPP_TOKEN.sub(lambda m: re.sub(r"[^\n]", " ", m[0])
                         if m[0].startswith(("//", "/*")) else m[0], source)


def registered_actions(root: Path) -> set[tuple[str, str]]:
    sources = {}
    for path in sorted((root / "Source").rglob("*")):
        if path.suffix in (".h", ".cpp") and not any(part.lower() == "tests" for part in path.parts):
            sources[path] = strip_comments(path.read_text(encoding="utf-8-sig"))

    # Project actions register Class::GetName(), whose definitions are inline.
    names = {}
    for source in sources.values():
        for cls in re.finditer(rf"\bclass\s+({IDENTIFIER})\s*\{{(.*?)\}};", source, re.S):
            method = re.search(r'\bGetName\s*\(\s*\)\s*\{\s*return\s+TEXT\s*\(\s*"(' +
                               IDENTIFIER + r')"\s*\)\s*;', cls[2])
            if method:
                names[cls[1]] = method[1]

    actions = set()
    for path, source in sources.items():
        if path.suffix != ".cpp":
            continue
        strings = [(match.start(), match.end()) for match in CPP_TOKEN.finditer(source)]
        string_starts = [start for start, _ in strings]
        for start in REGISTER_START.finditer(source):
            quoted = bisect_right(string_starts, start.start()) - 1
            if quoted >= 0 and start.start() < strings[quoted][1]:
                continue
            # Ignore the registry method definition itself.
            if source[max(0, start.start() - 2):start.start()] == "::":
                continue
            match = REGISTER_NAME.match(source, start.end())
            line = source.count("\n", 0, start.start()) + 1
            if not match:
                raise ValueError(f"{path.relative_to(root)}:{line}: unsupported RegisterAction name expression")
            namespace, literal, cls = match.groups()
            action = literal or names.get(cls)
            if action is None:
                raise ValueError(f"{path.relative_to(root)}:{line}: cannot resolve {cls}::GetName()")
            actions.add((namespace, action))
    if not actions:
        raise ValueError("No RegisterAction calls found under Source/")
    return actions


def skill_references(source: str):
    """Yield (line, namespace-or-None, action), without treating params as actions."""
    for match in QUALIFIED.finditer(source):
        namespace, action = match.groups()
        # Object member notation is transport metadata, not namespace dispatch.
        if namespace in ("params", "arguments", "error", "result"):
            continue
        if namespace.endswith("_query"):
            namespace = namespace[:-6]
        yield source.count("\n", 0, match.start()) + 1, namespace, action
    for match in QUERY_CALL.finditer(source):
        yield source.count("\n", 0, match.start()) + 1, match[1], match[2]

    action_column = None
    for line_number, line in enumerate(source.splitlines(), 1):
        if not line.lstrip().startswith("|"):
            action_column = None
            continue
        cells = line.strip().strip("|").split("|")
        headers = [cell.strip().lower() for cell in cells]
        if "action" in headers:
            action_column = headers.index("action")
            continue
        if action_column is not None and action_column < len(cells):
            for match in BARE.finditer(cells[action_column]):
                yield line_number, None, match[1]


def check(root: Path) -> tuple[list[str], int]:
    actions = registered_actions(root)
    bare_names = {action for _, action in actions}
    errors = []
    checked = 0
    skills = sorted((root / "Skills").rglob("*.md"))
    if not skills:
        raise ValueError("No Markdown files found under Skills/")
    for path in skills:
        source = path.read_text(encoding="utf-8-sig")
        for line, namespace, action in sorted(set(skill_references(source)), key=lambda item: (item[0], str(item[1]), item[2])):
            checked += 1
            exists = (namespace, action) in actions if namespace else action in bare_names
            if not exists:
                name = f"{namespace}.{action}" if namespace else action
                errors.append(f"{path.relative_to(root).as_posix()}:{line}: unknown action {name}")
    return errors, checked


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args(argv)
    try:
        errors, count = check(args.root)
    except (OSError, ValueError) as exc:
        print(f"Skill action check failed: {exc}", file=sys.stderr)
        return 1
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"Skill action check passed: {count} references verified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
