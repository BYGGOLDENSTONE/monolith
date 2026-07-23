# Sample Spec 03

This document is pure narrative. It records no architectural choice, carries
no ADR marker, no justification prose, and no YAML frontmatter key. The
indexer must emit zero rows for this file.

## Some Sub-Heading That Is Not A Decision

A paragraph here. Then another paragraph. Words about colour theory.

## Another Sub-Heading

More text. Nothing in either section body should trip the marker scan.

NOTE TO FIXTURE EDITORS: the four trigger tokens are listed in
`Docs/specs/SPEC_MonolithReflectionIntel.md` §3.2 and must never be spelled
out anywhere in this file — writing one here, even inside a sentence that
says the tokens are absent, puts it inside the eight-line lookahead window
of a header above it and makes the indexer emit a row. Describe them by
reference only, as this note does.
