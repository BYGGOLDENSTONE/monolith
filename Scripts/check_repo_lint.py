"""Fast repository checks used by CI (run with Python 3.11+ for TOML parsing)."""

import argparse
import fnmatch
import json
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
PRIVATE_SECTIONS = (
    "# Private training actions",
    "# Internal-only docs",
    "# Agent training infrastructure",
)


def private_patterns(ignore_text):
    patterns = []
    selected = False
    for raw in ignore_text.splitlines():
        line = raw.strip()
        if line.startswith("#"):
            selected = line.startswith(PRIVATE_SECTIONS)
        elif selected and line:
            patterns.append(line)
    return patterns


def tracked_private_paths(tracked, patterns):
    return sorted(path for path in tracked if any(
        path.startswith(pattern) if pattern.endswith("/") else fnmatch.fnmatchcase(path, pattern)
        for pattern in patterns))


def parse_templates(root):
    parsed = []
    # iterdir includes dotfiles, unlike a shell's unqualified '*' expansion.
    for path in sorted((root / "Templates").iterdir()):
        if not path.is_file():
            continue
        if ".json" in path.suffixes:
            json.loads(path.read_text(encoding="utf-8"))
        elif ".toml" in path.suffixes:
            import tomllib
            tomllib.loads(path.read_text(encoding="utf-8"))
        else:
            continue
        parsed.append(path.name)
    return parsed


def check_versions(root):
    descriptor = json.loads((root / "Monolith.uplugin").read_text(encoding="utf-8"))["VersionName"]
    header = (root / "Source/MonolithCore/Public/MonolithCoreModule.h").read_text(encoding="utf-8")
    reference = (root / "Docs/API_REFERENCE.md").read_text(encoding="utf-8")
    native = re.search(r'#define\s+MONOLITH_VERSION\s+TEXT\("([^"]+)"\)', header)
    docs = re.search(r'^\*\*Version:\*\*\s+v([^\s]+)', reference, re.M)
    if not native or not docs or descriptor != native.group(1) or descriptor != docs.group(1):
        raise ValueError("Version mismatch: uplugin, MONOLITH_VERSION, and API_REFERENCE must agree")
    return descriptor


CPP_STRINGS_AND_COMMENTS = re.compile(
    r'"(?:\\.|[^"\\\n])*"'      # double-quoted literal
    r"|'(?:\\.|[^'\\\n])*'"     # char literal
    r'|//[^\n]*'                # line comment
    r'|/\*.*?\*/',              # block comment
    re.S)
LOG_TEMP = re.compile(r"\bLogTemp\b")


def uses_log_temp(source):
    """True when plugin code references LogTemp outside strings and comments.

    Code generators legitimately emit `UE_LOG(LogTemp, ...)` inside string
    literals for the user's project; only the plugin's own logging is linted.
    """
    return bool(LOG_TEMP.search(CPP_STRINGS_AND_COMMENTS.sub(" ", source)))


def check_source_hygiene(root):
    failures = []
    for path in sorted((root / "Source").rglob("*")):
        if path.is_file() and path.suffix in (".h", ".cpp", ".inl") and uses_log_temp(
                path.read_text(encoding="utf-8-sig", errors="replace")):
            failures.append("Disallowed logging category: " + str(path.relative_to(root)))
    for path in sorted((root / "Scripts").glob("*.ps1")):
        if any(value > 127 for value in path.read_bytes()):
            failures.append("PowerShell must be ASCII: " + str(path.relative_to(root)))
    return failures


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--skip-skill-actions", action="store_true",
                        help="Local staged preparation only; CI always checks skills")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    failures = []
    for name, check in (("templates", parse_templates), ("versions", check_versions)):
        try:
            print(name + ": " + str(check(root)))
        except (OSError, ValueError, KeyError, ImportError) as error:
            failures.append(name + ": " + str(error))
    failures.extend(check_source_hygiene(root))
    tracked = subprocess.check_output(["git", "ls-files", "-z"], cwd=root).decode("utf-8").split("\0")
    patterns = private_patterns((root / ".gitignore").read_text(encoding="utf-8"))
    failures.extend("Tracked private path: " + path for path in tracked_private_paths(tracked, patterns))
    if not args.skip_skill_actions:
        result = subprocess.run([sys.executable, str(root / "Scripts/check_skill_actions.py")], cwd=root)
        if result.returncode:
            failures.append("Skill action validation failed")
    for failure in failures:
        print(failure, file=sys.stderr)
    print("Repository lint: " + ("FAILED" if failures else "passed"))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
