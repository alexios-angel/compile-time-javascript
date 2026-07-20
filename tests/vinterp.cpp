// Value tree-walking interpreter (vinterp.hpp): runs the flat value AST from
// vparse.hpp at RUNTIME, reusing ctjs's value/environment/context/builtins.
// One piece of code walks any program - no per-script template instantiation -
// so compiling a script costs only a value-parse. These checks drive real
// programs and compare console output.
#include <ctjs/vinterp.hpp>
#include <cstdio>
#include <string>

namespace vp = ctjs::vp;

static int failures = 0;
static void run(const char * label, const char * src, const char * want) {
	vp::vrun_result r = vp::vrun(src);
	if (!r.ok) { std::printf("THREW %s: %s\n", label, r.error.to_string().c_str()); ++failures; return; }
	std::string got{r.console()};
	if (got != want) { std::printf("FAIL %s: got[%s] want[%s]\n", label, got.c_str(), want); ++failures; }
}

int main() {
	run("arith", "console.log(1 + 2 * 3 - 4 / 2)", "5\n");
	run("vars", "let a = 10; a += 5; a++; console.log(a)", "16\n");
	run("func", "function add(x, y) { return x + y; } console.log(add(3, 4))", "7\n");
	run("closure",
	    "function mk(n) { return function (x) { return x + n; }; } let f = mk(10); console.log(f(5))", "15\n");
	run("arrow", "let sq = x => x * x; console.log(sq(6))", "36\n");
	run("recursion",
	    "function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); } console.log(fib(10))", "55\n");
	run("for", "let s = 0; for (let i = 1; i <= 5; i++) { s += i; } console.log(s)", "15\n");
	run("while", "let n = 1, c = 0; while (n < 100) { n *= 2; c++; } console.log(n, c)", "128 7\n");
	run("array", "let a = [1, 2, 3]; a.push(4); console.log(a.length, a[3])", "4 4\n");
	run("array-methods", "console.log([1, 2, 3, 4].map(x => x * 2).filter(x => x > 4).join(','))", "6,8\n");
	run("object", "let o = { x: 1, y: 2, sum() { return this.x + this.y; } }; console.log(o.sum())", "3\n");
	run("for-of", "let t = 0; for (const x of [10, 20, 30]) { t += x; } console.log(t)", "60\n");
	run("class",
	    "class P { constructor(n) { this.n = n; } greet() { return 'hi ' + this.n; } } console.log(new P('bob').greet())",
	    "hi bob\n");
	run("inherit", "class A { who() { return 'A'; } } class B extends A {} console.log(new B().who())", "A\n");
	run("field", "class C { v = 42; get2() { return this.v; } } console.log(new C().get2())", "42\n");
	run("logical", "console.log(true && 'y', false || 'z', null ?? 'd')", "y z d\n");
	run("template", "let n = 'world'; console.log(`hello ${n} ${1 + 1}`)", "hello world 2\n");
	run("try", "try { throw 'boom'; } catch (e) { console.log('caught', e); }", "caught boom\n");
	run("switch",
	    "function f(x) { switch (x) { case 1: return 'one'; case 2: return 'two'; default: return '?'; } } console.log(f(2), f(9))",
	    "two ?\n");
	run("builtins", "console.log('abc'.toUpperCase(), 'Hello'.length, Math.max(3, 7, 2), Math.floor(3.9))",
	    "ABC 5 7 3\n");

	if (failures == 0) { std::printf("vinterp suite: all checks passed\n"); }
	return failures ? 1 : 0;
}
