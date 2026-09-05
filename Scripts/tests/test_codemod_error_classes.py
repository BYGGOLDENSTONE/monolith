"""Semantic regressions for the conservative error-class codemod."""

import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    'codemod_error_classes', Path(__file__).resolve().parents[1] / 'codemod_error_classes.py')
codemod = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(codemod)


def action(body):
    return 'FMonolithActionResult FExample::Handle(const TSharedPtr<FJsonObject>& Params)\n{\n' + body + '\n}\n'


class ErrorClassCodemodTests(unittest.TestCase):
    def test_not_found_preserves_message_hints_and_data_override(self):
        message = 'FString::Printf(TEXT("Graph \'%s\' not found. Use list_graphs."), *GraphName)'
        source = action('return FMonolithActionResult::Error(' + message + ').WithErrorData(Context);')
        rewritten, rows = codemod.rewrite_source(source)
        self.assertIn('NotFound(TEXT("Graph"), GraphName).WithErrorMessage(' + message + ').WithErrorData(Context)', rewritten)
        self.assertEqual(rows[0]['status'], 'migrated')
        self.assertEqual(rows[0]['line'], 3)

    def test_missing_parameter_preserves_name_and_original_reason(self):
        for message, name in [('Missing required parameter: asset_path', 'asset_path'),
                              ("Missing required param 'emitter_asset': supply a path", 'emitter_asset'),
                              ('Missing or empty required parameter: tags', 'tags')]:
            with self.subTest(message=message):
                source = action('return FMonolithActionResult::Error(TEXT("' + message + '"));')
                rewritten, rows = codemod.rewrite_source(source)
                self.assertIn('InvalidParam(TEXT("' + name + '"), TEXT("' + message + '"))', rewritten)
                self.assertEqual(rows[0]['status'], 'migrated')

    def test_explicit_internal_migrates_but_other_codes_remain_exact(self):
        for code in ['-32603', 'FMonolithJsonUtils::ErrInternalError', '-32010', 'FMonolithJsonUtils::ErrInvalidParams']:
            source = action('return FMonolithActionResult::Error(TEXT("Missing required param: name"), ' + code + ');')
            rewritten, rows = codemod.rewrite_source(source)
            if code in codemod.INTERNAL_CODES:
                self.assertNotEqual(rewritten, source)
            else:
                self.assertEqual(rewritten, source)
                self.assertFalse(rows[0]['internal'])

    def test_ignores_comments_regular_and_raw_string_literals(self):
        source = action('''// FMonolithActionResult::Error(TEXT("Missing required param: fake"));
/* FMonolithActionResult::Error(TEXT("Missing required param: fake")); */
const char* A = "FMonolithActionResult::Error(\\\"not found\\\")";
const char* B = R"marker(FMonolithActionResult::Error(TEXT("Missing required param: fake"));)marker";
return FMonolithActionResult::Success(Json);''')
        self.assertEqual(codemod.rewrite_source(source), (source, []))

    def test_nested_calls_and_multiple_printf_values_are_balanced_and_skipped(self):
        source = action('return FMonolithActionResult::Error(FString::Printf(TEXT("Graph %s not found in %s"), *Name.ToString(), *FString::Join(Names, TEXT(","))));')
        rewritten, rows = codemod.rewrite_source(source)
        self.assertEqual(rewritten, source)
        self.assertEqual(rows[0]['reason'], 'not_found_requires_single_string')

    def test_no_double_evaluation_of_arbitrary_function_needle(self):
        source = action('return FMonolithActionResult::Error(FString::Printf(TEXT("Graph %s not found"), *FetchNextName()));')
        rewritten, rows = codemod.rewrite_source(source)
        self.assertEqual(rewritten, source)
        self.assertEqual(rows[0]['reason'], 'unstable_needle_expression')

    def test_post_mutation_error_is_left_for_review(self):
        for mutation in ['BP->Modify();', 'Thing->Asset = Loaded;', 'CreatePackage(*Path);',
                         'Registry.ExecuteAction(Namespace, Action, Params);', 'GEditor->BeginTransaction(Title);',
                         'GetOrCreatePackage(Path, Error);', 'FindOrCreateBinding(Asset);',
                         'GetBuilderForAsset(Path, Error);', 'World->SpawnActor(Class);']:
            source = action(mutation + '\nreturn FMonolithActionResult::Error(TEXT("Missing required param: name"));')
            rewritten, rows = codemod.rewrite_source(source)
            self.assertEqual(rewritten, source)
            self.assertEqual(rows[0]['reason'], 'preceding_side_effect_requires_review')
            self.assertEqual(rows[0]['side_effect_line'], 3)

    def test_previous_loop_iteration_may_have_mutated(self):
        for prefix, suffix in [('for (auto Name : Names) {', '}'),
                               ('while (More()) {', '}'), ('do {', '} while (More());')]:
            source = action(prefix + '\nif (Missing) return FMonolithActionResult::Error('
                            'TEXT("Missing required param: name"));\nAsset->Modify();\n' + suffix)
            rewritten, rows = codemod.rewrite_source(source)
            self.assertEqual(rewritten, source)
            self.assertEqual(rows[0]['reason'], 'loop_carried_side_effect_requires_review')

    def test_completed_readonly_loop_does_not_block_preflight(self):
        source = action('for (auto Value : Values) { FindValue(Value); }\n'
                        'return FMonolithActionResult::Error(TEXT("Missing required param: name"));')
        self.assertEqual(codemod.rewrite_source(source)[1][0]['reason'],
                         'invalid_param')

    def test_counts_are_readonly_and_distinguish_codes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'Source' / 'Example' / 'Actions.cpp'
            path.parent.mkdir(parents=True)
            source = action('return FMonolithActionResult::Error(TEXT("Missing required param: name"));')
            source += action('return FMonolithActionResult::Error(TEXT("failed"), -32603);')
            source += action('return FMonolithActionResult::Error(TEXT("dependency"), -32010);')
            source += action('return FMonolithActionResult::EngineError(TEXT("operation"), TEXT("failed"));')
            source += ('FMonolithActionResult FMonolithActionResult::EngineError('
                       'const FString& Operation, const FString& Message) { return Result; }\n')
            source += ('FMonolithActionResult FMonolithActionResult::Error('
                       'const FString& Message, int32 Code) { return Result; }\n')
            path.write_text(source, encoding='utf-8')
            stream = io.StringIO()
            with contextlib.redirect_stdout(stream):
                codemod.main(['--root', directory, '--counts'])
            import json
            self.assertEqual(json.loads(stream.getvalue())['counts']['Example'],
                             {'default_internal': 1, 'explicit_internal': 1, 'explicit_other': 1,
                              'typed_engine_error': 1, 'total_internal': 3})
            self.assertEqual(path.read_text(encoding='utf-8'), source)

    def test_previous_function_mutation_does_not_block_next_preflight(self):
        first = action('BP->Modify(); return FMonolithActionResult::Success(Json);')
        second = action('return FMonolithActionResult::Error(TEXT("Missing required param: name"));')
        rewritten, rows = codemod.rewrite_source(first + second)
        self.assertEqual(rows[0]['status'], 'migrated')
        self.assertTrue(rewritten.startswith(first))

    def test_multiple_names_are_not_mislabelled_as_one_parameter(self):
        source = action('return FMonolithActionResult::Error(TEXT("Missing required params: asset_path, node_id"));')
        rewritten, rows = codemod.rewrite_source(source)
        self.assertEqual(rewritten, source)
        self.assertEqual(rows[0]['reason'], 'multiple_parameter_names')

    def test_rewrite_is_idempotent_and_crlf_survives(self):
        source = action('return FMonolithActionResult::Error(TEXT("Missing required param: name"));').replace('\n', '\r\n')
        once, _ = codemod.rewrite_source(source)
        twice, _ = codemod.rewrite_source(once)
        self.assertEqual(once, twice)
        self.assertEqual(once.count('\r\n'), source.count('\r\n'))

    def test_cli_dry_run_does_not_write_source_and_apply_preserves_bom(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / 'Source' / 'Example' / 'Actions.cpp'
            path.parent.mkdir(parents=True)
            original = b'\xef\xbb\xbf' + action('return FMonolithActionResult::Error(TEXT("Missing required param: name"));').encode()
            path.write_bytes(original)
            with contextlib.redirect_stdout(io.StringIO()):
                codemod.main(['--root', directory])
            self.assertEqual(path.read_bytes(), original)
            with contextlib.redirect_stdout(io.StringIO()):
                codemod.main(['--root', directory, '--write'])
            self.assertTrue(path.read_bytes().startswith(b'\xef\xbb\xbf'))
            self.assertIn(b'InvalidParam', path.read_bytes())

    def test_parse_failure_leaves_earlier_files_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            folder = root / 'Source' / 'Example'
            folder.mkdir(parents=True)
            good = folder / 'A.cpp'
            original = action('return FMonolithActionResult::Error(TEXT("Missing required param: name"));')
            good.write_text(original, encoding='utf-8')
            (folder / 'Z.cpp').write_text('FMonolithActionResult F::Broken() {', encoding='utf-8')
            with self.assertRaises(ValueError):
                codemod.main(['--root', directory, '--write'])
            self.assertEqual(good.read_text(encoding='utf-8'), original)


if __name__ == '__main__':
    unittest.main()
