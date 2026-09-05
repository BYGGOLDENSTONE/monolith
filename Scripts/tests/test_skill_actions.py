"""Offline checks that stale documentation is rejected without confusing params with actions."""

import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "check_skill_actions.py"
SPEC = importlib.util.spec_from_file_location("check_skill_actions", SCRIPT)
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


class SkillActionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.write("Source/Example/Private/Actions.cpp", '''
Registry.RegisterAction(TEXT("logicdriver"), TEXT("runtime_stop_sm"), TEXT("Stop"), Handler);
Registry.RegisterAction(TEXT("ui"), TEXT("set_effect_surface_dropShadow"), TEXT("Shadow"), Handler);
Registry.RegisterAction(TEXT("project"), FProjectSearchAction::GetName(), TEXT("Search"), Handler);
// Registry.RegisterAction(TEXT("logicdriver"), TEXT("runtime_stop"), TEXT("Old"), Handler);
/* Registry.RegisterAction(TEXT("logicdriver"), TEXT("runtime_send_event"), TEXT("Old"), Handler); */
const char* example = "RegisterAction(TEXT(\\"fake\\"), TEXT(\\"fake\\"), Handler)";
''')
        self.write("Source/Example/Private/Search.h", '''
class FProjectSearchAction
{
public:
    static FString GetName() { return TEXT("search"); }
};
''')
        self.write("Source/Example/Private/Tests/Fixture.cpp", '''
Registry.RegisterAction(TestNamespace, DynamicTestAction, Handler);
''')

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def test_registry_resolves_getname_camelcase_and_ignores_comments_strings_tests(self):
        self.assertEqual(CHECKER.registered_actions(self.root), {
            ("logicdriver", "runtime_stop_sm"), ("ui", "set_effect_surface_dropShadow"),
            ("project", "search"),
        })

    def test_recognizes_qualified_table_and_multiline_call_examples(self):
        self.write("Skills/example/SKILL.md", '''
`logicdriver.runtime_stop_sm` and `ui_query.set_effect_surface_dropShadow`.
| Action | Parameters |
|---|---|
| `search` / `runtime_stop_sm` | `asset_path`, `not_an_action` |

project_query("search", {})
logicdriver_query({
  action: "runtime_stop_sm", params: {}
})
ui_query({"action": "set_effect_surface_dropShadow"})
Parameters: `asset_path`, `params._lease_token`, `is_initial`.
''')
        errors, count = CHECKER.check(self.root)
        self.assertEqual(errors, [])
        self.assertEqual(count, 7)

    def test_unknown_names_namespaces_and_wrong_namespace_fail_with_locations(self):
        self.write("Skills/example/SKILL.md", '''`logicdriver.runtime_stop`
`logidriver.runtime_stop_sm`
`ui.runtime_stop_sm`
| Action | Parameters |
|---|---|
| `runtime_send_event` | `asset_path` |

logicdriver_query({action: "runtime_restart"})
''')
        errors, count = CHECKER.check(self.root)
        self.assertEqual(count, 5)
        self.assertEqual(len(errors), 5)
        self.assertIn("Skills/example/SKILL.md:1: unknown action logicdriver.runtime_stop", errors)
        self.assertIn("Skills/example/SKILL.md:6: unknown action runtime_send_event", errors)

    def test_action_column_can_follow_another_column(self):
        self.write("Skills/example/SKILL.md", '''| Purpose | Action | Params |
|---|---|---|
| `description` | `search` | `query` |
''')
        self.assertEqual(CHECKER.check(self.root), ([], 1))

    def test_unresolvable_registration_fails_instead_of_silently_omitting_it(self):
        for expression in ("FUnknownAction::GetName()", "ComputedAction"):
            with self.subTest(expression=expression):
                self.write("Source/Example/Private/Unknown.cpp",
                           'Registry.RegisterAction(TEXT("project"), ' + expression + ', Handler);')
                with self.assertRaises(ValueError):
                    CHECKER.registered_actions(self.root)

    def test_cli_exits_nonzero_for_unknown_action(self):
        self.write("Skills/example/SKILL.md", "`logicdriver.runtime_stop`\n")
        with contextlib.redirect_stderr(io.StringIO()) as output:
            self.assertEqual(CHECKER.main(["--root", str(self.root)]), 1)
        self.assertIn("unknown action logicdriver.runtime_stop", output.getvalue())
        self.write("Skills/example/SKILL.md", "`logicdriver.runtime_stop_sm`\n")
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(CHECKER.main(["--root", str(self.root)]), 0)


if __name__ == "__main__":
    unittest.main()
