"""Offline risk reads must follow committed mining snapshots without hiding legacy data."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("monolith_offline_risk_test", ROOT / "Scripts/monolith_offline.py")
offline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(offline)


class RiskSnapshotContract:
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="monolith-risk-routing-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.source = self.root / "EngineSource.db"
        self.risk = self.root / "Risk.db"

    def database(self, path, score=None):
        with contextlib.closing(sqlite3.connect(path)) as db:
            db.execute("CREATE TABLE reflect_uclasses(class_name TEXT, module_name TEXT, parent_class TEXT, source_path TEXT, source_line INTEGER, flags TEXT)")
            if score is not None:
                db.execute("CREATE TABLE risk_hotspot_scores(file_path TEXT, churn INTEGER, complexity_proxy INTEGER, normalised_churn REAL, normalised_complexity REAL, score REAL)")
                db.execute("INSERT INTO risk_hotspot_scores VALUES ('Source/Fixture.cpp', 2, 10, 1.0, ?, ?)", (score, score))
            db.commit()

    def test_risk_prefers_dedicated_snapshot(self):
        self.database(self.source, 0.1)
        self.database(self.risk, 0.9)
        self.assertEqual(self.query("risk", "get_hotspot_score")["hotspot"]["score"], 0.9)

    def test_risk_falls_back_to_legacy_source_database(self):
        self.database(self.source, 0.1)
        self.assertEqual(self.query("risk", "get_hotspot_score")["hotspot"]["score"], 0.1)

    def test_dedicated_risk_does_not_require_source_index(self):
        self.database(self.risk, 0.9)
        self.assertEqual(self.query("risk", "get_hotspot_score")["hotspot"]["score"], 0.9)
        self.assertFalse(self.source.exists())

    def test_other_reflection_namespace_keeps_source_database(self):
        self.database(self.source)
        self.risk.write_bytes(b"This risk snapshot must not be opened by cppreflect")
        self.assertEqual(self.query("cppreflect", "get_uclass"), {"success": True, "uclass": None})

    def test_unmined_dedicated_database_never_silently_uses_old_scores(self):
        self.database(self.source, 0.1)
        self.database(self.risk)
        result = self.query("risk", "get_hotspot_score")
        self.assertIs(result["success"], False)
        self.assertEqual(result["error"], "risk_hotspot_scores not mined. Run risk.mine in-editor and wait for risk.get_mining_status state done.")


class PythonRiskSnapshotTests(RiskSnapshotContract, unittest.TestCase):
    def query(self, namespace, action):
        args = SimpleNamespace(file_path="Source/Fixture.cpp", class_name="Absent", module_name="")
        output = io.StringIO()
        with patch.object(offline, "SOURCE_DB", self.source), contextlib.redirect_stdout(output):
            reader = offline.ReflectionActions(namespace)
            try:
                getattr(reader, action)(args)
            finally:
                reader.db.close()
        return json.loads(output.getvalue())


@unittest.skipUnless(os.environ.get("MONOLITH_TEST_NATIVE_QUERY"),
                     "Set MONOLITH_TEST_NATIVE_QUERY to test native offline risk routing")
class NativeRiskSnapshotTests(RiskSnapshotContract, unittest.TestCase):
    def query(self, namespace, action, source_override=None):
        value = "Source/Fixture.cpp" if namespace == "risk" else "Absent"
        command = [os.environ["MONOLITH_TEST_NATIVE_QUERY"], namespace, action, value, "--db=" + str(self.root)]
        if source_override is not None:
            command.append("--source_db=" + str(source_override))
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_explicit_source_database_override_is_honored(self):
        self.database(self.source, 0.1)
        self.database(self.risk, 0.9)
        self.assertEqual(self.query("risk", "get_hotspot_score", self.source)["hotspot"]["score"], 0.1)


if __name__ == "__main__":
    unittest.main()
