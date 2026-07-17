# CLAUDE.md — compile-time-html (cthtml)

Header-only, compile-time (constexpr) HTML5 parser. A document is a
*type*: `cthtml::parse<...>()` always yields `html > (head, body)` like a
browser DOM; broken markup is a compile error (or `false` from
`is_valid`). HTML5 conveniences (void elements, optional end tags,
implied html/head/body, case-insensitive names, boolean/unquoted
attributes, DOCTYPE, raw-text script/style) parse silently; author
mistakes (stray/crossing close tags, duplicate attributes, `<div/>`)
are errors. Namespace `cthtml`. Compile-time ONLY — no runtime document
load. Work on `main`. Prefer `rg` over `grep`.

## Build & test — "compiling the tests IS the test"
Tests under `tests/*.cpp` are `static_assert` suites; each compiles to a `.o`.
```bash
make                                   # C++20 (default), one .o per test
make CXX=clang++                       # clang
make CXX=clang++ CXX_STANDARD=17       # C++17 path (variable-form API)
make clean
cmake -B build && cmake --build build && ctest --test-dir build
```
Flags are `-O2 -pedantic -Wall -Wextra -Werror -Wconversion` — keep every
change warning-clean. A PCH of the umbrella header (`make pch`, done
automatically) compiles the grammar + tables ONCE; TUs start from the baked
result. gcc uses `include/cthtml.hpp.gch`; clang uses `cthtml.pch` (`-include-pch`).

## Layout
- `include/cthtml.hpp` — umbrella (includes the pieces below); public API.
- `include/cthtml/grammar.hpp` — the HTML grammar as a **lark grammar string**, deliberately FLAT (HTML nesting is not context-free): it lexes a chunk stream; raw-text `*_BODY` terminals ride ctlark's contextual lexing.
- `include/cthtml/bind.hpp` — lowers chunks: lowercased names, 3 attribute value flavours + booleans, HTML character-reference decoding (never fails), raw-body close-tag stripping.
- `include/cthtml/treebuild.hpp` — HTML5 tree construction, TWO passes: a value-level validator (name stack → first `bind_error_t`; all `is_valid` costs) and a type-level fold (frame stack → the document type; total, never errors on its own).
- `include/cthtml/entities.hpp` — GENERATED WHATWG named-reference table; regenerate with `python3 tools/gen-entities.py`, never edit by hand.
- `include/cthtml/types.hpp` — `element` / `text` node types, `kind` enum, accessors, case-insensitive matching (`ascii_iequals`, `is_void_tag`).
- `include/cthtml/views.hpp` — `node_view` / `attribute_view` (uniform runtime views for `operator[]`, iteration).
- `include/cthtml/serialize.hpp` — `serialize()` back to minified HTML (voids bare, boolean attrs bare, raw script/style bodies unescaped).
- `external/compile-time-lark/` — git SUBMODULE providing ctlark + ctll (see GOTCHAS).
- `tests/` (`document.cpp` — a real page, `html5.cpp` — the feature matrix, `cxx17.cpp`), `examples/` (`page`, `wellformed`, `introspection`, `iteration`), `single-header/cthtml.hpp`, `cthtml.cppm` (module, `import std`).

## Public API (all `template <fixed_string input>`)
- `cthtml::is_valid<input>` — `bool`, never a compile error.
- `cthtml::parse<input>()` — the `html` root element; invalid HTML fails the build with a message naming the query to run.
- `cthtml::error_info<input>()` / `error_message<input>()` — syntax failure location + expected tokens (rendered caret).
- `cthtml::bind_error<input>()` — why a document that PARSES is rejected: `bind_reason::{stray_end_tag, mismatched_tag, duplicate_attribute, self_closing_non_void, depth_overflow}` (defined in `bind.hpp`), plus `.where`.
- `cthtml::serialize(...)`, `cthtml::for_each_child`, `cthtml::for_each_attribute`, `attributes(...)`.
- `cthtml::debug::{traced_parse, parse_runtime, dump_tokens, dump_grammar}` — ctlark toolbox with the HTML grammar baked in.
- Diagnostics macros: `CTLARK_VERBOSE_ERRORS`, `CTLARK_DEBUG`, `CTLARK_CONSTEXPR_ASSERT`.

