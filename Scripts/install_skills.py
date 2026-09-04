#!/usr/bin/env python3
"""Opt-in installation of bundled Monolith skills (Python standard library only)."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import sys


def available_skills(source: Path) -> dict:
    return {p.name: p for p in sorted(source.iterdir())
            if p.is_dir() and not p.is_symlink() and (p / "SKILL.md").is_file()}


def install(source: Path, target: Path, names: list, *, update=False, dry_run=False):
    """Preflight the whole selection before copying; never remove user files."""
    available = available_skills(source)
    unknown = set(names) - set(available)
    if unknown:
        raise ValueError("Unknown skills: " + ", ".join(sorted(unknown)))
    selected = list(dict.fromkeys(names))
    target = target.expanduser().resolve()
    source = source.resolve()
    if target == source or source in target.parents or target in source.parents:
        raise ValueError("Install target must be separate from the bundled Skills directory")
    plan = []
    for name in selected:
        destination = target / name
        if destination.is_symlink() or destination.resolve() != destination:
            raise ValueError("Refusing linked skill destination: " + str(destination))
        if destination.exists() and not destination.is_dir():
            raise ValueError("Destination is not a directory: " + str(destination))
        if destination.exists() and not update:
            raise ValueError("Skill already exists; inspect it and use --update to overwrite bundled files: " + name)
        for path in available[name].rglob("*"):
            if path.is_symlink():
                raise ValueError("Refusing linked source: " + str(path))
            dest = destination / path.relative_to(available[name])
            if dest.is_symlink() or dest.resolve() != dest:
                raise ValueError("Refusing linked destination: " + str(dest))
            if dest.exists() and dest.is_dir() != path.is_dir():
                raise ValueError("File/directory conflict: " + str(dest))
        plan.append((available[name], destination))
    for origin, destination in plan:
        print(("Would install " if dry_run else "Installing ") + str(destination))
        if not dry_run:
            shutil.copytree(origin, destination, dirs_exist_ok=update)
    return len(plan)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("skills", nargs="*", help="Skill folder names to install")
    parser.add_argument("--all", action="store_true", help="Select every bundled skill")
    parser.add_argument("--list", action="store_true", help="List available skills without installing")
    parser.add_argument("--target", type=Path, help="Destination skills directory (default: CODEX_HOME/skills or ~/.codex/skills)")
    parser.add_argument("--update", action="store_true", help="Allow overwriting files of selected installed skills; preserves other files")
    parser.add_argument("--dry-run", action="store_true", help="Validate and print the plan without writing")
    args = parser.parse_args(argv)
    source = Path(__file__).resolve().parent.parent / "Skills"
    if args.list:
        print("\n".join(available_skills(source)))
        return 0
    if args.all and args.skills:
        parser.error("Use skill names or --all, not both")
    names = list(available_skills(source)) if args.all else args.skills
    if not names:
        parser.error("Choose skill names or --all; use --list to inspect choices")
    codex_dir = Path(os.environ.get("CODEX_HOME", str(Path.home() / ".codex")))
    try:
        install(source, args.target or codex_dir / "skills", names,
                update=args.update, dry_run=args.dry_run)
    except (ValueError, OSError) as exc:
        print("Installation failed: " + str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
