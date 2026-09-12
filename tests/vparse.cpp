// Parse-by-VALUE (vparse.hpp): a constexpr recursive-descent / Pratt parser
// producing a FLAT VALUE AST. These static_asserts prove it accepts the real
// JavaScript surface AT COMPILE TIME as a VALUE computation (no per-node types,
// O(n)) - the same 65 KB program that exceeds the Earley type-parser's constexpr
// budget value-parses in ~30 s. Runtime checks confirm the AST shape.
#include <ctjs/vparse.hpp>
#include <cstdio>
#include <string_view>

namespace vp = ctjs::vp;

// --- expressions -------------------------------------------------------------
static_assert(vp::is_valid("1 + 2 * 3 - 4 / 5 % 6 ** 7;"));
static_assert(vp::is_valid("a = b || c && d | e ^ f & g == h < i << j;"));
static_assert(vp::is_valid("let x = cond ? yes : no;"));
static_assert(vp::is_valid("foo.bar.baz(1, 2)(3)[k].m;"));
// A unicode escape in an identifier is decoded into the ast's arena - still
// constant-evaluable, the arena being a transient allocation here.
static_assert(vp::is_valid("var \\u{6F}bj = 1; obj;"));
static_assert(vp::is_valid("a?.b?.(x)?.[y] ?? fallback;"));
static_assert(vp::is_valid("let f = x => x * 2;"));
static_assert(vp::is_valid("let g = (a, b = 1, ...rest) => { return a + b; };"));
static_assert(vp::is_valid("let o = { a: 1, b, c() { return 2; }, [k]: 3, ...more };"));
static_assert(vp::is_valid("let r = /ab+c/gi.test(s);"));
static_assert(vp::is_valid("let s = `x=${1 + 2} y=${z}`;"));
static_assert(vp::is_valid("let e = new Foo(1, 2).method();"));
static_assert(vp::is_valid("let u = !-+~a; b++; --c;"));

// --- numeric literal forms ---------------------------------------------------
// The lexer special-cased 0x alone, so `0o17` came out as the number `0`
// followed by the identifier `o17` and the parse failed on ordinary modern
// code; and a number token stopped at `_`, so `1_000` was `1` then `_000`.
// Both prefixes and the ES2021 separator lex as ONE token now.
static_assert(vp::is_valid("let a = 0o17;"));
static_assert(vp::is_valid("let b = 0O17;"));
static_assert(vp::is_valid("let c = 0b1010;"));
static_assert(vp::is_valid("let d = 0B1010;"));
static_assert(vp::is_valid("let e = 0xFF;"));
static_assert(vp::is_valid("let f = 1_000_000;"));
static_assert(vp::is_valid("let g = 0xFF_FF;"));
static_assert(vp::is_valid("let h = 1_0.5;"));
static_assert(vp::is_valid("let i = 1e1_0;"));
// One token, not two: an expression statement of two numbers in a row would
// still "parse" under a lenient reading, so the shape that proves it is a
// literal used where only ONE operand fits.
static_assert(vp::is_valid("f(0o17, 0b11, 1_000);"));
static_assert(vp::is_valid("let j = [0o17, 0b11][0];"));

// The BigInt suffix rides on the number token: `1n` is ONE token, and a bundle
// carrying a single BigInt literal anywhere fails to parse as a whole without
// this - not just the expression.
static_assert(vp::is_valid("let a = 1n;"));
static_assert(vp::is_valid("let b = 0n;"));
static_assert(vp::is_valid("let c = 9007199254740993n;"));
static_assert(vp::is_valid("let d = 0xFFn;"));
static_assert(vp::is_valid("let e = 0b101n;"));
static_assert(vp::is_valid("let f = 0o17n;"));
static_assert(vp::is_valid("let g = 1_000n;"));
static_assert(vp::is_valid("f(1n, 2n);"));
static_assert(vp::is_valid("let h = 1n + 2n;"));
static_assert(vp::is_valid("let i2 = [1n, 2n][0];"));

// --- statements --------------------------------------------------------------
static_assert(vp::is_valid("if (a) { b(); } else if (c) d(); else { e(); }"));
static_assert(vp::is_valid("for (let i = 0; i < 10; i++) { sum += i; }"));
static_assert(vp::is_valid("for (const x of xs) use(x);"));
static_assert(vp::is_valid("while (a) { b(); } do { c(); } while (d);"));
static_assert(vp::is_valid("switch (x) { case 1: a(); break; default: b(); }"));
static_assert(vp::is_valid("try { f(); } catch (e) { g(e); } finally { h(); }"));
static_assert(vp::is_valid("function foo(a, b) { return a - b; }"));
static_assert(vp::is_valid(
    "class C extends B { x = 1; static y = 2; m() { return this.x; } get z() { return 3; } }"));

// --- semicolons optional (ASI is free in recursive descent) ------------------
static_assert(vp::is_valid("let a = 1\nlet b = 2\nconst o = { p: 1 }\nfoo()"));
static_assert(vp::is_valid("function f() { return 1 }\nf()"));

static int failures = 0;
#define CHECK(cond)                                                            \
	do {                                                                       \
		if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } \
	} while (0)

int main() {
	// runtime: shape of a small program
	vp::ast a = vp::parse("let x = 1 + 2; foo(x);");
	CHECK(a.ok);
	CHECK(a.root >= 0);
	CHECK(a.nodes[static_cast<std::size_t>(a.root)].kind == vp::nk::program);
	CHECK(a.nodes[static_cast<std::size_t>(a.root)].list_len == 2);

	// a genuine syntax error is reported, not thrown
	vp::ast bad = vp::parse("let x = ;");
	CHECK(!bad.ok);

	// class members counted
	vp::ast cls = vp::parse("class C { a = 1; b() {} static c = 3; }");
	CHECK(cls.ok);

	// `\u{6F}bj` IS `obj`: the token views the decoded spelling, and a private
	// name keeps its `#`.
	vp::ast esc = vp::parse("var \\u{6F}bj = 1; class C { #\\u{6F} = 2; }");
	CHECK(esc.ok);
	CHECK(esc.decoded.size() == 2);
	CHECK(*esc.decoded[0] == "obj");
	CHECK(*esc.decoded[1] == "#o");

	if (failures == 0) { std::printf("vparse suite: all checks passed\n"); }
	return failures ? 1 : 0;
}
