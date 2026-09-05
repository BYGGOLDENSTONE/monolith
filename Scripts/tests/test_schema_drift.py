"""Source parser and command-line enforcement regressions for schema drift."""
import importlib.util
import sys
import json
import subprocess
import tempfile
import unittest
from unittest import mock
from pathlib import Path

SPEC = importlib.util.spec_from_file_location('schema_drift', Path(__file__).resolve().parents[1] / 'check_schema_drift.py')
LINT = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = LINT
SPEC.loader.exec_module(LINT)


def source(body='', schema='FParamSchemaBuilder().Build()', handler='FMonolithActionHandler::CreateStatic(&A::Handle)', args='const TSharedPtr<FJsonObject>& Params'):
    return f'''FMonolithActionResult A::Handle({args}) {{ {body} }}
void A::Register() {{ R.RegisterAction(TEXT("unit"), TEXT("action"), TEXT("description"), {handler}, {schema}); }}'''


class SchemaDriftTests(unittest.TestCase):
    def run_source(self, text, forwarding=None):
        return LINT.Checker({'Source/Module/Test.cpp': text}, forwarding).run()

    def test_literal_direct_reads_and_aliases(self):
        schema = 'FParamSchemaBuilder().RequiredAssetPath(TEXT("path"), TEXT("p"), {TEXT("asset")}).Optional(TEXT("count"), TEXT("integer"), TEXT("c"), TEXT("0"), {TEXT("max")}).Build()'
        result = self.run_source(source('Params->GetStringField(TEXT("asset")); Params->TryGetNumberField(TEXT("max"), N);', schema))
        self.assertEqual(result['errors'], [])
        self.assertEqual(result['registrations'][0]['keys'], ['asset', 'count', 'max', 'path'])

    def test_undeclared_literal_fails(self):
        result = self.run_source(source('Params->HasField(TEXT("hidden"));'))
        self.assertEqual(result['registrations'][0]['missing'], ['hidden'])

    def test_nested_json_is_not_promoted(self):
        result = self.run_source(source('Params->TryGetArrayField(TEXT("nodes"), Nodes); Entry->HasField(TEXT("position")); Result->GetStringField(TEXT("output"));', 'FParamSchemaBuilder().Required(TEXT("nodes"), TEXT("array"), TEXT("n")).Build()'))
        self.assertEqual(result['errors'], [])

    def test_comments_strings_and_raw_strings_are_not_reads(self):
        body = '''// Params->GetStringField(TEXT("comment"));
/* Params->HasField(TEXT("block")); */
FString Text = R"cpp(Params->GetStringField(TEXT("literal")))cpp";'''
        self.assertEqual(self.run_source(source(body))['errors'], [])

    def test_parameter_rename_simple_alias_and_get_wrapper(self):
        body = 'const auto& Input = Args; auto Copy = Input; Copy.Get()->GetStringField(TEXT("hidden"));'
        result = self.run_source(source(body, args='const TSharedPtr<FJsonObject>& Args'))
        self.assertEqual(result['registrations'][0]['missing'], ['hidden'])

    def test_namespace_inline_class_getter_and_header_registration(self):
        text = '''namespace Outer { class A { public:
static FString GetName() { return TEXT("action"); }
static TSharedPtr<FJsonObject> GetSchema() { return FParamSchemaBuilder().Required(TEXT("p"), TEXT("string"), TEXT("p")).Build(); }
static FMonolithActionResult Execute(const TSharedPtr<FJsonObject>& Input) { Input->GetStringField(TEXT("p")); }
static void Register() { R.RegisterAction(TEXT("unit"), A::GetName(), TEXT("d"), FMonolithActionHandler::CreateStatic(&A::Execute), A::GetSchema()); }
}; }'''
        result = LINT.Checker({'Source/Module/Inline.h': text}).run()
        self.assertEqual(result['errors'], [])
        self.assertEqual(result['registrations'][0]['handler'], 'Outer::A::Execute')

    def test_compile_gate_implementations_are_unioned(self):
        text = source('Params->GetStringField(TEXT("first"));') + '''
#if OPTIONAL
FMonolithActionResult A::Handle(const TSharedPtr<FJsonObject>& Params) { Params->HasField(TEXT("second")); }
#else
FMonolithActionResult A::Handle(const TSharedPtr<FJsonObject>& /*Params*/) { return Unavailable(); }
#endif'''
        self.assertEqual(self.run_source(text)['registrations'][0]['missing'], ['first', 'second'])

    def test_inline_handler_and_schema_factory_lambdas(self):
        text = '''void Register() { const auto Schema = []() { return FParamSchemaBuilder().Optional(TEXT("p"), TEXT("object"), TEXT("p")).Build(); };
R.RegisterAction(TEXT("unit"), TEXT("action"), TEXT("d"), FMonolithActionHandler::CreateLambda([](const TSharedPtr<FJsonObject>& Input) { Input->GetObjectField(TEXT("p")); }), Schema()); }'''
        self.assertEqual(self.run_source(text)['errors'], [])

    def test_manual_schema_and_named_helper_keys(self):
        text = '''void Register() { auto Schema = MakeShared<FJsonObject>();
const auto Add = [&Schema](const TCHAR* Name, const TCHAR* Type) { auto Prop = MakeShared<FJsonObject>(); Schema->SetObjectField(Name, Prop); };
Add(TEXT("p"), TEXT("string")); Schema->SetObjectField(TEXT("q"), Q);
R.RegisterAction(TEXT("unit"), TEXT("action"), TEXT("d"), FMonolithActionHandler::CreateLambda([](const TSharedPtr<FJsonObject>& Params) { Params->GetStringField(TEXT("p")); Params->HasField(TEXT("q")); }), Schema); }'''
        self.assertEqual(self.run_source(text)['errors'], [])

    def test_manual_schema_getter_and_variable_scope(self):
        text = 'TSharedPtr<FJsonObject> A::Schema() { auto Schema = MakeShared<FJsonObject>(); Schema->SetObjectField(TEXT("p"), Prop); return Schema; }\n' + source('Params->HasField(TEXT("p"));', schema='A::Schema()')
        self.assertEqual(self.run_source(text)['errors'], [])
        text = 'void Before() { auto Schema = MakeShared<FJsonObject>(); Schema->SetObjectField(TEXT("p"), Prop); }\n' + source(schema='Schema')
        self.assertTrue(self.run_source(text)['errors'])

    def test_named_builder_does_not_collect_unrelated_builder(self):
        text = source('Params->HasField(TEXT("hidden"));', schema='Schema').replace('R.RegisterAction(', 'auto Schema = FParamSchemaBuilder().Required(TEXT("p"), TEXT("string"), TEXT("p")).Build(); auto Other = FParamSchemaBuilder().Optional(TEXT("hidden"), TEXT("string"), TEXT("h")).Build(); R.RegisterAction(')
        self.assertEqual(self.run_source(text)['registrations'][0]['missing'], ['hidden'])

    def test_empty_schema_getter_and_explicit_empty(self):
        self.assertEqual(self.run_source(source(schema='MakeShared<FJsonObject>()'))['errors'], [])
        text = 'TSharedPtr<FJsonObject> A::Schema() { return MakeShared<FJsonObject>(); }\n' + source(schema='A::Schema()')
        self.assertEqual(self.run_source(text)['errors'], [])

    def test_missing_and_null_schema(self):
        omitted = source().replace(', FParamSchemaBuilder().Build()', '')
        for text in [omitted, source(schema='nullptr')]:
            with self.subTest(text=text):
                self.assertTrue(self.run_source(text)['errors'])

    def test_unknown_handler_schema_and_registration_fail_closed(self):
        cases = [source(handler='FMonolithActionHandler::CreateStatic(&Unknown)'), source(schema='Mystery()'), source().replace('TEXT("unit")', 'Namespace')]
        for text in cases:
            with self.subTest(text=text):
                self.assertTrue(self.run_source(text)['errors'])

    def test_macro_definition_not_silently_ignored(self):
        result = self.run_source('#define REGISTER(N) R.RegisterAction(TEXT("unit"), N, TEXT("d"), H, S)\nREGISTER(TEXT("action"));')
        self.assertTrue(result['errors'])

    def test_wrapping_registration_macro_and_nested_arguments(self):
        text = source('Params->GetStringField(TEXT("hidden"));').replace('R.RegisterAction(', 'WRAP(R.RegisterAction(').replace('Build());', 'Build()));')
        result = self.run_source(text)
        self.assertEqual(result['registrations'][0]['missing'], ['hidden'])

    def test_forwarding_allowlist_adds_keys_without_hiding_direct_drift(self):
        entry = {'action': 'unit.action', 'handler': 'A::Handle', 'helper': 'Forward', 'input_arg': 0, 'keys': ['forwarded'], 'reason': 'Forward reads the original top-level object.'}
        result = self.run_source(source('Forward(Params); Params->HasField(TEXT("direct"));'), [entry])
        self.assertEqual(result['registrations'][0]['missing'], ['direct', 'forwarded'])

    def test_revision_blob_reader_uses_in_memory_snapshot(self):
        commit = 'a' * 40
        blob = b'b' * 40
        text = source().encode('utf-8')
        tree = b'100644 blob ' + blob + b'\tSource/Inline.h\0' + b'100644 blob ' + blob + b'\tSource/Tests/Ignore.cpp\0'
        payload = blob + b' blob ' + str(len(text)).encode() + b'\n' + text + b'\n'
        with mock.patch.object(LINT.subprocess, 'check_output', side_effect=[commit + '\n', tree]) as read, \
             mock.patch.object(LINT.subprocess, 'run', return_value=mock.Mock(stdout=payload)) as batch:
            resolved, files = LINT.revision_sources(Path('.'), 'baseline')
        self.assertEqual(resolved, commit)
        self.assertEqual(files, {'Source/Inline.h': text.decode()})
        self.assertEqual(read.call_count, 2)
        self.assertEqual(batch.call_args.kwargs['input'], blob + b'\n')
        self.assertEqual(batch.call_args.args[0], ['git', 'cat-file', '--batch'])

    def test_repeated_run_resets_errors(self):
        checker = LINT.Checker({'Source/Test.cpp': source('Params->HasField(TEXT("missing"));')})
        first = checker.run()
        second = checker.run()
        self.assertEqual(first, second)
        self.assertEqual(len(second['errors']), 1)

    def test_builder_description_does_not_declare_fake_keys(self):
        schema = 'FParamSchemaBuilder().Optional(TEXT("real"), TEXT("string"), R"cpp(.Optional(TEXT("hidden"), TEXT("string"), TEXT("fake")))cpp").Build()'
        result = self.run_source(source('Params->HasField(TEXT("hidden"));', schema))
        self.assertEqual(result['registrations'][0]['keys'], ['real'])
        self.assertEqual(result['registrations'][0]['missing'], ['hidden'])

    def test_cli_enforces_default_forwarding_and_excludes_tests(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'Source' / 'Tests').mkdir(parents=True)
            (root / 'Scripts').mkdir()
            header = root / 'Source' / 'Inline.h'
            header.write_text(source('Forward(Params);'), encoding='utf-8')
            (root / 'Source' / 'Tests' / 'Ignored.cpp').write_text('R.RegisterAction(unsupported);', encoding='utf-8')
            config = root / 'Scripts' / 'schema_drift_forwarding.json'
            entry = {'action': 'unit.action', 'handler': 'A::Handle', 'helper': 'Forward', 'input_arg': 0,
                     'keys': ['forwarded'], 'reason': 'Forward consumes a top-level input.'}
            config.write_text(json.dumps([entry]), encoding='utf-8')
            command = [sys.executable, str(Path(LINT.__file__)), '--root', str(root)]
            bad = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(bad.returncode, 1, bad.stdout + bad.stderr)
            self.assertIn('forwarded', bad.stdout)
            schema = 'FParamSchemaBuilder().Optional(TEXT("forwarded"), TEXT("string"), TEXT("p")).Build()'
            header.write_text(source('Forward(Params);', schema), encoding='utf-8')
            good = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(good.returncode, 0, good.stdout + good.stderr)
            header.write_text(source('Forward(Params); Params->HasField(TEXT("direct"));', schema), encoding='utf-8')
            override = root / 'empty.json'
            override.write_text('[]', encoding='utf-8')
            direct = subprocess.run(command + ['--allowlist', str(override)], capture_output=True, text=True)
            self.assertEqual(direct.returncode, 1, direct.stdout + direct.stderr)
            self.assertIn('(direct)', direct.stdout)
            self.assertNotIn('(forwarded)', direct.stdout)

    def test_forwarding_entry_cannot_claim_nested_input(self):
        entry = {'action': 'unit.action', 'handler': 'A::Handle', 'helper': 'Forward', 'input_arg': 0, 'keys': ['child'], 'reason': 'Wrong input.'}
        self.assertTrue(self.run_source(source('Forward(Child);'), [entry])['errors'])
        entry['action'] = 'unit.missing'
        self.assertIn('unused forwarding entry', [e['error'] for e in self.run_source(source(), [entry])['errors']])


if __name__ == '__main__':
    unittest.main()
