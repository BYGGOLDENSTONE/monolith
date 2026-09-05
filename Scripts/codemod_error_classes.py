"""Conservatively migrate mechanical action errors; default is a read-only report.

Requires FMonolithActionResult::WithErrorMessage to preserve existing diagnostics.
The execution scan is a review aid, not C++ data-flow analysis: every migration
records its enclosing action and line, while known preceding mutation operations
are left untouched. Optional-dependency and other explicit codes are preserved.
"""

import argparse
import collections
import json
from pathlib import Path
import re


TOKEN = re.compile(
    r'//[^\n]*|/\*[\s\S]*?\*/|'
    r'(?:u8|u|U|L)?R"(?P<delimiter>[^\s\\()]*)\([\s\S]*?\)(?P=delimiter)"|'
    r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
ERROR = re.compile(r'\bFMonolithActionResult\s*::\s*Error\s*\(')
FUNCTION = re.compile(
    r'(?m)^[ \t]*(?:static\s+|inline\s+)*FMonolithActionResult\s+([\w:]+)\s*\(')
LITERAL = re.compile(r'TEXT\(\s*"((?:\\.|[^"\\])*)"\s*\)', re.S)
MUTATION = re.compile(
    r'\b(?:Modify|MarkPackageDirty|MarkBlueprint\w*|BeginTransaction|'
    r'CreatePackage|NewObject|ConstructObject|DuplicateObject|'
    r'GetOrCreate\w*|FindOrCreate\w*|FindOrBegin\w*|GetBuilderForAsset|'
    r'Spawn\w*|TryAdd\w*|GiveAbility\w*|TryActivate\w*|'
    r'Compile\w*|Save\w*|Create\w*|Delete\w*|Remove\w*|Add\w*|'
    r'Set\w*|Apply\w*|Import\w*|Rename\w*|ExecuteAction|Dispatch\w*|'
    r'Connect\w*|Disconnect\w*|Break\w*|Insert\w*|Clear\w*|'
    r'Build\w*|Update\w*|Attach\w*|Detach\w*|Register\w*|Unregister\w*)\s*(?:<[^;{}]*>)?\s*\('
    r'|\b\w+\s*->\s*\w+\s*(?:\[[^\]]*\])?\s*(?:=(?!=)|\+=|-=|\+\+|--)')
INTERNAL_CODES = {'-32603', 'FMonolithJsonUtils::ErrInternalError'}


def code_mask(source):
    """Keep offsets/newlines while masking comments, raw strings and literals."""
    return TOKEN.sub(lambda match: ''.join('\n' if c == '\n' else ' ' for c in match[0]), source)


def balanced_end(masked, opening):
    pairs = {'(': ')', '[': ']', '{': '}'}
    stack = [pairs[masked[opening]]]
    for position in range(opening + 1, len(masked)):
        char = masked[position]
        if char in pairs:
            stack.append(pairs[char])
        elif char in ')]}':
            if not stack or stack.pop() != char:
                raise ValueError('Unbalanced C++ delimiter at offset %d' % position)
            if not stack:
                return position + 1
    raise ValueError('Unterminated C++ delimiter at offset %d' % opening)


def call_arguments(source, masked, opening, end):
    arguments = []
    start = position = opening + 1
    while position < end - 1:
        char = masked[position]
        if char in '([{':
            position = balanced_end(masked, position)
            continue
        if char == ',':
            arguments.append(source[start:position].strip())
            start = position + 1
        position += 1
    arguments.append(source[start:end - 1].strip())
    return arguments


def action_spans(masked):
    spans = []
    for match in FUNCTION.finditer(masked):
        end = balanced_end(masked, match.end() - 1)
        opening = end
        while opening < len(masked) and masked[opening].isspace():
            opening += 1
        if opening < len(masked) and masked[opening] == '{':
            spans.append((opening + 1, balanced_end(masked, opening) - 1, match[1]))
    return spans


def callsites(pattern, masked):
    """Exclude qualified names in out-of-line definitions/declarations."""
    signatures = [(match.start(), match.end()) for match in FUNCTION.finditer(masked)]
    return (match for match in pattern.finditer(masked)
            if not any(start <= match.start() < end for start, end in signatures))


def enclosing_mutating_loop(masked, start, position):
    """A later iteration may already have mutated even before this source line.

    Deliberately conservative: local container Add calls also need hand review.
    Unbraced loops cannot contain both a returning error and a later mutation.
    """
    for loop in re.finditer(r'\b(?:for|while)\s*\(', masked[start:position]):
        opening = balanced_end(masked, start + loop.end() - 1)
        while opening < len(masked) and masked[opening].isspace():
            opening += 1
        if opening < len(masked) and masked[opening] == '{':
            end = balanced_end(masked, opening)
            if opening < position < end and MUTATION.search(masked[position:end]):
                return start + loop.start()
    # The body precedes its condition in do/while, unlike the loops above.
    for loop in re.finditer(r'\bdo\s*\{', masked[start:position]):
        end = balanced_end(masked, start + loop.end() - 1)
        if position < end and MUTATION.search(masked[position:end]):
            return start + loop.start()
    return None


def text_literal(value):
    return 'TEXT("%s")' % value.replace('\\', '\\\\').replace('"', '\\"')


def string_expression(value):
    # Only stable values: preserving the original Printf evaluates the needle
    # twice, so arbitrary getters/calls with unknown side effects are excluded.
    match = re.fullmatch(r'\*([A-Za-z_]\w*(?:(?:\.|->)[A-Za-z_]\w*)*(?:\.ToString\(\))?)', value)
    return match[1] if match else None


def classify_message(message):
    """Return (helper expression, review reason), before message preservation."""
    literal = LITERAL.fullmatch(message)
    values = []
    if literal:
        format_text = literal[1]
    else:
        prefix = re.match(r'FString::Printf\s*\(', message)
        if not prefix:
            return None, 'nonmechanical_message'
        masked = code_mask(message)
        end = balanced_end(masked, prefix.end() - 1)
        if end != len(message):
            return None, 'nonmechanical_message'
        args = call_arguments(message, masked, prefix.end() - 1, end)
        literal = LITERAL.fullmatch(args[0])
        if not literal:
            return None, 'nonliteral_format'
        format_text = literal[1]
        values = args[1:]

    if re.search(r'\bnot found\b', format_text, re.I):
        if len(values) != 1 or re.findall(r'%(?:\d+\$)?[A-Za-z%]', format_text) != ['%s']:
            return None, 'not_found_requires_single_string'
        needle = string_expression(values[0])
        if needle is None:
            return None, 'unstable_needle_expression'
        # Label the subject only; preserve the entire original message below,
        # including scope, available-name text, and next-action instructions.
        kind = re.split(r'%s|\bnot found\b', format_text, maxsplit=1, flags=re.I)[0]
        kind = kind.strip(' :\'"`-')
        if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9 ]*', kind):
            return None, 'ambiguous_not_found_kind'
        return 'FMonolithActionResult::NotFound(%s, %s)' % (text_literal(kind), needle), 'not_found'

    missing = re.search(
        r'\bMissing(?:\s+or\s+(?:empty|invalid))?\s+(?:required\s+)?param(?:eter)?(?:s|\(s\))?\s*[:\'"`]\s*',
        format_text, re.I)
    if missing:
        tail = format_text[missing.end():]
        if values:
            if len(values) != 1 or not tail.startswith('%s'):
                return None, 'nonmechanical_missing_param'
            name = string_expression(values[0])
            if name is None:
                return None, 'unstable_parameter_expression'
        else:
            name_match = re.match(r'([A-Za-z_]\w*)', tail)
            if not name_match:
                return None, 'ambiguous_parameter_name'
            remainder = tail[name_match.end():]
            if re.match(r'\s*(?:,|/|\band\b|\bor\b)', remainder):
                return None, 'multiple_parameter_names'
            name = text_literal(name_match[1])
        return 'FMonolithActionResult::InvalidParam(%s, %s)' % (name, message), 'invalid_param'
    return None, 'outside_mechanical_families'