## Conventions
- C++17/C++20 split via `CTLL_CNTTP_COMPILER_CHECK`: C++20 takes string-literal
  NTTPs; C++17 takes a `const auto&` to a `constexpr ctll::fixed_string`
  variable with linkage. Test both — `cxx17.cpp` guards the C++17 form.
- Constexpr/Earley parsing needs HUGE budgets (Makefile sets them; CMake
  attaches via `CTHTML_CONSTEXPR_LIMITS`, opt out `-DCTHTML_CONSTEXPR_LIMITS=OFF`):
  - gcc: `-fconstexpr-ops-limit=3000000000 -fconstexpr-loop-limit=10000000 -fconstexpr-depth=1024`
  - clang: `-fconstexpr-steps=500000000 -fconstexpr-depth=1024 -fbracket-depth=2048`
  Hitting the compiler's own step cap is a distinct failure from the library's
  queryable overflow/depth errors.
- CMake toggles: `CTHTML_PCH`, `CTHTML_BUILD_TESTS`, `CTHTML_BUILD_EXAMPLES`, `CTHTML_CXX_STANDARD` (default 20), `CTHTML_MODULE`.

## HTML semantics decisions (keep these consistent)
- Every parse synthesizes `html > (head, body)`; explicit
  `<html>/<head>/<body>` tags merge attributes, never nest. Metadata
  before content → head; after content starts → stays in body.
- Auto-close applies at the TOP of the open stack only (documented v0.1
  divergence: `<p>a<b>c<p>` nests the second p inside b).
- `</body>`/`</html>` close open elements only through omissible end
  tags; EOF closes anything. `</head>` is only valid while in head.
- Whitespace-only text is dropped except inside `<pre>`/`<textarea>`;
  one leading newline after `<pre>`/`<textarea>` open is stripped.
- References decode leniently (unknown → literal); entity names are
  case-SENSITIVE but tag/attr names fold to lowercase at lift time —
  lookups fold too.
- Raw-text limits (documented): a literal `</script` in script content
  must be followed by `>` or whitespace-`>`; `a < b` in text is a lex
  error (write `&lt;`).

## GOTCHAS
- **ctlark and ctll are a git SUBMODULE, never edit here:**
  `external/compile-time-lark` — run `git submodule update --init` once
  after cloning; bump by checking out a new commit inside the submodule and
  committing the gitlink. The build adds `<sub>/include` AND
  `<sub>/include/ctlark` / `<sub>/include/ctll` to the include path so the
  headers' relative `"../ctlark.hpp"`-style includes resolve via the
  quoted-include fallback; the CMake install flattens everything back to
  include/{cthtml,ctlark,ctll}. Regenerate the single-header after bumps.
- **single-header** — `make single-header` (needs `quom`); prepends `LICENSE`,
  amalgamates `include/cthtml.hpp` into `single-header/cthtml.hpp`.
- **entities.hpp is generated** — `python3 tools/gen-entities.py` (data
  from CPython's `html.entities.html5`, i.e. the WHATWG table); commit
  the result, never hand-edit.
- **Grammar tables via Tablewright** — the only generated table left is
  ctlark's own `lark.hpp` (the grammar-of-grammars), which lives in the
  compile-time-lark submodule; regenerate it THERE (`make regrammar` in
  compile-time-lark). cthtml's own HTML grammar is a plain data string in
  `grammar.hpp` — no codegen step.
- **Attribution** — CTLL is Hana Dusíková's (via `notre`, from CTRE); the Lark
  grammar language is the lark-parser project's; the entity data is the
  WHATWG's. Preserve `NOTICE` and `LICENSE` (Apache-2.0 w/ LLVM Exceptions).
