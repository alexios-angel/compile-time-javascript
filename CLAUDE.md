# CLAUDE.md — compile-time-javascript (ctjs)

Header-only C++23. JavaScript syntax proven at COMPILE time by a
CONSTEXPR value parser (`vparse.hpp` — the script can be an NTTP, or a
runtime string through the same call), executed at RUNTIME by a value
tree-walking interpreter (`vinterp.hpp` — dynamic values, real
closures, prototypes, cycle-collected). Namespace `ctjs`. Work on
`main`. Prefer `rg` over `grep`.

(History: the type-level path — ctlark Earley grammar, parse tree as a
TYPE, per-script-specialized interpreter, 10-min PCH bake — was
removed 2026-07. Builds take seconds now; `make -j` is safe again.)

## Build & test
Every `tests/*.cpp` is an EXECUTABLE: NTTP scripts parse during
compilation, then it runs its checks (non-zero exit = failure).
```bash
make                # clang-only; builds the small PCH, compiles + RUNS suites
make clean
cmake -B build && cmake --build build && ctest --test-dir build
python3 tools/gen-v8diff.py   # re-capture node's output for the corpus
```
Flags: `-O2 -pedantic -Wall -Wextra -Werror -Wconversion` — stay
warning-clean. Raised budget: `-fconstexpr-steps=500000000`.

## Layout
- `include/ctjs.hpp` — umbrella (value, builtins, script).
- `include/ctjs/vparse.hpp` — THE parser: context-aware constexpr
  lexer (regex-vs-division by previous token), recursive descent into
  a flat `node` pool (`std::int32_t` indices, `string_view`s into the
  source). `vp::parse` / `vp::is_valid`.
- `include/ctjs/vinterp.hpp` — the tree-walk `vm`: eval/exec over the
  pool, hoisting (`var` + function decls), TDZ, per-iteration for-let,
  classes (home-object `super`, static inheritance via props proto
  chain), EAGER generators (gen_sink buffer), labeled loops.
- `include/ctjs/value.hpp` — runtime `value` (variant via `rc<>`),
  `environment` chains (vars/consts/tdz/function_scope), `context`
  (console, gen_sink, depth guard), coercions, ECMA number printing.
- `include/ctjs/builtins.hpp` — get/set_member (accessor-aware),
  call_value, make_globals (console/Math/JSON/Object/Date/Promise/...),
  the regex engine (backtracking; no lookaround/backrefs).
- `include/ctjs/script.hpp` — NTTP bridge: `src_bytes` re-materializes
  `ctll::fixed_string`'s wide code units as bytes; `is_valid<Src>`,
  `script<Src>` (`.valid`, `.run()`), `run<Src>`, `run_value`.
- `include/ctjs/rc.hpp` + `gc.hpp` + `cfunction.hpp` — constexpr
  refcount pointer, cycle collector, constexpr callable.
- `external/compile-time-lark/` — git SUBMODULE; only
  `ctll/fixed_string.hpp` + `ctll/utilities.hpp` are consumed now.
- `tests/`: `parse.cpp` (static_assert validity contract),
  `runtime.cpp` (behavior vs node), `vparse.cpp`/`vinterp.cpp` (unit),
  `v8diff.cpp` (GENERATED differential suite — regen via tools/).

## Semantics decisions (keep consistent; README documents all)
- V8-ALIGNED: var function-scoped + hoisted; let/const TDZ ("Cannot
  access 'x' before initialization"); const reassign TypeError
  ("Assignment to constant variable."); classic for+let PER-ITERATION
  bindings (step runs in the next copy); method calls bind `this`
  (plain calls: undefined). v8diff byte-compares against node;
  PARSE-GAP = accepted grammar hole (comma operator, destructuring).
- LENIENCY CONTRACT (tests/parse.cpp): `"let x = 1"` (no semi) and
  `let let = 1;` ARE valid; structural breaks (`"if (a { b(); }"`,
  `"let o = { a: };"`) are not. A failed runtime parse still runs its
  parsed prefix; the NTTP form static_asserts validity inside run().
- Generators are EAGER: body runs at call, yields buffer via
  cx.gen_sink, `{next()}` object drains; yield outside a generator =
  SyntaxError at CALL time (sink blinded in non-generator frames).
- Object/class literal keys: `"quoted"` and numeric keys ride the
  computed-key path (evaluating the literal cooks quotes/escapes).
- Strings are bytes; Math.random seeded; Date UTC-only; promises
  settled-only; ECMA-262 Number::toString exactly.

## GOTCHAS
- **node.d defaults to -1 (ALL BITS SET)** — always zero `d` before
  using it as a bitfield on a new node kind (the prop-getter-as-setter
  bug came from exactly this).
- **string_views into parse sources**: an AST borrows its source
  string. eval_subexpr (template `${}`) must keep the parsed string
  named+alive for the walk — a temporary is use-after-free.
- **eval_call resolves indexed callees via get_index** (`xs[0]()` is
  the ELEMENT); name-based get_member would find array methods only.
- **ctlark and ctll are a git SUBMODULE**: `git submodule update
  --init` once; bump = checkout in submodule + commit gitlink. Build
  adds `<sub>/include` + `/ctlark` + `/ctll` to -I (quoted-include
  fallback).
- **single-header** — `make single-header` (needs `quom`); prepends
  LICENSE.
- **Attribution** — CTLL is Hana Dusíková's (via `notre`, from CTRE).
  Preserve `NOTICE` and `LICENSE` (Apache-2.0 w/ LLVM Exceptions).
