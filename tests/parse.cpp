// The parse suite: the CONSTEXPR value parser proves syntax properties
// during this file's compilation. Compiling this file IS the test (the
// binary just reports success).
#include <ctjs.hpp>
#include <cstdio>

// --- statements and declarations
static_assert(ctjs::is_valid<"let x = 1;">);
static_assert(ctjs::is_valid<"const y = [1, 2, 3];">);
static_assert(ctjs::is_valid<"var z;">);
static_assert(ctjs::is_valid<"function f(a, b = 1, ...rest) { return a; }">);
static_assert(ctjs::is_valid<"if (a) { b(); } else if (c) { d(); } else { e(); }">);
static_assert(ctjs::is_valid<"for (let i = 0; i < 10; i++) { work(i); }">);
static_assert(ctjs::is_valid<"for (const v of xs) { use(v); }">);
static_assert(ctjs::is_valid<"for (const k in o) { use(k); }">);
static_assert(ctjs::is_valid<"while (p()) { step(); }">);
static_assert(ctjs::is_valid<"do { step(); } while (p());">);
static_assert(ctjs::is_valid<"try { risky(); } catch (e) { handle(e); } finally { done(); }">);
static_assert(ctjs::is_valid<"switch (t) { case 1: a(); break; default: b(); }">);
static_assert(ctjs::is_valid<"outer: for (;;) { break outer; }">);
static_assert(ctjs::is_valid<"throw new Error('boom');">);

// --- expressions
static_assert(ctjs::is_valid<"a ?? b ?? c;">);
static_assert(ctjs::is_valid<"o?.p?.[i]?.(x);">);
static_assert(ctjs::is_valid<"f(...args, 1, ...more);">);
static_assert(ctjs::is_valid<"let o = { a, [k]: v, m() { return 1; }, ...rest };">);
static_assert(ctjs::is_valid<"let t = `a${1 + 2}b`;">);
static_assert(ctjs::is_valid<"x **= 2; y ||= 1; z &&= 2; w ?\?= 3;">);
static_assert(ctjs::is_valid<"let n = x instanceof C;">);

// --- classes
static_assert(ctjs::is_valid<
    "class B extends A { static n = 1; #p = 2; get v() { return 1; } set v(x) {} constructor() { super(); } m() { return super.m(); } }">);

// --- generator methods, in both places they can be written. The star used to
// be EATEN AND DISCARDED in a class body (so `*m() {}` parsed and then compiled
// as an ordinary method, whose `yield` had nowhere to go) and to break the
// parse outright in an object literal, where nothing expected it at all.
// Babylon.js has 162 generator methods.
static_assert(ctjs::is_valid<"class C { *m() { yield 1; } }">);
static_assert(ctjs::is_valid<"class C { static *m() { yield 1; } }">);
static_assert(ctjs::is_valid<"class C { async *m() { yield 1; } }">);
static_assert(ctjs::is_valid<"let o = { *m() { yield 1; } };">);
static_assert(ctjs::is_valid<"let o = { async *m() { yield 1; } };">);
static_assert(ctjs::is_valid<"let o = { *[k]() { yield 1; } };">);
// `yield*` is delegation, not a yield of the operand: the node says so.
static_assert(ctjs::is_valid<"function* g() { yield* other(); const r = yield* h; }">);
// `async` is still a usable property name, which is what the lookahead in the
// object-literal path is for - `{ async: 1 }` must not read as an async method.
static_assert(ctjs::is_valid<"let o = { async: 1 };">);
static_assert(ctjs::is_valid<"let o = { async() { return 1; } };">);

// --- ES modules. Every form docs/modules-plan.md lists, because "no shims"
// means a page is not rewritten to avoid one. ctbrowser refuses these by name
// at COMPILE time for now; parsing them is this parser's half.
static_assert(ctjs::is_valid<"import d from './m.js';">);
static_assert(ctjs::is_valid<"import { a, b as c } from './m.js';">);
static_assert(ctjs::is_valid<"import * as ns from './m.js';">);
static_assert(ctjs::is_valid<"import './side-effect.js';">);
static_assert(ctjs::is_valid<"import d, { a } from './m.js';">);
static_assert(ctjs::is_valid<"import d, * as ns from './m.js';">);
static_assert(ctjs::is_valid<"export const x = 1;">);
static_assert(ctjs::is_valid<"export function f() {}">);
static_assert(ctjs::is_valid<"export class C {}">);
static_assert(ctjs::is_valid<"const a = 1; export { a };">);
static_assert(ctjs::is_valid<"const a = 1; export { a as b };">);
static_assert(ctjs::is_valid<"export default 42;">);
static_assert(ctjs::is_valid<"export default function () {};">);
static_assert(ctjs::is_valid<"export * from './m.js';">);
static_assert(ctjs::is_valid<"export * as ns from './m.js';">);
static_assert(ctjs::is_valid<"export { x } from './m.js';">);
static_assert(ctjs::is_valid<"const u = import.meta.url;">);
static_assert(ctjs::is_valid<"const p = import('./m.js');">);
static_assert(ctjs::is_valid<"const p = await import('./m.js');">);
// POSTFIX AFTER BOTH EXPRESSION FORMS, which is how they are actually written.
// These failed while the handling sat in unary() instead of primary(): unary()
// returns before postfix() can apply a `.` or a call, so `import.meta` parsed
// and `import.meta.url` did not.
static_assert(ctjs::is_valid<"const u = import.meta.url;">);
static_assert(ctjs::is_valid<"import('./m.js').then(f);">);
static_assert(ctjs::is_valid<"const x = (await import('./m.js')).default;">);

// `from` and `as` ARE NOT KEYWORDS - they mean something only inside an import
// or export, and must stay usable as ordinary names. This is why the parser
// matches them by SPELLING rather than by token kind.
static_assert(ctjs::is_valid<"let from = 1; let as = 2; from = as;">);
static_assert(ctjs::is_valid<"o.from(); o.as;">);
// `import` and `export` are RESERVED, but a property may still be named either.
static_assert(ctjs::is_valid<"o.import; o.export; let q = { import: 1, export: 2 };">);

// --- contextual keywords stay usable as names (the old Earley grammar
// choked on these; the value parser is lenient by design)
static_assert(ctjs::is_valid<"let letter = of + async;">);
static_assert(ctjs::is_valid<"let let = 1;">);

// --- leniency contract (deliberate: keywords usable as names where
// unambiguous, semicolons recoverable - the parser mirrors what the
// interpreter can run, not a style guide)
static_assert(ctjs::is_valid<"let x = 1">);         // trailing semicolon recoverable
static_assert(ctjs::is_valid<"let = 4;">);          // assignment to the name `let`

// --- what is NOT valid (structural breaks)
static_assert(!ctjs::is_valid<"if (a { b(); }">);   // lost a paren
static_assert(!ctjs::is_valid<"let o = { a: };">);  // a property needs a value
static_assert(!ctjs::is_valid<"while (">);          // truncated
static_assert(!ctjs::is_valid<"f(1, 2">);           // unclosed call

// --- the NTTP layer rides the same constexpr parser
static_assert(ctjs::script<"let x = 1;">.valid);
static_assert(!ctjs::script_t<"let o = { a: };">::valid); // queryable, not an error

int main() {
	std::printf("parse suite: all checks passed\n");
	return 0;
}
