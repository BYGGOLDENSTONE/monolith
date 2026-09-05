"""Offline proxy seed names must agree with one another and core registrations."""

import importlib.util
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


class SeedCatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location(
            "monolith_seed_contract", ROOT / "Scripts" / "monolith_proxy.py")
        cls.proxy = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.proxy)

    def test_python_native_seed_names_match(self):
        native = (ROOT / "Tools" / "MonolithProxy" / "monolith_proxy.cpp").read_text(encoding="utf-8")
        dispatchers = re.search(r"CORE_QUERY_TOOLS\s*=\s*\{(.*?)\};", native, re.S)
        self.assertIsNotNone(dispatchers)
        native_names = re.findall(r'"([a-z_]+)"', dispatchers.group(1))
        seed_function = native.split("static json make_seed_tools()", 1)[1].split("static void write_cache", 1)[0]
        native_names += re.findall(r'make_tool\(\s*"([a-z_]+)"', seed_function)
        python_names = [tool["name"] for tool in self.proxy._seed_tools()]
        self.assertEqual(len(python_names), len(set(python_names)), "Duplicate Python seed")
        self.assertEqual(len(native_names), len(set(native_names)), "Duplicate native seed")
        self.assertCountEqual(python_names, native_names)

    def test_seed_core_tools_match_actual_registrations(self):
        registered = set()
        for filename in ("MonolithCoreTools.cpp", "MonolithGuideTool.cpp", "MonolithCoordination.cpp"):
            source = (ROOT / "Source" / "MonolithCore" / "Private" / filename).read_text(encoding="utf-8")
            registered.update("monolith_" + action for action in re.findall(
                r'RegisterAction\(\s*TEXT\("monolith"\)\s*,\s*TEXT\("([a-z_]+)"\)', source))
        self.assertTrue(registered, "No core registrations extracted")
        seeds = {tool["name"] for tool in self.proxy._seed_tools() if tool["name"].startswith("monolith_")}
        self.assertEqual(seeds, registered)


if __name__ == "__main__":
    unittest.main()