def rewrite_source(source, filename='<fixture>'):
    masked = code_mask(source)
    spans = action_spans(masked)
    replacements, review = [], []
    for match in callsites(ERROR, masked):
        end = balanced_end(masked, match.end() - 1)
        arguments = call_arguments(source, masked, match.end() - 1, end)
        internal = len(arguments) == 1 or (len(arguments) == 2 and re.sub(r'\s+', '', arguments[1]) in INTERNAL_CODES)
        row = {'file': filename, 'line': source.count('\n', 0, match.start()) + 1,
               'internal': internal, 'status': 'skipped',
               'code_kind': 'default_internal' if len(arguments) == 1 else
                            'explicit_internal' if internal else 'explicit_other'}
        if not internal:
            row['reason'] = 'explicit_noninternal_or_unclassified_code'
        else:
            replacement, reason = classify_message(arguments[0])
            row['reason'] = reason
            if replacement:
                contexts = [span for span in spans if span[0] <= match.start() < span[1]]
                if not contexts:
                    row['reason'] = 'no_action_function_context'
                else:
                    start, _, action = max(contexts)
                    row['action'] = action
                    preceding = MUTATION.search(masked[start:match.start()])
                    loop_position = enclosing_mutating_loop(masked, start, match.start())
                    if preceding:
                        row['reason'] = 'preceding_side_effect_requires_review'
                        row['side_effect'] = source[start + preceding.start():start + preceding.end()]
                        row['side_effect_line'] = source.count('\n', 0, start + preceding.start()) + 1
                    elif loop_position is not None:
                        row['reason'] = 'loop_carried_side_effect_requires_review'
                        row['side_effect_line'] = source.count('\n', 0, loop_position) + 1
                    else:
                        replacement += '.WithErrorMessage(%s)' % arguments[0]
                        replacements.append((match.start(), end, replacement))
                        row['status'] = 'migrated'
                        row['execution_review'] = 'No known preceding mutation marker; review action context before commit.'
        review.append(row)
    for start, end, replacement in reversed(replacements):
        source = source[:start] + replacement + source[end:]
    return source, review


