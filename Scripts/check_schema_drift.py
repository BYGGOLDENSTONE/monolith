#!/usr/bin/env python3
"""Check literal top-level action input reads against registered parameter schemas.

This is deliberately a C++ source lint, not a C++ compiler. Unsupported handler or
schema syntax fails closed with a source location. Tests directories are excluded.
Forwarding entries add known helper-consumed keys; they never suppress direct reads.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

TOKEN = re.compile(
    r'R"(?P<delim>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delim)"'
    r'|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    r'|//[^\n]*|/\*.*?\*/', re.S)
TEXT = re.compile(r'TEXT\s*\(\s*"([^"\\]*(?:\\.[^"\\]*)*)"\s*\)')
JSON_PARAM = re.compile(r'(?:TSharedPtr|TSharedRef)\s*<\s*FJsonObject\s*>\s*(?:const\s*)?&?\s*(\w+)')


class ParseError(ValueError):
    pass


def mask(source: str, comments_only: bool = False) -> str:
    """Keep offsets/newlines stable; optionally retain strings for literal reads."""
    return TOKEN.sub(lambda m: re.sub(r'[^\n]', ' ', m[0])
                     if not comments_only or m[0].startswith(('//', '/*')) else m[0], source)


def closing(source: str, start: int) -> int:
    pairs = {'(': ')', '{': '}', '[': ']'}
    stack = [pairs[source[start]]]
    for i in range(start + 1, len(source)):
        char = source[i]
        if char in pairs:
            stack.append(pairs[char])
        elif char in ')}]':
            if not stack or char != stack.pop():
                raise ParseError(f'unbalanced delimiters at offset {i}')
            if not stack:
                return i
    raise ParseError(f'unterminated delimiter at offset {start}')


def arguments(source: str) -> list[str]:
    scrubbed = mask(source)
    result, start, i = [], 0, 0
    while i < len(scrubbed):
        if scrubbed[i] in '({[':
            i = closing(scrubbed, i)
        elif scrubbed[i] == ',':
            result.append(source[start:i].strip())
            start = i + 1
        i += 1
    result.append(source[start:].strip())
    return result


def literal(source: str) -> str | None:
    match = TEXT.fullmatch(source.strip())
    return match[1] if match else None


@dataclass
class Function:
    path: str
    name: str
    params: str
    body: str
    start: int
    end: int


@dataclass
class Source:
    path: str
    text: str
    masked: str
    scopes: list[tuple[int, int, str]]

    def qualify(self, name: str, offset: int) -> str:
        enclosing = [scope[2] for scope in self.scopes if scope[0] < offset < scope[1]]
        prefix = '::'.join(enclosing)
        return prefix + '::' + name if prefix and not name.startswith(prefix + '::') else name


class Checker:
    def __init__(self, sources: dict[str, str], forwarding: list[dict] | None = None):
        self.sources: dict[str, Source] = {}
        self.functions: list[Function] = []
        self.forwarding = forwarding or []
        self.errors: list[dict] = []
        for path, raw in sorted(sources.items()):
            clean = mask(raw, comments_only=True)
            scrubbed = mask(clean)
            scopes = []
            for match in re.finditer(r'\b(?:class|struct)\s+(?:\w+_API\s+)?(\w+)[^;{}]*\{|\bnamespace\s+([\w:]+)\s*\{', scrubbed):
                scopes.append((match.start(), closing(scrubbed, match.end() - 1), match[1] or match[2]))
            src = Source(path, clean, scrubbed, scopes)
            self.sources[path] = src
            # Actions and schema/name getter methods, including inline class methods.
            pattern = r'\b(?:FMonolithActionResult|TSharedPtr\s*<\s*FJsonObject\s*>|TSharedRef\s*<\s*FJsonObject\s*>|FString|const\s+TCHAR\s*\*)\s+((?:\w+::)*\w+)\s*\('
            for match in re.finditer(pattern, scrubbed):
                param_end = closing(scrubbed, match.end() - 1)
                after = re.match(r'\s*(?:const\s*)?(?:override\s*)?\{', scrubbed[param_end + 1:])
                if not after:
                    continue
                body_start = param_end + 1 + after.end() - 1
                body_end = closing(scrubbed, body_start)
                self.functions.append(Function(path, src.qualify(match[1], match.start()),
                                               clean[match.end():param_end], clean[body_start + 1:body_end],
                                               match.start(), body_end))
            # Local schema factory lambdas (e.g. dual gas/ui registrations).
            for match in re.finditer(r'\bauto\s+(\w+)\s*=\s*\[[^\]]*\]\s*\(', scrubbed):
                param_end = closing(scrubbed, match.end() - 1)
                after = re.match(r'\s*(?:->[^{}]+)?\{', scrubbed[param_end + 1:])
                if not after:
                    continue
                body_start = param_end + 1 + after.end() - 1
                body_end = closing(scrubbed, body_start)
                self.functions.append(Function(path, src.qualify(match[1], match.start()),
                                               clean[match.end():param_end], clean[body_start + 1:body_end],
                                               match.start(), body_end))

    def resolve(self, name: str, path: str) -> list[Function]:
        exact = [fn for fn in self.functions if fn.name == name]
        candidates = exact or [fn for fn in self.functions if fn.name.endswith('::' + name)]
        local = [fn for fn in candidates if fn.path == path]
        candidates = local or candidates
        # Multiple #if implementations of one qualified symbol are valid; unrelated
        # symbols with the same short name are not silently guessed.
        if len({fn.name for fn in candidates}) > 1:
            raise ParseError(f'ambiguous function {name}')
        if not candidates:
            raise ParseError(f'unresolved function {name}')
        return candidates

    def schema_keys(self, expression: str, src: Source, offset: int, seen: frozenset = frozenset()) -> tuple[set[str], str]:
        expression = expression.strip()
        if expression in ('nullptr', 'NULL', '{}'):
            raise ParseError('schema is null or absent')
        if re.fullmatch(r'MakeShared\s*<\s*FJsonObject\s*>\s*\(\s*\)', expression):
            return set(), 'explicit_empty'
        if 'FParamSchemaBuilder' in expression:
            keys = set()
            scrubbed = mask(expression)
            if not re.search(r'\.\s*Build\s*\(', scrubbed):
                raise ParseError('schema builder has no Build()')
            for match in re.finditer(r'\.\s*(Required\w*|Optional\w*)\s*\(', scrubbed):
                end = closing(scrubbed, match.end() - 1)
                args = arguments(expression[match.end():end])
                key = literal(args[0])
                if key is None:
                    raise ParseError(f'nonliteral schema key in {match[1]}')
                keys.add(key)
                if args[-1].startswith('{'):
                    aliases = arguments(args[-1][1:-1])
                    for alias in aliases:
                        if not alias:
                            continue
                        value = literal(alias)
                        if value is None:
                            raise ParseError('nonliteral schema alias')
                        keys.add(value)
            return keys, 'builder'
        getter = re.fullmatch(r'([\w:]+)\s*\(\s*\)', expression)
        if getter:
            if getter[1] in seen:
                raise ParseError('recursive schema getter')
            keys = set()
            for fn in self.resolve(getter[1], src.path):
                keys.update(self.schema_keys(fn.body, self.sources[fn.path], fn.end, seen | {getter[1]})[0])
            return keys, 'schema_getter'
        variable = re.fullmatch(r'\w+', expression)
        if not variable:
            returned = re.search(r'\breturn\s+([^;]+)\s*;', expression)
            if returned:
                return self.schema_keys(returned[1], src, offset, seen)
            raise ParseError('unsupported schema expression: ' + expression[:100])
        # Locate the nearest definition in the active lexical block. It may be an
        # empty JsonObject filled by SetObjectField, or a named schema builder.
        prefix = src.text[:offset]
        init_re = r'\b' + re.escape(expression) + r'\s*=\s*(MakeShared\s*<\s*FJsonObject\s*>\s*\(\s*\)|FParamSchemaBuilder\s*\(\s*\))'
        def block_stack(at: int) -> list[int]:
            stack = []
            for index, char in enumerate(src.masked[:at]):
                if char == '{':
                    stack.append(index)
                elif char == '}':
                    if stack:
                        stack.pop()
            return stack
        active = block_stack(offset)
        definitions = [match for match in re.finditer(init_re, prefix)
                       if (lambda scope: active[:len(scope)] == scope)(block_stack(match.start()))]
        if not definitions:
            raise ParseError('unresolved schema variable ' + expression)
        definition = definitions[-1]
        relevant = prefix[definition.start():]
        if 'FParamSchemaBuilder' in definition[1]:
            scrubbed = mask(relevant)
            index = 0
            while index < len(scrubbed):
                if scrubbed[index] in '({[':
                    index = closing(scrubbed, index)
                elif scrubbed[index] == ';':
                    return self.schema_keys(relevant[:index], src, offset, seen)
                index += 1
            raise ParseError('unterminated named schema builder')
        keys = set()
        fields = re.finditer(r'\b' + re.escape(expression) + r'\s*->\s*Set(?:Object)?Field\s*\(', mask(relevant))
        for match in fields:
            end = closing(mask(relevant), match.end() - 1)
            args = arguments(relevant[match.end():end])
            key = literal(args[0])
            if key is None:
                field_offset = definition.start() + match.start()
                owners = [fn for fn in self.functions if fn.path == src.path and fn.start < field_offset < fn.end]
                if not owners:
                    raise ParseError('nonliteral manual schema key')
                owner = min(owners, key=lambda fn: fn.end - fn.start)
                param_names = [re.search(r'(\w+)\s*$', arg)[1] for arg in arguments(owner.params)]
                if args[0] not in param_names:
                    raise ParseError('manual schema helper key is not a parameter')
                arg_index = param_names.index(args[0])
                call_text = src.text[owner.end + 1:offset]
                for call in re.finditer(r'\b' + re.escape(owner.name.split('::')[-1]) + r'\s*\(', mask(call_text)):
                    call_end = closing(mask(call_text), call.end() - 1)
                    values = arguments(call_text[call.end():call_end])
                    forwarded = literal(values[arg_index]) if len(values) > arg_index else None
                    if forwarded is None:
                        raise ParseError('manual schema helper called with nonliteral key')
                    keys.add(forwarded)
                continue
            keys.add(key)
            # Literal aliases attached to the descriptor used as the field value.
            descriptor = args[1] if len(args) > 1 else ''
            alias_assignment = re.search(r'\b' + re.escape(descriptor) + r'\s*->\s*SetArrayField\s*\(\s*TEXT\("aliases"\)\s*,\s*([^;]+)\);', relevant)
            if alias_assignment:
                raise ParseError('manual schema aliases require a builder or explicit parser support')
        return keys, 'manual_variable'

    @staticmethod
    def reads(body: str, params: str) -> set[str]:
        names = set(JSON_PARAM.findall(params))
        if not names:
            # The delegate permits unnamed parameters (common for no-input actions
            # and unavailable #if branches). Output/local object reads are unrelated.
            return set()
        body = mask(body, comments_only=True)
        # Follow only trivial copies/references of the top-level object. Nested
        # TryGetObjectField results deliberately do not enter this set.
        for _ in range(4):
            found = set(re.findall(r'\b(\w+)\s*=\s*(?:' + '|'.join(map(re.escape, names)) + r')\s*;', mask(body)))
            if found <= names:
                break
            names.update(found)
        prefix = r'\b(?:' + '|'.join(map(re.escape, names)) + r')\s*(?:\.\s*Get\s*\(\s*\)\s*)?->\s*'
        keys = set()
        for match in re.finditer(prefix + r'(?:TryGet\w*|Get\w*|HasField)(?:\s*<[^>]+>)?\s*\(', mask(body)):
            first = arguments(body[match.end():closing(mask(body), match.end() - 1)])[0]
            key = literal(first)
            if key is not None:
                keys.add(key)
        return keys

    def run(self) -> dict:
        self.errors = []
        registrations = []
        used_forwarding = set()
        for src in self.sources.values():
            for match in re.finditer(r'\bRegisterAction\s*\(', src.masked):
                location = {'path': src.path, 'line': src.text.count('\n', 0, match.start()) + 1}
                prefix = src.masked[:match.start()].rstrip()
                # Ignore the public declaration and out-of-class implementation.
                if prefix.endswith('::') or re.search(r'\bvoid\s*$', prefix):
                    continue
                row = dict(location, action='<unresolved>', keys=[], reads=[], missing=[], handler='<unresolved>')
                try:
                    end = closing(src.masked, match.end() - 1)
                    args = arguments(src.text[match.end():end])
                    if len(args) < 4:
                        raise ParseError('RegisterAction needs namespace, name, description, handler')
                    namespace, action = literal(args[0]), literal(args[1])
                    if namespace is None:
                        raise ParseError('nonliteral action namespace')
                    if action is None:
                        getter = re.fullmatch(r'([\w:]+)::GetName\s*\(\s*\)', args[1])
                        if not getter:
                            raise ParseError('unsupported action name expression')
                        names = set()
                        for fn in self.resolve(getter[1] + '::GetName', src.path):
                            ret = re.search(r'\breturn\s+(TEXT\s*\(\s*"[^"\\]+"\s*\))', fn.body)
                            if ret:
                                names.add(literal(ret[1]))
                        if len(names) != 1:
                            raise ParseError('action GetName does not return one literal')
                        action = names.pop()
                    row['action'] = namespace + '.' + action
                    if len(args) < 5:
                        row['schema_kind'] = 'absent'
                        self.errors.append(dict(location, action=row['action'], error='RegisterAction has no schema'))
                        declared = set()
                    elif args[4].strip() in ('nullptr', 'NULL', '{}'):
                        row['schema_kind'] = 'null'
                        self.errors.append(dict(location, action=row['action'], error='RegisterAction has null schema'))
                        declared = set()
                    else:
                        declared, row['schema_kind'] = self.schema_keys(args[4], src, match.start())
                    row['keys'] = sorted(declared)
                    static = re.search(r'CreateStatic\s*\(\s*&?\s*([\w:]+)', args[3])
                    if static:
                        functions = self.resolve(static[1], src.path)
                        row['handler'] = functions[0].name
                    elif 'CreateLambda' in args[3]:
                        delegate = args[3]
                        lam = re.search(r'\]\s*\(', mask(delegate))
                        if not lam:
                            raise ParseError('unsupported handler lambda signature')
                        pe = closing(mask(delegate), lam.end() - 1)
                        bs = mask(delegate).find('{', pe)
                        if bs == -1:
                            raise ParseError('handler lambda has no body')
                        be = closing(mask(delegate), bs)
                        row['handler'] = '<lambda>'
                        functions = [Function(src.path, '<lambda>', delegate[lam.end():pe], delegate[bs + 1:be], match.start(), end)]
                    else:
                        raise ParseError('unsupported handler delegate')
                    consumed = set()
                    for fn in functions:
                        consumed.update(self.reads(fn.body, fn.params))
                    row['direct_reads'] = sorted(consumed)
                    for index, entry in enumerate(self.forwarding):
                        if entry.get('action') != row['action']:
                            continue
                        if entry.get('handler') != row['handler']:
                            raise ParseError('forwarding entry handler does not match registration')
                        if (not entry.get('reason') or not entry.get('helper') or not isinstance(entry.get('keys'), list)
                                or not all(isinstance(key, str) for key in entry['keys'])
                                or not isinstance(entry.get('input_arg'), int) or entry['input_arg'] < 0):
                            raise ParseError('forwarding entry requires helper, input_arg, string keys and reason')
                        helper = entry['helper']
                        forwarded = False
                        for fn in functions:
                            for call in re.finditer(r'\b' + re.escape(helper) + r'\s*\(', mask(fn.body)):
                                end = closing(mask(fn.body), call.end() - 1)
                                forwarded_args = arguments(fn.body[call.end():end])
                                if len(forwarded_args) <= entry['input_arg']:
                                    continue
                                value = forwarded_args[entry['input_arg']]
                                if value in JSON_PARAM.findall(fn.params):
                                    forwarded = True
                        if not forwarded:
                            raise ParseError('forwarding helper is not passed the top-level input: ' + helper)
                        consumed.update(entry['keys'])
                        used_forwarding.add(index)
                    row['reads'] = sorted(consumed)
                    row['missing'] = sorted(consumed - declared)
                    for key in row['missing']:
                        self.errors.append(dict(location, action=row['action'], key=key, error='input read is not declared by schema'))
                except ParseError as exc:
                    self.errors.append(dict(location, action=row['action'], error=str(exc)))
                registrations.append(row)
        for index, entry in enumerate(self.forwarding):
            if index not in used_forwarding:
                self.errors.append({'action': entry.get('action', '<unknown>'), 'error': 'unused forwarding entry'})
        return {'summary': {'registrations': len(registrations), 'errors': len(self.errors),
                            'schema_kinds': dict(Counter(r.get('schema_kind', 'unresolved') for r in registrations)),
                            'drift_actions': sum(bool(r['missing']) for r in registrations),
                            'drift_keys': sum(len(r['missing']) for r in registrations)},
                'errors': self.errors, 'registrations': registrations}


def revision_sources(root: Path, revision: str) -> tuple[str, dict[str, str]]:
    """Read a committed source snapshot through git objects; never alter checkout."""
    commit = subprocess.check_output(['git', 'rev-parse', '--verify', '--end-of-options',
                                      revision + '^{commit}'], cwd=root, text=True).strip()
    tree = subprocess.check_output(['git', 'ls-tree', '-r', '-z', commit, '--', 'Source'], cwd=root)
    blobs = []
    for record in tree.split(b'\0'):
        if not record:
            continue
        metadata, raw_path = record.split(b'\t', 1)
        _mode, kind, object_id = metadata.split()
        path = raw_path.decode('utf-8')
        if kind == b'blob' and Path(path).suffix in ('.cpp', '.h') and 'Tests' not in Path(path).parts:
            blobs.append((path, object_id))
    payload = subprocess.run(['git', 'cat-file', '--batch'], cwd=root,
                             input=b''.join(object_id + b'\n' for _, object_id in blobs),
                             capture_output=True, check=True).stdout
    sources, offset = {}, 0
    for path, expected_id in blobs:
        header_end = payload.index(b'\n', offset)
        object_id, kind, size = payload[offset:header_end].split()
        if object_id != expected_id or kind != b'blob':
            raise ParseError('unexpected git object for ' + path)
        offset = header_end + 1
        end = offset + int(size)
        sources[path] = payload[offset:end].decode('utf-8-sig')
        offset = end + 1
    return commit, sources


def check(root: Path, allowlist: Path | None = None, revision: str | None = None) -> dict:
    if not revision and not (root / 'Source').is_dir():
        raise ParseError('Source directory is missing under ' + str(root))
    commit = None
    if revision:
        commit, sources = revision_sources(root, revision)
    else:
        sources = {str(p.relative_to(root)).replace('\\', '/'): p.read_text(encoding='utf-8-sig')
                   for p in (root / 'Source').rglob('*')
                   if p.suffix in ('.cpp', '.h') and 'Tests' not in p.parts}
    if allowlist is None:
        default_allowlist = root / 'Scripts' / 'schema_drift_forwarding.json'
        if default_allowlist.is_file():
            allowlist = default_allowlist
    entries = json.loads(allowlist.read_text(encoding='utf-8-sig')) if allowlist else []
    if not isinstance(entries, list):
        raise ParseError('forwarding allowlist must be a JSON array')
    report = Checker(sources, entries).run()
    if commit:
        report['revision'] = commit
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument('--allowlist', type=Path, help='override root/Scripts/schema_drift_forwarding.json')
    parser.add_argument('--revision', help='read Source from this git commit; use the current forwarding config')
    parser.add_argument('--json', type=Path, help='write full machine-readable inventory')
    args = parser.parse_args()
    try:
        report = check(args.root.resolve(), args.allowlist, args.revision)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print('Schema drift check failed: ' + str(error))
        return 1
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    for error in report['errors']:
        print(f"{error.get('path', '<allowlist>')}:{error.get('line', 0)}: {error['action']}: {error['error']}" +
              (f" ({error['key']})" if 'key' in error else ''))
    print(json.dumps(report['summary'], sort_keys=True))
    return 1 if report['errors'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
