// The compile-time suite: syntax acceptance is a static property, so
// these are static_asserts - if this file compiles, the grammar layer
// holds. (Runtime behavior lives in runtime.cpp.)
#include <ctjs.hpp>
#include <cstdio>
#include <string_view>

using namespace std::literals;

// --- what parses
static_assert(ctjs::is_valid<"let x = 1;">);
static_assert(ctjs::is_valid<"const a = 1, b = [2, 3];">);
static_assert(ctjs::is_valid<"var old = 'school';">);
static_assert(ctjs::is_valid<"let letter = 1; let fortune = letter;">); // keyword prefixes
static_assert(ctjs::is_valid<"x = 1 + 2 * 3 ** 2 - -4 / (5 % 2);">);
static_assert(ctjs::is_valid<"ok = a < b && c !== d || !e == f;">);
static_assert(ctjs::is_valid<"pick = a ?? b ? c : d;">);
static_assert(ctjs::is_valid<"n = 0x1f + .5 + 1e3 + 2.5e-2;">);
static_assert(ctjs::is_valid<"x += 1; y.z -= 2; a[0] *= 3; p **= 2;">);
static_assert(ctjs::is_valid<"i++; --j; a.b++; x = i++ + --j;">);
static_assert(ctjs::is_valid<"console.log(f(x)[0].y, g());">);
static_assert(ctjs::is_valid<R"(let o = {a: 1, "b": [2, 3], 'c': {}};)">);
static_assert(ctjs::is_valid<"function add(a, b) { return a + b; }">);
static_assert(ctjs::is_valid<"let f = function(a) { return a; };">);
static_assert(ctjs::is_valid<"let g = (a, b) => a + b; let h = x => { return x; };">);
static_assert(ctjs::is_valid<"let k = () => 0;">);
static_assert(ctjs::is_valid<"if (a) { b(); } else if (c) d(); else { e(); }">);
static_assert(ctjs::is_valid<"while (a) { b(); } do { c(); } while (d);">);
static_assert(ctjs::is_valid<"for (let i = 0; i < 10; i++) { f(i); }">);
static_assert(ctjs::is_valid<"for (;;) { break; }">);
static_assert(ctjs::is_valid<"for (const x of arr) { f(x); }">);
static_assert(ctjs::is_valid<"try { f(); } catch (e) { g(e); } finally { h(); }">);
static_assert(ctjs::is_valid<"throw { name: 'E', message: 'm' };">);
static_assert(ctjs::is_valid<"t = typeof x === 'number';">);
static_assert(ctjs::is_valid<"// line\nlet x = 1; /* block\nmulti */ let y = 2;">);
static_assert(ctjs::is_valid<"q = a / b / c;">); // division, not a comment
static_assert(ctjs::is_valid<"">);

// --- what does not
static_assert(!ctjs::is_valid<"let x = 1">);        // no ASI: semicolon required
static_assert(!ctjs::is_valid<"f() = 1;">);         // not an assignment target
static_assert(!ctjs::is_valid<"let x = ;">);
static_assert(!ctjs::is_valid<"if a { b(); }">);    // parens required
static_assert(!ctjs::is_valid<"function () {}">);   // declarations need a name
static_assert(!ctjs::is_valid<"let x = 'unterminated;">);
static_assert(!ctjs::is_valid<"class C {}">);       // not in v0.1
static_assert(!ctjs::is_valid<"x = `template`;">);  // not in v0.1

// --- the script surface
static_assert(ctjs::script<"let x = 1;">.valid);
static_assert(!ctjs::script_t<ctll::fixed_string{"let x = 1"}>::valid);

// --- diagnostics: location and expected tokens, at compile time
static_assert(ctjs::error_info<"let x = 1;">().ok());
static_assert(ctjs::error_message<"let x = 1;">() == ""sv);
constexpr auto missing_semi = ctjs::error_info<"let x = 1">();
static_assert(missing_semi.kind != ctlark::error_kind::none);
static_assert(missing_semi.position == 9);
static_assert(!ctjs::error_message<"let x = 1">().empty());
static_assert(!ctjs::debug::dump_tokens<"let x = 1;">().empty());
static_assert(ctjs::debug::dump_grammar().find("terminal NAME") != std::string_view::npos);

int main() {
	std::printf("parse suite: all static_asserts held\n");
	return 0;
}
