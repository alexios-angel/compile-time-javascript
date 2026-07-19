> **Attribution:** this library is built on the CTLL compile-time LL(1)
> parser from [CTRE](https://github.com/hanickadot/compile-time-regular-expressions)
> by Hana Dusíková, via the [notre](https://github.com/alexios-angel/notre)
> fork, and follows the architecture of its siblings
> [compile-time-html](https://github.com/alexios-angel/compile-time-html),
> [compile-time-python](https://github.com/alexios-angel/compile-time-python) and
> [compile-time-json](https://github.com/alexios-angel/compile-time-json).
> Apache License 2.0 with LLVM Exceptions; see [NOTICE](NOTICE).

# ctjs — compile-time JavaScript

JavaScript **parsed while your code compiles, executed when it runs**.
The script is a template argument: a typo, a missing semicolon or a
bad assignment target is a *compile error* with a caret diagnostic,
and the parse tree becomes a *type*. The interpreter is specialized
over that type, so the C++ optimizer compiles every script into code
generated for exactly that script — at runtime there is no parser, no
bytecode, no dispatch loop. What runs is real JavaScript semantics:
IEEE doubles, closures, reference-semantics arrays and objects,
`try`/`catch`, coercions and all.

```c++
#include <ctjs.hpp>

auto out = ctjs::run<R"(
    function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

    let squares = [1, 2, 3, 4, 5].map((x) => x * x);
    console.log("squares", squares.join(","));
)">();

out.ok();                              // no uncaught exception
out.console();                         // "squares 1,4,9,16,25\n"
out["squares"][2].to<int>();           // 9
out.call("fib", 20).to<int>();         // 6765 - call script functions from C++

static_assert(ctjs::is_valid<"let x = 1;">);
static_assert(!ctjs::is_valid<"let x = 1">);   // no ASI: semicolons required
static_assert(!ctjs::is_valid<"f() = 1;">);    // not an assignment target
```

Hosts inject native functions as script globals, and call script
functions back when events happen — the seam a
[compile-time browser](#roadmap) hangs DOM APIs on:

```c++
auto ui = ctjs::run<R"(
    setTitle("counter");
    let clicks = 0;
    function onClick() { clicks += 1; setTitle("clicked " + clicks); }
)">({
    ctjs::binding{"setTitle", ctjs::native([&](const std::vector<ctjs::value> & a) {
        window_title = a[0].to_string();
    })},
});
ui.call("onClick");   // the host's event loop, driving script handlers
```

## Run it at compile time

The interpreter itself is `constexpr` — so an entire script can execute
during **constant evaluation**, with its result handed back as a
compile-time value:

```c++
static_assert(ctjs::eval<"2 + 3 * 4;">().to<int>() == 14);
static_assert(ctjs::eval<"let s = 0; for (let i = 1; i <= 10; i++) s += i; s;">().to<int>() == 55);
static_assert(ctjs::eval<"function fact(n){ return n<=1 ? 1 : n*fact(n-1); } fact(5);">().to<int>() == 120);
static_assert(ctjs::eval<"'a' + 'b' + 'c';">().to<std::string>() == "abc");
```

Variables, loops, recursion, strings, objects, arrays and closures all
evaluate with **no runtime at all**. It is the *same* interpreter that
runs at runtime (`std::shared_ptr`/`std::function` were replaced with a
constexpr refcounted pointer and a constexpr callable so the value model
is usable in constant evaluation) — `ctjs::eval<Src>()` at compile time,
`ctjs::script<Src>.run()` at runtime.

**As constexpr as possible, with a clean fallback:** a script that
reaches something that genuinely cannot run at compile time — `Math`
(via `<cmath>`), `Date` (via `<chrono>`), `Math.random`, a `throw`, a
fractional-number `toString`, or a host binding — simply isn't a
constant expression there, so you run it at runtime instead. Everything
else is free to fold away.

## Constant folding — evaluated at compile time

A script does not have to be wholly static, but **anything that can be
computed ahead of time is** — during lowering (all `constexpr`), a
partial-evaluation pass collapses constant subexpressions and prunes
dead `?:` / `&&` / `||` / `??` branches *before the interpreter runs*.
A single constant expression is available as a `constexpr`:

```c++
static_assert(ctjs::is_constant<"2 ** 10 + 24;">);
static_assert(ctjs::constant<"2 ** 10 + 24;"> == 1048.0);  // computed at compile time
static_assert(!ctjs::is_constant<"x + 1;">);               // x is dynamic
```

Inside a larger script it folds in place: `1 + 2 + 3 + x` becomes
`6 + x`, `true ? a : sideEffect()` becomes `a`, `false && boom()`
becomes `false` — the runtime only does the genuinely dynamic work. It
also folds pure **string** expressions byte-for-byte: `"a" + "b" + "c"`
becomes `"abc"`, and JS string coercion of the constants it can
reproduce exactly — `"id-" + 42` becomes `"id-42"`, `7 + "up"` becomes
`"7up"`, `"x" + true + null` becomes `"xtruenull"`.

It even evaluates a pure **function** applied to constants. An
immediately-invoked arrow or function expression folds by substituting
the arguments and re-folding — `(x => x * x)(5)` becomes `25`. So do
calls to **named** functions: a whole-program pass collects top-level
`function name(...) { return <expr>; }` definitions and folds every call
made with constant arguments, recursively —

```js
function sq(x)   { return x * x; }
function cube(x) { return x * sq(x); }                    // calls sq
function fact(n) { return n <= 1 ? 1 : n * fact(n - 1); } // recursion

let a = cube(3);   // -> 27 at compile time (the inner sq folds too)
let b = fact(6);   // -> 720
let c = sq(k);     // NOT folded: k is dynamic — runs against the real def
```

Recursion is bounded by a depth budget (so a branching recursion like
`fib` with a large constant argument bails to the interpreter rather than
exploding the compile), and any call whose body touches something dynamic
simply doesn't reduce — soundness without a separate purity analysis.

The fold is deliberately **sound over speed**: it reproduces the
interpreter bit-for-bit, so it folds integer literals, booleans, `null`
and strings (not fractional/exponent literals, whose runtime
`Number::toString` it will not second-guess) with the
IEEE-deterministic operators.

## What is supported (v0.1)

* **values**: numbers (IEEE-754 doubles, hex/decimal/exponent
  literals), strings (`'`/`"`, escapes incl. `\u`/`\x`), booleans,
  `null`, `undefined`, arrays, objects, first-class functions

* **expressions**: the full operator ladder — assignment (`=`, `+=`,
  ... `**=`), ternary, `??`, `||`/`&&` (short-circuit), `==`/`!=` and
  `===`/`!==` (spec coercion rules), relational, `+` (concat rules),
  `-` `*` `/` `%` `**`, unary `!` `-` `+` `typeof`, `++`/`--` (pre and
  post), calls, `obj.prop`, `obj[expr]`, array/object literals,
  grouping

* **functions**: declarations (hoisted within their scope),
  expressions, arrow functions (expression- and block-bodied), REAL
  closures over lexical environment chains, recursion (soft
  `RangeError` at depth 256), first-class use as values/arguments

* **statements**: `let`/`const`/`var` (multi-declarator), blocks with
  lexical scope, `if`/`else`, `while`, `do`/`while`, classic `for`,
  `for...of` (arrays and strings, per-iteration binding),
  `break`/`continue`, `return`, `throw`, `try`/`catch`/`finally`

* **runtime library**: `console.log` (captured, node-style
  formatting), `Math` (floor/ceil/round/trunc/abs/sqrt/sign/pow/
  min/max/random/PI/E), `JSON.stringify`, `parseInt`, `parseFloat`,
  `isNaN`, `isFinite`, `String`, `Number`, `Boolean`,
  `Array.isArray`; array methods (push/pop/shift/unshift/slice/
  indexOf/includes/join/reverse/concat/map/filter/forEach/reduce,
  `.length`), string methods (slice/indexOf/includes/startsWith/
  endsWith/toUpperCase/toLowerCase/trim/split/charAt/charCodeAt/
  repeat/replace/replaceAll/padStart/padEnd, `.length`), number
  methods (toString/toFixed)

* **semantics**: JS truthiness, ToNumber/ToString coercion, the
  ECMA-262 `Number::toString` algorithm (`0.1 + 0.2` prints
  `0.30000000000000004`, `1e21` prints `1e+21`), `NaN`, `typeof null
  === "object"`, reference semantics for arrays/objects, `Math.random`
  deterministic-seeded (reproducible runs)

**V8-aligned semantics (v0.2):**

- `var` is function-scoped and hoists (reads before the declaration see `undefined`)
- `let`/`const` are block-scoped with a temporal dead zone (access before initialization throws V8's `ReferenceError`)
- `const` reassignment throws V8's `TypeError`
- classic `for` with `let` creates per-iteration bindings (closures capture each iteration's values)
- method calls bind `this` to the receiver

Verified against node by the generated differential suite:
`python3 tools/gen-v8diff.py && make tests/v8diff` re-captures V8's
output for every corpus snippet and byte-compares.

**Documented deviations:**

- no ASI — semicolons are required (a missing one is a *compile* error, which is rather the point)
- plain calls see `this === undefined` (module semantics; sloppy-mode `globalThis` is not modeled)
- reserved words cannot be used as identifiers (`let let = 1;` is a syntax error), but they remain legal as property names (`p.catch(f)`, `{ class: 1 }`) — the contextual words `of`/`async`/`get`/`set`/`static` stay usable as identifiers
- strings are bytes (UTF-8 passes through, `.length` counts bytes)
- `Math.random` is seeded deterministically
- **promises are the SETTLED subset** — the engine is synchronous, so `async function` / top-level `await` / then/catch/finally / `Promise.resolve|reject|all` all exist, but a promise is fulfilled or rejected the moment it is created and handlers run immediately instead of on a microtask queue (host natives hand scripts pre-resolved promises — compile-time-browser's `await fetch(url)`; `new Promise(executor)` is deliberately absent since an executor implies pending state)
- optional chaining short-circuits PER LINK — `a?.b.c` still throws when `a?.b` is undefined (write `a?.b?.c`), unlike V8's whole-chain skip
- **generators are EAGER** — the body runs to completion on the call, yields buffer up, and the returned iterator (a plain `{next()}` object, which `for...of` also speaks for hand-rolled iterators) drains the buffer, so infinite generators hang and `next(v)` cannot feed values back in
- objects have a real `[[Prototype]]` chain (`Object.create`/`getPrototypeOf`/`setPrototypeOf`, `__proto__`, `Fn.prototype`); `instanceof` walks it
- **regex literals lex greedily** (the lexer has no parser context), so a regex body may not contain a BARE space, `;` or `,` — write `[ ]`, `[;]`, `[,]` or `\x20` — which also keeps `a / b / c` division safe; the regex engine has no lookaround, backreferences or named groups, `exec` results carry no `.index/.input`, and `replace` takes string templates (`$&`, `$1`…), not callbacks
- classes desugar to prototypes: methods live once on a shared prototype, `class B extends A` links the chain, and `super()` / `super.method()` work; `static` members and fields (`static x = 1`), instance fields (`count = 0`) and computed member names (`[expr]() {}`) are supported. A derived class with no own constructor forwards its arguments to the base
- **`Date` is UTC-only** — `new Date(ms)`/`new Date()`/`new Date(str)`, `Date.parse` (the ISO-8601 subset `toISOString` emits, plus date-only forms), the `getUTC*`/`get*` getters (local aliases UTC), `getTime`, `getDay`, `toISOString`; no setters; `Date.now()` is the one impure global besides `console`, so hosts wanting determinism rebind it

**Not yet:** non-UTC time zones, `Symbol`/iterators-by-`Symbol.iterator`,
`Proxy`/`Reflect`, tagged template literals, labelled-block `break` to a
non-loop, property descriptors (`Object.defineProperty`, real
`freeze`).

## API

```c++
// syntax as a static property (never a compile error):
template <ctll::fixed_string Src> constexpr bool ctjs::is_valid;
ctjs::error_info<Src>();     // stage/kind, byte offset, line, column
ctjs::error_message<Src>();  // rendered caret diagnostic, at compile time

// parse at compile time, run now:
ctjs::run<Src>() -> ctjs::run_result;
ctjs::run<Src>({ctjs::binding{"name", value}, ...});   // host globals
ctjs::script<Src>.run(...);                            // same, reusable form

// the result:
out.ok();                  // false if an exception escaped
out.exception();           // the thrown value; exception_message() renders it
out.console();             // everything console.log printed
out.result();              // last expression-statement value (REPL-style)
out["name"];               // a global the script left behind
out.call("fn", args...);   // invoke a script function from C++
```

`ctjs::value` is the runtime value: `is_number()`/`is_string()`/...,
`to<double>()`, `to<int>()`, `to<bool>()`, `to<std::string>()` (JS
coercion rules), `operator[]` drilling for objects and arrays with
null-object misses, `truthy()`, `typeof_string()`.
`ctjs::native(callable)` wraps a C++ lambda as a JS function value —
taking `(const std::vector<value>&)` or `(context&, const
std::vector<value>&)` when it needs to call script closures back via
`ctjs::call_value`.

## Debugging

A script that will not parse fails the build with a `static_assert`
naming the query to run; the queries work at compile time:

```c++
constexpr auto info = ctjs::error_info<"let x = 1">();
// info.kind, info.position (9), info.line, info.column

constexpr auto why = ctjs::error_message<"let x = 1">();
// the rendered diagnostic: location, snippet with a caret, expected terminals
```

`ctjs::debug` bundles the [ctlark debugging
toolbox](../compile-time-lark#debugging) with the JavaScript grammar
baked in: `traced_parse<Src>()`, `parse_runtime(text)` (runtime
strings against the compile-time tables — handy for grammar work),
`dump_tokens<Src>()` and `dump_grammar()`.

Runtime failures are values, not crashes: an uncaught `throw` lands in
`out.exception()`, and the library throws the spec error shapes
(`TypeError: Cannot read properties of null (reading 'x')`,
`ReferenceError: y is not defined`, `RangeError: Maximum call stack
size exceeded`).

## How it works

The grammar layer is
[ctlark](https://github.com/alexios-angel/compile-time-lark)
(compile-time Lark). [`grammar.hpp`](include/ctjs/grammar.hpp) is a
token-level *lark grammar string* with a precedence ladder of
`?`-inlined rules, so operator binding lives in the grammar and the
parse tree IS the expression structure; operator families are single
terminals (`EQ_OP`, `ASSIGN_OP`, ...) kept apart by longest-match,
which keeps the Earley tables a fraction of the naive size. Keywords
are string literals: contextual lexing plus longest-match keeps `let`
a keyword exactly where one can appear while `letter` stays a NAME.

[`lower.hpp`](include/ctjs/lower.hpp) maps the parse tree to a
type-level AST ([`ast.hpp`](include/ctjs/ast.hpp)) by rule name.
[`interp.hpp`](include/ctjs/interp.hpp) specializes `eval_`/`exec_`
over those types — instantiation happens per script node, so the
inliner sees straight-line code for each script — over a dynamic
runtime ([`value.hpp`](include/ctjs/value.hpp),
[`builtins.hpp`](include/ctjs/builtins.hpp)): a variant value, shared
environment chains for closures, C++ exceptions carrying thrown JS
values, and array/string methods materialized as receiver-bound native
functions (`arr.push` is itself a value).

**The one-time cost:** compiling the grammar's Earley tables is a
heavy constexpr evaluation (several minutes, a few GB). It belongs in
the **precompiled header** — `make pch` (the builds here do it
automatically) bakes `ctjs.hpp` once, and every translation unit after
starts from the result, paying only its own scripts' parses. Budget-
raising constexpr flags are set by the Makefile and the CMake
interface (`CTJS_CONSTEXPR_LIMITS`):

```
clang:  -fconstexpr-steps=500000000 -fconstexpr-depth=1024 -fbracket-depth=2048
gcc:    -fconstexpr-ops-limit=3000000000 -fconstexpr-loop-limit=10000000 -fconstexpr-depth=1024
```

ctlark and ctll come in as a git submodule
(`external/compile-time-lark` — clone with `--recurse-submodules` or
run `git submodule update --init`); never edit under `external/`. The
build adds the submodule's include directories so the headers'
relative `"../ctlark.hpp"`-style includes resolve, and the CMake
install flattens everything back to `include/{ctjs,ctlark,ctll}`.

## Building and integrating

Header-only, C++20. Pick whichever fits your project:

**CMake, as a subdirectory or via FetchContent:**

```cmake
add_subdirectory(compile-time-javascript)   # or FetchContent_MakeAvailable(ctjs)
target_link_libraries(your-target PRIVATE ctjs::ctjs)
```

**CMake, installed** (`cmake -B build && cmake --install build`):

```cmake
find_package(ctjs 0.1 REQUIRED)
target_link_libraries(your-target PRIVATE ctjs::ctjs)
```

The install also ships a `pkg-config` file (`ctjs.pc`). Tests and
examples build only when ctjs is the top-level project
(`CTJS_BUILD_TESTS`, `CTJS_BUILD_EXAMPLES`). CPack can produce TGZ/ZIP
archives (plus DEB/RPM where the tooling exists), and
`-DCTJS_MODULE=ON` builds `ctjs.cppm` as a named C++ module
(experimental).

**No build system:** add `include/` plus the submodule's
`external/compile-time-lark/include` (and its `ctlark`/`ctll`
subdirectories) to your include path, or copy the amalgamated
[`single-header/ctjs.hpp`](single-header/ctjs.hpp)
(regenerate with `make single-header`, which needs the
[quom](https://pypi.org/project/quom/) tool).

Run the tests — scripts parse during compilation, then the binaries
execute their checks:

```bash
git submodule update --init            # ctlark + ctll (once, after cloning)
make                                   # builds the PCH once, compiles + RUNS the suites
make CXX=clang++
# or through CMake/CTest:
cmake -B build && cmake --build build && ctest --test-dir build
```

Runnable demos live in [`examples/`](examples/): `hello` (the hero
demo) and `host` (native bindings + event handlers — the browser
seam).

## Roadmap

ctjs is the second brick of a compile-time web stack, after
[compile-time-html](https://github.com/alexios-angel/compile-time-html):
**compile-time-css** comes next, and they meet in
**compile-time-browser** — HTML, CSS and JS parsed at compile time and
lowered into an SDL3 application, as if the page had been hand-written
as native code. ctjs's host-binding seam (`ctjs::binding`,
`run_result::call`) is exactly where the browser's DOM API will plug
in.

## License

Apache License 2.0 with LLVM Exceptions (see [LICENSE](LICENSE)).
The CTLL parser is Hana Dusíková's work, via notre; see
[NOTICE](NOTICE).
