# CLAUDE.md — compile-time-javascript (ctjs)

Header-only C++20. JavaScript **parsed at COMPILE time** (the script is
an NTTP; a syntax error is a compile error; the AST is a TYPE) and
**executed at RUNTIME** (the interpreter is specialized per script, so
the optimizer emits script-specific code; values are dynamic with real
closures). Namespace `ctjs`. Work on `main`. Prefer `rg` over `grep`.

## Build & test
Every `tests/*.cpp` is an EXECUTABLE: its scripts parse during
compilation, then it runs its checks (non-zero exit = failure).
```bash
make                # builds the grammar PCH once (SLOW - see below), compiles + RUNS suites
make CXX=clang++
make clean
cmake -B build && cmake --build build && ctest --test-dir build
```
Flags: `-O2 -pedantic -Wall -Wextra -Werror -Wconversion` — stay
warning-clean. C++20 ONLY (no C++17 path; the runtime layer needs it).

**THE ONE-TIME COST:** building the Earley tables for the JS grammar is
a ~10-minute, ~4 GB constexpr evaluation. It lives in the PCH
(`make pch`, automatic): gcc `include/ctjs.hpp.gch`, clang `ctjs.pch`
via `-include-pch`. Examples reuse the same PCH (`examples/Makefile`).
NEVER build two of these repos in parallel on this machine (7.5 GB
WSL2) — see the memory notes; `make -j1` for anything grammar-touching.

## Layout
- `include/ctjs.hpp` — umbrella; public compile-time API (`is_valid`, `error_info/message`, `debug::*`).
- `include/ctjs/grammar.hpp` — the JS subset as a token-level **lark grammar string**: precedence LADDER of `?`-inlined rules (the tree IS the expression structure); operator FAMILIES as single terminals (`EQ_OP`, `REL_OP`, `MUL_OP`, `ASSIGN_OP`, `INCDEC`) to keep table size down; a dedicated `lhs` rule makes bad assignment targets syntax errors; semicolons REQUIRED.
- `include/ctjs/ast.hpp` — empty-template-struct AST nodes; literal spellings ride as `ctlark::text<...>` params, cooked lazily at runtime (`num_of`/`str_of`, static per instantiation).
- `include/ctjs/lower.hpp` — parse tree → AST, dispatched on RULE NAME via `if constexpr (Name::view() == "...")` chains (no char-pack tag types).
- `include/ctjs/value.hpp` — runtime `value` (variant: undefined/null/bool/double/string/array/object/function via shared_ptr = JS reference semantics), `environment` chains (closures), `context` (console capture, `last`, depth guard), coercions, `strict_equals`/`loose_equals`, ECMA `number_to_string`, `js_throw`.
- `include/ctjs/builtins.hpp` — `get_member`/`set_member`/`get_index`/`set_index` (array/string/number methods materialize as receiver-BOUND native fns), `call_value`, `make_globals()` (console/Math/JSON/parseInt/...), node-style `inspect` for console.log.
- `include/ctjs/interp.hpp` — `eval_<Expr>`/`exec_<Stmt>` specializations; `flow` enum for break/continue/return; C++ exceptions for `throw`; `fn_maker` builds closures capturing the env chain; function declarations HOIST per scope.
- `include/ctjs/script.hpp` — `script<Src>.run(bindings)`, `run_result` (ok/exception/console/result/operator[]/call), `ctjs::binding`, `ctjs::native`.
- `external/compile-time-lark/` — git SUBMODULE (ctlark + ctll). Never edit here.
- `tests/` (`parse.cpp` — compile-time static_asserts, `runtime.cpp` — behavior vs node), `examples/` (`hello`, `host`).

## Semantics decisions (keep consistent; all in README too)
- V8-ALIGNED (v0.2): `var` function-scoped + hoisted (recursive
  pre-declare at function entry); let/const TDZ ("Cannot access 'x'
  before initialization"); const reassignment → TypeError ("Assignment
  to constant variable."); classic `for`+let = PER-ITERATION bindings
  (step runs in the next copy); method calls bind `this` (plain calls:
  undefined — module semantics, no sloppy globalThis). Differential
  suite: `python3 tools/gen-v8diff.py && make tests/v8diff` (captures
  node's output per corpus snippet, byte-compares; PARSE-GAP = grammar
  hole, not a wrong answer).
- Still deliberate: no ASI. Keywords usable as names where unambiguous
  (`let let`). Strings are bytes. `Math.random` seeded
  deterministically. Array sort default = lexicographic (spec).
- Number printing follows ECMA-262 Number::toString exactly (shortest
  digits via to_chars, fixed for exponent in (-7,21), else exponential).
  Careful: `from_chars` rejects the `+` in `to_chars` exponents.
- Errors: spec shapes as `{name, message}` objects thrown via
  `js_throw`; uncaught → `run_result.exception()`. TypeError message
  strings mirror node ("Cannot read properties of null (reading 'x')").

## GOTCHAS
- **Grammar changes re-bake the PCH** (~10 min). Iterate on grammar
  with `ctjs::debug::parse_runtime` (runtime inputs, compile once).
- **ctlark tie-breaks**: keyword literals beat NAME on exact ties via
  contextual candidates; `letter` stays NAME via longest-match. The
  `lhs INCDEC` postfix rule and `lhs ASSIGN_OP` keep targets sane.
- **ctlark and ctll are a git SUBMODULE**: `git submodule update
  --init` once; bump = checkout in submodule + commit gitlink. Build
  adds `<sub>/include` + `/ctlark` + `/ctll` to -I (quoted-include
  fallback for `"../ctlark.hpp"`).
- **single-header** — `make single-header` (needs `quom`); prepends LICENSE.
- **Attribution** — CTLL is Hana Dusíková's (via `notre`, from CTRE);
  the Lark grammar language is the lark-parser project's. Preserve
  `NOTICE` and `LICENSE` (Apache-2.0 w/ LLVM Exceptions).