def production_files(root):
    return sorted(path for path in (root / 'Source').rglob('*')
                  if path.suffix in {'.cpp', '.h'}
                  and not any(part.lower() in {'test', 'tests'} for part in path.parts)
                  and 'Test' not in path.stem)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--write', action='store_true', help='Apply reviewed mechanical rewrites in place')
    parser.add_argument('--report', type=Path, help='Write detailed file/line review JSON')
    parser.add_argument('--counts', action='store_true', help='Inventory current Error call codes only; never rewrite')
    options = parser.parse_args(argv)
    if options.counts and options.write:
        parser.error('--counts cannot be combined with --write')
    review, pending = [], []
    counts = collections.defaultdict(collections.Counter)
    for path in production_files(options.root):
        with path.open(encoding='utf-8-sig', newline='') as handle:
            source = handle.read()
        rewritten, rows = rewrite_source(source, path.relative_to(options.root).as_posix())
        module = path.relative_to(options.root).parts[1]
        typed_engine_errors = sum(1 for _ in callsites(
            re.compile(r'\bFMonolithActionResult\s*::\s*EngineError\s*\('), code_mask(source)))
        counts[module]['typed_engine_error'] += typed_engine_errors
        counts[module]['total_internal'] += typed_engine_errors
        for row in rows:
            counts[module][row['code_kind']] += 1
            counts[module]['total_internal'] += row['internal']
            if options.counts:
                continue
            if row['internal']:
                counts[module]['before_internal'] += 1
                counts[module]['after_internal'] += row['status'] != 'migrated'
            if row['status'] == 'migrated':
                counts[module][row['reason']] += 1
        review.extend(rows)
        if options.write and rewritten != source:
            bom = path.read_bytes().startswith(b'\xef\xbb\xbf')
            pending.append((path, rewritten, bom))
    # Parse and classify the full input before writing any file. A malformed
    # fixture or unexpected C++ construct must not leave a partial codemod.
    for path, rewritten, bom in pending:
        with path.open('w', encoding='utf-8-sig' if bom else 'utf-8', newline='') as handle:
            handle.write(rewritten)
    methodology = ('Balanced fully-qualified FMonolithActionResult::Error calls in Source .cpp/.h; '
                   'excludes Test/Tests directories and filenames containing Test; comments and literals masked. '
                   'Default code or explicit -32603 / FMonolithJsonUtils::ErrInternalError count as internal. '
                   'Typed EngineError callsites also count toward total_internal (the current source, before proposed rewrites). '
                   'Inline automation blocks in production files are included; unqualified calls and helper definitions are not. '
                   'Generic Error calls with symbolic/dynamic codes other than ErrInternalError are explicit_other.')
    report = {'applied': options.write, 'methodology': methodology, 'counts': dict(counts), 'review': review}
    if options.report:
        options.report.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({'applied': options.write, 'methodology': methodology, 'counts': dict(counts),
                      'review_reasons': dict(collections.Counter(row['reason'] for row in review))}, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
