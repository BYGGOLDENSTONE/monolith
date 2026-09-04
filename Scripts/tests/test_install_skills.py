import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("install_skills", Path(__file__).resolve().parents[1] / "install_skills.py")
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class SkillInstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "bundled"
        self.target = self.root / "installed"
        for name in ("first", "second"):
            folder = self.source / name
            folder.mkdir(parents=True)
            (folder / "SKILL.md").write_text("bundled", encoding="utf-8")

    def test_selection_and_dry_run(self):
        installer.install(self.source, self.target, ["first"], dry_run=True)
        self.assertFalse(self.target.exists())
        installer.install(self.source, self.target, ["first"])
        self.assertEqual((self.target / "first" / "SKILL.md").read_text(), "bundled")
        self.assertFalse((self.target / "second").exists())

    def test_collision_preflight_and_explicit_update(self):
        installer.install(self.source, self.target, ["second"])
        installed = self.target / "second"
        (installed / "SKILL.md").write_text("custom", encoding="utf-8")
        (installed / "local.txt").write_text("preserve", encoding="utf-8")
        with self.assertRaises(ValueError):
            installer.install(self.source, self.target, ["first", "second"])
        self.assertFalse((self.target / "first").exists())
        self.assertEqual((installed / "SKILL.md").read_text(), "custom")
        installer.install(self.source, self.target, ["second"], update=True)
        self.assertEqual((installed / "local.txt").read_text(), "preserve")
        self.assertEqual((installed / "SKILL.md").read_text(), "bundled")

    def test_unknown_and_source_overlap_rejected(self):
        for target, names in ((self.target, ["../escape"]),
                              (self.source, ["first"]),
                              (self.source / "child", ["first"])):
            with self.assertRaises(ValueError):
                installer.install(self.source, target, names)

    def test_file_conflict_is_preflighted(self):
        (self.target / "second").mkdir(parents=True)
        (self.target / "second" / "SKILL.md").mkdir()
        with self.assertRaises(ValueError):
            installer.install(self.source, self.target, ["first", "second"], update=True)
        self.assertFalse((self.target / "first").exists())

    def test_linked_destination_rejected(self):
        self.target.mkdir()
        external = self.root / "external"
        external.mkdir()
        try:
            (self.target / "first").symlink_to(external, target_is_directory=True)
        except OSError:
            self.skipTest("OS does not permit unprivileged symlinks")
        with self.assertRaises(ValueError):
            installer.install(self.source, self.target, ["first"], update=True)
        self.assertEqual(list(external.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
