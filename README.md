> **Attribution:** this library builds on
> [compile-time-containers](https://github.com/alexios-angel/compile-time-containers)
> (`ctc::string` carries NTTP scripts, `ctc::cfunction` type-erases
> native functions) and follows the architecture of its siblings
> [compile-time-html](https://github.com/alexios-angel/compile-time-html),
> [compile-time-python](https://github.com/alexios-angel/compile-time-python) and
> [compile-time-json](https://github.com/alexios-angel/compile-time-json).
> Apache License 2.0 with LLVM Exceptions; see [NOTICE](NOTICE).

# ctjs — compile-time JavaScript

JavaScript **parsed while your code compiles, executed when it runs**.
The script can ride as a template argument: its syntax is proven during
compilation by a `constexpr` value parser, so a structural mistake in
`ctjs::script<...>` fails the build. The *same* parser accepts ordinary
runtime strings — an embedded asset, a network payload — and both paths
execute through one tree-walking interpreter with real JavaScript
semantics: IEEE doubles, closures, reference-semantics arrays and
objects, prototypes, classes, `try`/`catch`, coercions and all.

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
static_assert(!ctjs::is_valid<"let o = { a: };">);   // structural breaks fail
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

Runtime strings go through `ctjs::run_value` — the identical machinery,
no template argument:

```c++
std::string src = load_script();               // decided at runtime
auto out = ctjs::run_value(src, {/* bindings */});
```

## Validity is lenient by design

The parser mirrors what the interpreter can run, not a style guide.
Recoverable slips parse; structural breaks do not:

```c++
static_assert(ctjs::is_valid<"let x = 1">);       // trailing semicolon recoverable
static_assert(ctjs::is_valid<"let let = 1;">);    // keywords as names where unambiguous
static_assert(!ctjs::is_valid<"if (a { b(); }">); // lost a paren
static_assert(!ctjs::is_valid<"while (">);        // truncated
static_assert(!ctjs::is_valid<"f(1, 2">);         // unclosed call

static_assert(ctjs::script<"let x = 1;">.valid);  // queryable on the NTTP form
```

A runtime string that fails to parse still runs its parsed prefix; the
NTTP form (`script<Src>.run()`) `static_assert`s validity at the point
of use so an invalid literal cannot slip into an executable.

## What is supported (v0.2)

* **values**: numbers (IEEE-754 doubles, hex/decimal/exponent
  literals), strings (`'`/`"`, escapes incl. `\u`/`\x`), template
  literals with `${}` interpolation, booleans, `null`, `undefined`,
  arrays, objects, first-class functions, regex literals

* **expressions**: the full operator ladder — assignment (`=`, `+=`,
  ... `**=`, `||=`/`&&=`/`??=`), ternary, `??`, `||`/`&&`
  (short-circuit), `==`/`!=` and `===`/`!==` (spec coercion rules),
  relational + `instanceof` + `in`, `+` (concat rules), `-` `*` `/`
  `%` `**`, unary `!` `-` `+` `~` `typeof` `delete` `void`, `++`/`--`
  (pre and post), optional chaining (`?.`, `?.[]`, `?.()`), spread in
  calls/arrays/objects, calls, `obj.prop`, `obj[expr]`, grouping

* **functions**: declarations (hoisted within their scope),
  expressions, arrow functions (expression- and block-bodied), default
  and rest parameters, REAL closures over lexical environment chains,
  recursion (soft `RangeError` at depth 256), `async` (settled
  promises), generators (eager — see deviations)

* **classes**: methods on a shared prototype, `extends` with
  `super()` / `super.method()` (home-object semantics — a method
  resolves `super.*` against its defining class no matter when it is
  invoked), getters/setters, static members with inheritance through
  the chain, instance and static fields, computed member names

* **statements**: `let`/`const`/`var` (multi-declarator), blocks with
  lexical scope, `if`/`else`, `while`, `do`/`while`, classic `for`,
  `for...of` (arrays, strings, generator objects and hand-rolled
  `{next()}` iterators), `for...in`, labeled loops with
  `break`/`continue` to a label, `return`, `throw`,
  `try`/`catch`/`finally`, `switch`

* **runtime library**: `console.log` (captured, node-style
  formatting), `Math`, `JSON.stringify`/`parse`, `parseInt`,
  `parseFloat`, `isNaN`, `isFinite`, `String`, `Number`, `Boolean`,
  `Object` (keys/values/entries/assign/create/getPrototypeOf/
  setPrototypeOf/fromEntries), `Array.isArray`/`from`/`of`, `Date`
  (UTC subset), `Promise` (settled subset), the array/string/number
  methods (map/filter/reduce/slice/splice/find/sort/... ,
  split/replace/match/repeat/padStart/... , toString/toFixed), regex
  `test`/`exec`

* **semantics (V8-aligned, verified against node)**: `var` is
  function-scoped and hoists (reads before the declaration see
  `undefined`); `let`/`const` have a temporal dead zone (access before
  initialization throws V8's `ReferenceError`); `const` reassignment
  throws V8's `TypeError`; classic `for` + `let` creates per-iteration
  bindings (closures capture each iteration's values, the step runs in
  the next copy); method calls bind `this` to the receiver; JS
  truthiness and ToNumber/ToString coercions; the ECMA-262
  `Number::toString` algorithm (`0.1 + 0.2` prints
  `0.30000000000000004`); spec error shapes with node's messages

The differential suite re-captures V8's output for every corpus
snippet and byte-compares: `python3 tools/gen-v8diff.py`, then rebuild
and rerun the tests.

**Documented deviations:**

- plain calls see `this === undefined` (module semantics; sloppy-mode `globalThis` is not modeled)
- strings are bytes (UTF-8 passes through, `.length` counts bytes)
- `Math.random` is seeded deterministically
- **promises are the SETTLED subset** — the engine is synchronous, so `async function` / `await` / then/catch/finally / `Promise.resolve|reject|all` all exist, but a promise is fulfilled or rejected the moment it is created and handlers run immediately instead of on a microtask queue (`new Promise(executor)` is deliberately absent since an executor implies pending state)
- **generators are EAGER** — the body runs to completion on the call, yields buffer up, and the returned iterator drains the buffer; infinite generators hang, `next(v)` cannot feed values back in, and `yield*` yields the operand value itself; `yield` outside a generator throws `SyntaxError` at call time
- optional chaining short-circuits PER LINK — `a?.b.c` still throws when `a?.b` is undefined (write `a?.b?.c`)
- the regex engine has no lookaround, backreferences or named groups; `exec` results carry no `.index`/`.input`; `replace` takes string templates (`$&`, `$1`…), not callbacks
- **`Date` is UTC-only** — no setters, local-time getters alias UTC; `Date.now()` is the one impure global besides `console`, so hosts wanting determinism rebind it
- **parse gaps** (accepted, tracked by the differential suite): the comma operator inside expressions, and array/object destructuring patterns

## API

```c++
// syntax as a value (never a compile error):
template <ctc::string Src> constexpr bool ctjs::is_valid;
ctjs::script<Src>.valid;            // same fact on the reusable form
ctjs::vp::is_valid(std::string_view);   // the runtime spelling
ctjs::vp::parse(std::string_view);      // the AST, with error + offset on failure

// parse at compile time, run now:
ctjs::run<Src>() -> ctjs::run_result;
ctjs::run<Src>({ctjs::binding{"name", value}, ...});   // host globals
ctjs::script<Src>.run(...);                            // same, reusable form
ctjs::run_value(src_string, ...);                      // runtime strings

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

Runtime failures are values, not crashes: an uncaught `throw` lands in
`out.exception()`, and the library throws the spec error shapes
(`TypeError: Cannot read properties of null (reading 'x')`,
`ReferenceError: y is not defined`, `RangeError: Maximum call stack
size exceeded`).

## How it works

[`vparse.hpp`](include/ctjs/vparse.hpp) is a fully `constexpr`
hand-written recursive-descent parser: a context-aware lexer (it knows
when `/` is division and when it opens a regex literal) feeding a
Pratt-style expression climber, producing a flat `node` pool indexed
by `std::int32_t` — `string_view`s into the source, no allocation per
node kind. Because it is a value function, the SAME code checks an
NTTP script inside a `static_assert` and a network payload at runtime.

[`vinterp.hpp`](include/ctjs/vinterp.hpp) walks that pool over a
dynamic runtime ([`value.hpp`](include/ctjs/value.hpp),
[`builtins.hpp`](include/ctjs/builtins.hpp)): a variant value, shared
environment chains for closures (with a cycle collector,
[`gc.hpp`](include/ctjs/gc.hpp)), C++ exceptions carrying thrown JS
values, and array/string methods materialized as receiver-bound native
functions (`arr.push` is itself a value).

[`script.hpp`](include/ctjs/script.hpp) is the thin NTTP bridge:
`ctc::string` carries the source as a template argument - a structural
BYTE string, so the template parameter object IS the script text and
is viewed directly - `is_valid`/`script<>` prove syntax during
compilation, and `run<Src>()` executes through the same `run_value`
machinery as any runtime string.

(History: the original type-level path — a ctlark Earley grammar
producing the parse tree as a TYPE, lowered to a type-level AST with a
per-script-specialized interpreter — was removed in 2026-07 after the
value parser reproduced its behavior; the differential suite carried
the equivalence proof. The 10-minute grammar PCH bake died with it:
builds take seconds now. The ctlark/ctll submodule went with it in
2026-07 — ctc supplies the NTTP string.)

[compile-time-containers](https://github.com/alexios-angel/compile-time-containers)
(ctc) comes in as a git submodule (`external/compile-time-containers`
— clone with `--recurse-submodules` or run
`git submodule update --init`). Never edit under `external/`.

## Building and integrating

Header-only, C++23. Pick whichever fits your project:

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
(experimental). Constant evaluation of whole scripts wants a raised
step budget; the CMake interface (`CTJS_CONSTEXPR_LIMITS`) sets it:

```
clang: -fconstexpr-steps=500000000 -fconstexpr-depth=1024
```

**No build system:** add `include/` plus the submodule's
`external/compile-time-containers/include` to your include path, or
copy the amalgamated
[`single-header/ctjs.hpp`](single-header/ctjs.hpp)
(regenerate with `cmake --build build --target single-header`, which needs the
[quom](https://pypi.org/project/quom/) tool).

Run the tests — scripts parse during compilation, then the binaries
execute their checks:

```bash
git submodule update --init            # ctc (once, after cloning)
cmake --preset default                 # Ninja + Release (--preset clang for clang++)
cmake --build --preset default         # compiles + RUNS via ctest below (seconds)
ctest --preset default
```

Runnable demos live in [`examples/`](examples/): `hello` (the hero
demo) and `host` (native bindings + event handlers — the browser
seam).

## Roadmap

ctjs is a brick of a compile-time web stack, alongside
[compile-time-html](https://github.com/alexios-angel/compile-time-html)
and [compile-time-css](https://github.com/alexios-angel/compile-time-css);
they meet in
[compile-time-browser](https://github.com/alexios-angel/compile-time-browser)
— HTML, CSS and JS parsed at compile time and lowered into an SDL3
application, as if the page had been hand-written as native code.
ctjs's host-binding seam (`ctjs::binding`, `run_result::call`) is
exactly where the browser's DOM API plugs in.

## License

Apache License 2.0 with LLVM Exceptions (see [LICENSE](LICENSE)).
ctc (compile-time-containers) is MIT-licensed; historical CTLL/CTRE
lineage is recorded in [NOTICE](NOTICE).
