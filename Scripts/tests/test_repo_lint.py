"""Negative fixtures for release-blocking repository lint checks."""

import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location(
    "monolith_repo_lint", Path(__file__).resolve().parents[1] / "check_repo_lint.py")
lint = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(lint)


class RepositoryLintTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def write(self, name, contents):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")

    def test_hidden_json_template_is_parsed_and_rejected_when_invalid(self):
        self.write("Templates/.mcp.json.example", '{"mcpServers": {}}')
        self.write("Templates/AGENTS.md.example", "Not JSON")
        self.assertEqual(lint.parse_templates(self.root), [".mcp.json.example"])
        self.write("Templates/.mcp.json.example", "{invalid")
        with self.assertRaises(ValueError):
            lint.parse_templates(self.root)

    @unittest.skipIf(sys.version_info < (3, 11), "TOML lint runs in the Python 3.12 CI job")
    def test_toml_template_is_parsed_and_rejected_when_invalid(self):
        self.write("Templates/codex.toml.example", '[mcp_servers.monolith]\ncommand = "proxy"')
        self.assertEqual(lint.parse_templates(self.root), ["codex.toml.example"])
        self.write("Templates/codex.toml.example", "[invalid")
        with self.assertRaises(ValueError):
            lint.parse_templates(self.root)

    def version_fixture(self):
        self.write("Monolith.uplugin", '{"VersionName": "0.22.0"}')
        self.write("Source/MonolithCore/Public/MonolithCoreModule.h", '#define MONOLITH_VERSION TEXT("0.22.0")')
        self.write("Docs/API_REFERENCE.md", "# API\n\n**Version:** v0.22.0")

    def test_version_mismatch_and_missing_marker_fail(self):
        self.version_fixture()
        self.assertEqual(lint.check_versions(self.root), "0.22.0")
        for text in ("**Version:** v0.21.0", "missing version"):
            self.write("Docs/API_REFERENCE.md", text)
            with self.assertRaises(ValueError):
                lint.check_versions(self.root)

    def test_bad_source_category_and_non_ascii_powershell_fail(self):
        self.write("Source/Foo.cpp", "UE_LOG(" + "LogTemp, Log, TEXT(\"bad\"));")
        self.write("Scripts/build.ps1", "# non-ascii: \u00f6")
        failures = lint.check_source_hygiene(self.root)
        self.assertEqual(len(failures), 2)
        self.assertTrue(any("Foo.cpp" in item for item in failures))
        self.assertTrue(any("build.ps1" in item for item in failures))

    def test_generated_code_strings_and_comments_are_not_linted(self):
        # Code generators emit UE_LOG(LogTemp, ...) for the user's project as
        # string literals; commented history is not plugin logging either.
        generated = (
            'Cpp += TEXT("\\tUE_LOG(' + 'LogTemp, Log, TEXT(\\"hi\\"));\\n");\n'
            '// was ' + 'LogTemp before the LogMonolith migration\n'
            '/* multi-line\n ' + 'LogTemp */\n'
            'UE_LOG(LogMonolith, Log, TEXT("ok"));\n')
        self.write("Source/Gen.cpp", generated)
        self.assertEqual(lint.check_source_hygiene(self.root), [])
        self.assertTrue(lint.uses_log_temp('DEFINE_LOG_CATEGORY_STATIC(' + 'LogTemp, Log, All);'))
        self.assertFalse(lint.uses_log_temp('TEXT("' + 'LogTemp")'))
        self.assertFalse(lint.uses_log_temp('LogTemplate'))

    def test_private_guard_uses_index_even_for_ignored_files(self):
        self.version_fixture()
        self.write("Templates/.mcp.json.example", "{}")
        self.write(".gitignore", "# Internal-only docs\nDocs/ROADMAP.md\nDocs/research/\n\n# Build outputs\nTools/\n")
        self.write("Docs/ROADMAP.md", "private")
        self.write("Tools/public.cpp", "public source")
        subprocess.run(["git", "init", "-q"], cwd=self.root, check=True)
        subprocess.run(["git", "add", "-f", "Docs/ROADMAP.md", "Tools/public.cpp"], cwd=self.root, check=True)
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            result = lint.main(["--root", str(self.root), "--skip-skill-actions"])
        self.assertEqual(result, 1)
        self.assertIn("Tracked private path: Docs/ROADMAP.md", output.getvalue())
        self.assertNotIn("Tracked private path: Tools/", output.getvalue())
        subprocess.run(["git", "rm", "--cached", "-q", "Docs/ROADMAP.md"], cwd=self.root, check=True)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(lint.main(["--root", str(self.root), "--skip-skill-actions"]), 0)
        self.assertEqual(lint.tracked_private_paths(["Docs/research/secret.md", "Docs/researcher.md"],
            lint.private_patterns((self.root / ".gitignore").read_text())), ["Docs/research/secret.md"])


if __name__ == "__main__":
    unittest.main()
