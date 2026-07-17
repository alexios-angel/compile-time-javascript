> **Attribution:** this library is built on the CTLL compile-time LL(1)
> parser from [CTRE](https://github.com/hanickadot/compile-time-regular-expressions)
> by Hana Dusíková, via the [notre](https://github.com/alexios-angel/notre)
> fork, and follows the architecture of its siblings
> [compile-time-xml](https://github.com/alexios-angel/compile-time-xml),
> [compile-time-json](https://github.com/alexios-angel/compile-time-json) and
> [compile-time-json5](https://github.com/alexios-angel/compile-time-json5).
> The named-character-reference table is the
> [WHATWG](https://html.spec.whatwg.org/multipage/named-characters.html)'s.
> Apache License 2.0 with LLVM Exceptions; see [NOTICE](NOTICE).

# ctjs — compile-time HTML

HTML5 parsed while your code compiles. The DOM is a *type*: broken
markup is a compile error, lookups are resolved at compile time, and
every accessor is `constexpr` — usable in `static_assert`, as template
arguments, or at runtime with zero parsing cost. Write HTML the way
HTML is written — void elements, `<li>` without `</li>`, unquoted
attributes — and get back a browser-shaped document:
`html > (head, body)`, always.

```c++
#include <ctjs.hpp>

constexpr auto page = ctjs::parse<R"(<!DOCTYPE html>
<title>demo &mdash; releases</title>
<ul id=nav>
    <li><a href=/docs>docs &amp; guides</a>
    <li><a href=/code>code</a>
</ul>)">();

static_assert(page.name() == "html");
static_assert(page.get<"head">().get<"title">().text() == "demo — releases");
static_assert(page.get<"body">().get<"ul">().count<"li">() == 2);
static_assert(page["body"]["ul"]["li"]["a"].attribute("href") == "/docs");

// author mistakes are a compile-time property:
static_assert(!ctjs::is_valid<"<b><i>crossed</b></i>">);  // crossing close tag
static_assert(!ctjs::is_valid<"<p x='1' x='2'></p>">);    // duplicate attribute
static_assert(!ctjs::is_valid<"<div/>">);                 // only voids self-close
```

## What is supported

HTML5 the way browsers read it, minus the repairs that hide bugs:

* **implied structure**: every parse yields `html > (head, body)` like
  a browser DOM — fragments land in body, metadata (`<meta>`,
  `<title>`, `<link>`, `<style>`, `<script>`, ...) written before any
  content collects into head, and explicit `<html>`/`<head>`/`<body>`
  tags contribute their attributes to the synthesized elements
* **void elements** (`<br>`, `<img>`, `<meta>`, ...) — no close tag,
  `<br/>` tolerated and identical
* **optional end tags**: the HTML5 auto-close table — `<li>` closes
  `<li>`, `<td>`/`<tr>` close each other, a block element closes
  `<p>`, `<option>`/`<optgroup>`, `<dt>`/`<dd>`, table sections,
  ruby annotations — and EOF closes everything (`<div>hi` is valid)
* **case-insensitive names**, stored canonically lowercase;
  `get<"DIV">()`, `["Div"]` and `attribute<"ID">()` all hit
* **attributes** double-quoted, single-quoted, unquoted (`width=100`)
  or bare boolean (`disabled`, reported as the empty string)
* **`<!DOCTYPE html>`** accepted and skipped, any case, legacy strings
  included
* **raw text**: `<script>`/`<style>` content is never parsed as markup
  (`if (a<b)`, `"</div>"` — fine); `<title>`/`<textarea>` are RCDATA
  (references decode); `<pre>`/`<textarea>` preserve whitespace, minus
  the single newline right after the open tag
* **character references, never an error**: the full WHATWG named
  table (2125 references, two-code-point ones included), decimal and
  hex numeric references with the spec's windows-1252 remap and
  `U+FFFD` fallbacks — all decoded to UTF-8 at parse time; unknown
  names and bare `&` stay literal
* `<!-- comments -->` (HTML rules: `--` inside is fine) and
  `<![CDATA[...]]>` sections are dropped; whitespace-only text between
  elements is dropped (except inside `<pre>`/`<textarea>`)

**Where ctjs is stricter than a browser** — the spec makes browsers
*repair* these; ctjs makes them compile errors, because markup you
compile in is markup you control:

* a stray end tag (`</p>` with no `<p>`, `</br>` at all)
* a close tag crossing a still-open element (`<b><i>x</b></i>`,
  `<div><b>x</div>`)
* a duplicate attribute name (case-insensitively)
* self-closing syntax on a non-void element (`<div/>`)
* a raw `<` in text (write `&lt;`), and a raw-text element that never
  reaches its close tag
* elements nested deeper than 256 levels

Not supported (yet): tag-omission rules that need more than the top of
the open stack (`<p>` is closed by a block element only when it is the
innermost open element), foreign content (SVG/MathML) semantics, and
encodings other than UTF-8/ASCII.

## API

```c++
// acceptability as a bool (never a compile error):
template <ctll::fixed_string input> constexpr bool ctjs::is_valid;

// the parsed document, always the html element; invalid HTML fails the build:
template <ctll::fixed_string input> constexpr auto ctjs::parse();
```

`parse` returns an `element`; its children are `element`s and `text`
nodes:

| Type | Accessors |
|------|-----------|
| `element<name, attrs, children...>` | `name()`, `attribute<"key">()`, `has_attribute<"key">()`, `attribute_count()`, positional `attribute_name<I>()` / `attribute_value<I>()`, `get<"tag">()` / `["tag"]` (first matching child element), `contains<"tag">()`, `count<"tag">()`, `child<I>()` / `[N]`, `child_count()`, `empty()`, `text()` |
| `text<chars...>` | `view()`, `c_str()` (null-terminated), `size()`, `empty()`, `==` with `std::string_view` |

Name lookups are case-insensitive everywhere. Every type carries
`static constexpr ctjs::kind type` for introspection
(`kind::element`, `kind::text`), and two free functions iterate at
compile time:

```c++
ctjs::for_each_child(doc, [](auto child) { /* each has its own type */ });
ctjs::for_each_attribute(doc, [](auto name, auto value) { ... });

// render any element back to minified HTML, in static storage:
static_assert(ctjs::serialize(ctjs::parse<"<ul id=nav><li>Docs</ul>">())
    == R"(<html><head></head><body><ul id="nav"><li>Docs</li></ul></body></html>)");
```

Brackets and iteration:

```c++
doc["body"]["ul"];           // first matching child, as a uniform node_view
doc[1];                      // child at position 1, as a uniform node_view
doc["body"].attribute("class");   // runtime names; attribute<"class">() stays typed

// begin/end yield uniform views (kind + name + text) from static storage,
// so range-for and algorithms work - in constexpr evaluation included:
for (const auto & n : doc) {
    n.type;   // ctjs::kind::element or kind::text
    n.name(); // elements: the tag; text nodes: empty
    n.text(); // elements: their direct text; text nodes: the content
}
for (const auto & a : ctjs::attributes(doc)) {
    a.name, a.value;   // std::string_views
}
```

Children have distinct types, so a runtime tag or index cannot return the
child itself. `operator[]` accepts an ordinary string or integer and returns
a uniform `node_view`; when you need the child itself, with its typed
accessors, use `get<...>()`, `child<I>()`, or `for_each_child`. The
records are `node_view` and `attribute_view`
([`views.hpp`](include/ctjs/views.hpp)), and
[`examples/iteration.cpp`](examples/iteration.cpp) is a runnable tour.

`serialize` renders HTML: text re-escapes `& < >` and attribute values
`& "`, void elements come out bare (`<br>`), boolean attributes come
out bare (`disabled`), other childless elements as `<div></div>`,
`<script>`/`<style>` bodies pass through raw, and the result is
null-terminated.

## Debugging

When `is_valid` says `false`, the reason is one query away, computed at
compile time. Syntax failures carry the location and the expected
tokens:

```c++
constexpr auto info = ctjs::error_info<"<p class=x">();
// info.kind (lex/parse/...), info.position, info.line, info.column

constexpr auto why = ctjs::error_message<"<p class=x">();
// the rendered diagnostic: location, snippet with a caret, expected terminals
```

Documents that PARSE can still be rejected by tree construction; the
error names the rule and the offending token:

```c++
ctjs::bind_error<"<b><i>x</b></i>">();  // mismatched_tag, where == "</b>"
ctjs::bind_error<"<p>x</p></p>">();     // stray_end_tag, where == "</p>"
ctjs::bind_error<"<p a=1 a=2></p>">();  // duplicate_attribute, where == "a"
ctjs::bind_error<"<div/>">();           // self_closing_non_void, where == "<div"
```

A failed `parse<>()` names the failing stage and the query to run in
its `static_assert` message. `ctjs::debug` bundles the [ctlark
debugging toolbox](../compile-time-lark#debugging) with the HTML
grammar baked in: `traced_parse<input>()` (a recorded event log, also
runnable at runtime under a debugger), `parse_runtime(text)` (runtime
inputs against the compile-time tables), `dump_tokens<input>()` and
`dump_grammar()`.

## C++17

With a pre-C++20 compiler, inputs and keys are `constexpr
ctll::fixed_string` variables with linkage instead of string literals:

```c++
static constexpr auto text = ctll::fixed_string{"<ul id=nav><li>Docs</ul>"};
static constexpr ctll::fixed_string id_key = "id";

constexpr auto doc = ctjs::parse<text>();
static_assert(doc["body"]["ul"].attribute("id") == std::string_view{"nav"});
```

## How it works

The grammar layer is
[ctlark](https://github.com/alexios-angel/compile-time-lark)
(compile-time Lark) — but unlike the XML/JSON/YAML siblings, the
grammar does not nest elements at all, because HTML tag nesting is not
context-free (end tags may be omitted, `<html>/<head>/<body>` are
implied). Instead, the *lark grammar string*
([`grammar.hpp`](include/ctjs/grammar.hpp)) lexes the document into a
FLAT chunk stream — open tags, close tags, text, and whole raw-text
elements, whose `*_BODY` terminals swallow `<script>`/`<style>` content
up to the first real close tag by riding ctlark's **contextual** lexer
(they are the only candidate tokens right after the open tag's `>`).

The binder ([`bind.hpp`](include/ctjs/bind.hpp)) lowers chunks into
building blocks — names folded to lowercase, the three attribute value
flavours plus booleans, character references decoded through the
generated WHATWG table ([`entities.hpp`](include/ctjs/entities.hpp),
regenerate with `tools/gen-entities.py`). Then the tree-construction
layer ([`treebuild.hpp`](include/ctjs/treebuild.hpp)) does what a
browser's tree builder does, at compile time, twice: a cheap
value-level validator walks a name stack and reports the first author
mistake (this is all `is_valid` costs), and a type-level fold — the
open elements as a stack of frames — applies the auto-close table,
attaches void and raw-text elements, merges adjacent text, synthesizes
`html > (head, body)`, and closes everything at EOF, producing the one
document type.

Because that work happens in headers, a **precompiled header** makes
it a one-time cost: `make pch` (done automatically by the test build)
compiles `ctjs.hpp` once - grammar parse, table build and all - and
every translation unit that includes it afterwards starts from the
baked result. The CMake tests and examples use
`target_precompile_headers` the same way (`CTJS_PCH`, default ON).

An Earley parse needs a raised constexpr budget; the CMake interface
target carries the compiler-specific limit flags automatically
(`CTJS_CONSTEXPR_LIMITS`, default ON) and the Makefiles set them:

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

Header-only. Pick whichever fits your project:

**CMake, as a subdirectory or via FetchContent:**

```cmake
add_subdirectory(compile-time-html)   # or FetchContent_MakeAvailable(ctjs)
target_link_libraries(your-target PRIVATE ctjs::ctjs)
```

**CMake, installed** (`cmake -B build && cmake --install build`):

```cmake
find_package(ctjs 0.1 REQUIRED)
target_link_libraries(your-target PRIVATE ctjs::ctjs)
```

The install also ships a `pkg-config` file (`ctjs.pc`). Tests and
examples build only when ctjs is the top-level project
(`CTJS_BUILD_TESTS`, `CTJS_BUILD_EXAMPLES`); `CTJS_CXX_STANDARD`
selects the advertised standard (default 20). CPack can produce
TGZ/ZIP archives (plus DEB/RPM where the tooling exists), and
`-DCTJS_MODULE=ON` builds `ctjs.cppm` as a named C++ module
(experimental; needs CMake 3.30+, a modules-capable toolchain and
`import std`).

**No build system:** add `include/` plus the submodule's
`external/compile-time-lark/include` (and its `ctlark`/`ctll`
subdirectories) to your include path, or copy the amalgamated
[`single-header/ctjs.hpp`](single-header/ctjs.hpp)
(regenerate with `make single-header`, which needs the
[quom](https://pypi.org/project/quom/) tool).

Requires C++17 (C++20 for the string-literal API). Runnable demos live
in [`examples/`](examples/).

Run the tests (compilation is the test — the suite is `static_assert`s):

```bash
git submodule update --init            # ctlark + ctll (once, after cloning)
make CXX=clang++                       # C++20
make CXX=clang++ CXX_STANDARD=17
# or through CMake/CTest:
cmake -B build && cmake --build build && ctest --test-dir build
```

## Roadmap

ctjs is the first brick of a compile-time web stack:
**compile-time-javascript** and **compile-time-css** come next, and
they meet in **compile-time-browser** — HTML, CSS and JS parsed at
compile time and lowered into an SDL3 application, as if the page had
been hand-written as native code.

## License

Apache License 2.0 with LLVM Exceptions (see [LICENSE](LICENSE)).
The CTLL parser is Hana Dusíková's work, via notre; the named
character references are the WHATWG's; see [NOTICE](NOTICE).
