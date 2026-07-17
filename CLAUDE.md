# CLAUDE.md — compile-time-xml (cthtml)

Header-only, compile-time (constexpr) well-formed XML parser for a supported
subset of XML 1.0. A document is a *type*: `cthtml::parse<...>()` yields an
`element` whose every accessor is `constexpr`; malformed or ill-formed XML is a
compile error (or `false` from `is_valid`). Namespace `cthtml`. Compile-time
ONLY — no runtime document load. Work on `main`. Prefer `rg` over `grep`.

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
- `include/cthtml/grammar.hpp` — the XML grammar as a **lark grammar string** (data, parsed by ctlark); relies on ctlark's contextual lexing.
- `include/cthtml/bind.hpp` — lowers the lark tree into document types; enforces well-formedness the grammar can't (see below).
- `include/cthtml/types.hpp` — `element` / `text` node types, `kind` enum, accessors.
- `include/cthtml/views.hpp` — `node_view` / `attribute_view` (uniform runtime views for `operator[]`, iteration).
- `include/cthtml/serialize.hpp` — `serialize()` back to minified XML.
- `external/compile-time-lark/` — git SUBMODULE providing ctlark + ctll (see GOTCHAS).
- `tests/` (`document.cpp`, `cxx17.cpp`), `examples/` (`config`, `introspection`, `iteration`, `wellformed`), `single-header/cthtml.hpp`, `cthtml.cppm` (module, `import std`).

## Public API (all `template <fixed_string input>`)
- `cthtml::is_valid<input>` — `bool`, never a compile error.
- `cthtml::parse<input>()` — returns the root `element`; invalid XML fails the build with a message naming the query to run.
- `cthtml::error_info<input>()` / `error_message<input>()` — syntax failure location + expected tokens (rendered caret).
- `cthtml::bind_error<input>()` — why a document that PARSES is ill-formed: `bind_reason::{mismatched_tag, duplicate_attribute, bad_reference}` (defined in `bind.hpp`), plus `.where`.
- `cthtml::serialize(...)`, `cthtml::for_each_child`, `cthtml::for_each_attribute`, `attributes(...)`.
- `cthtml::debug::{traced_parse, parse_runtime, dump_tokens, dump_grammar}` — ctlark toolbox with the XML grammar baked in.
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
- **Grammar tables via Tablewright** — the only generated table left is
  ctlark's own `lark.hpp` (the grammar-of-grammars), which lives in the
  compile-time-lark submodule; regenerate it THERE (`make regrammar` in
  compile-time-lark). cthtml's own XML grammar is a plain data string in
  `grammar.hpp` — no codegen step.
- **Attribution** — CTLL is Hana Dusíková's (via `notre`, from CTRE); the Lark
  grammar language is the lark-parser project's; cthtml's LL(1)-table lineage
  traces to Tablewright/Desatomat. Preserve `NOTICE` and `LICENSE` (Apache-2.0
  w/ LLVM Exceptions).
