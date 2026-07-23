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
