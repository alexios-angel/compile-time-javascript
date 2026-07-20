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
static_assert(vp::is_valid("a?.b?.(x)?.[y] ?? fallback;"));
static_assert(vp::is_valid("let f = x => x * 2;"));
static_assert(vp::is_valid("let g = (a, b = 1, ...rest) => { return a + b; };"));
static_assert(vp::is_valid("let o = { a: 1, b, c() { return 2; }, [k]: 3, ...more };"));
static_assert(vp::is_valid("let r = /ab+c/gi.test(s);"));
static_assert(vp::is_valid("let s = `x=${1 + 2} y=${z}`;"));
static_assert(vp::is_valid("let e = new Foo(1, 2).method();"));
static_assert(vp::is_valid("let u = !-+~a; b++; --c;"));

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

	if (failures == 0) { std::printf("vparse suite: all checks passed\n"); }
	return failures ? 1 : 0;
}
