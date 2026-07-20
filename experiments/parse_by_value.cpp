// ============================================================================
// parse_by_value.cpp - PROOF OF CONCEPT: parsing "by value" for fast, scalable
// compile-time parsing. (Not yet integrated; this validates the approach.)
//
// WHY. ctjs (via ctlark) parses "by type": the Earley chart and parse tree are
// encoded in the C++ TYPE system (template instantiation), and the interpreter
// is specialised on the AST type. Two compile-time costs stack up:
//   1. Type-instantiation overhead - a template type per node (the slowest kind
//      of compile-time work: allocate/mangle/dedupe a type each).
//   2. Earley's ALGORITHMIC complexity - O(n^2) unambiguous, O(n^3) worst case.
// Measured on the std::embed clang (8-core server), the Earley parse of a real
// 65 KB script - computed even as a constexpr VALUE (is_valid's `result`) -
// exceeded 500M constexpr steps (>8 min, then failed); a 66 KB HTML document
// did the same (9 min, 495 MB). So for large inputs the wall is the SUPERLINEAR
// ALGORITHM, not only the type encoding.
//
// PARSING BY VALUE fixes both: a hand-written recursive-descent / Pratt parser
// is O(n) and emits a FLAT VALUE AST (an array of nodes) with ZERO per-node
// types; a value tree-walk interprets it. Constexpr value computation is the
// compiler's fast path (see: this session's ASI pass - a constexpr value scan
// over the same 65 KB - is effectively free).
//
// MEASURED here (this prototype, plain clang, -O0), parsing+evaluating an
// N-term expression AT COMPILE TIME:
//     4 KB  -> 0.44 s / 111 MB       80 KB  -> 4.04 s / 179 MB
//    20 KB  -> 1.14 s / 125 MB      200 KB  -> 10.5 s / 286 MB
// i.e. LINEAR, and 200 KB parses in ~10 s where Earley cannot finish 65 KB.
//
// FULL BUILD-OUT (the real work, not done here): a constexpr recursive-descent
// parser for the entire ctjs grammar (statements/classes/async/closures/
// destructuring/templates/regex) -> flat value AST, plus a value tree-walking
// interpreter to replace the type-driven one. The same argument applies to
// cthtml, which the per-module split showed is the next Earley bottleneck.
//
// Build:  clang++ -std=c++20 -O0 -fconstexpr-steps=2000000000 \
//                 -fconstexpr-depth=4000 -DN=100000 parse_by_value.cpp -o pbv
// ============================================================================
//
// A constexpr recursive-descent (Pratt) parser that produces a FLAT VALUE AST
// (an array of nodes), evaluated by a value tree-walk. No template instantiation
// per node, and O(n) instead of Earley's O(n^2+).
//
// Grammar (a JS-expression subset, enough to measure scaling):
//   expr := term (('+'|'-') term)*
//   term := factor (('*'|'/') factor)*
//   factor := NUMBER | '(' expr ')'
//
// Build with -DN=<count> to parse an expression of N '+'-separated 1's and
// static_assert the sum; scaling N shows the parser is linear and cheap.

#include <array>
#include <string_view>
#include <utility>
#include <cstdio>

namespace pv {

// ---- flat value AST ---------------------------------------------------------
enum : char { NUM = 'n', ADD = '+', SUB = '-', MUL = '*', DIV = '/' };

struct node {
	char op = NUM;
	double num = 0;   // for NUM
	int lhs = -1, rhs = -1;
};

template <std::size_t CAP>
struct ast {
	std::array<node, CAP> nodes{};
	int count = 0;
	int root = -1;
	constexpr int emit(node n) {
		nodes[static_cast<std::size_t>(count)] = n;
		return count++;
	}
};

// ---- the parser: pure constexpr VALUE computation, single left-to-right pass -
template <std::size_t CAP>
struct parser {
	std::string_view s;
	std::size_t i = 0;
	ast<CAP> * a;

	constexpr void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) { ++i; } }
	constexpr char peek() { ws(); return i < s.size() ? s[i] : '\0'; }

	constexpr int number() {
		ws();
		double v = 0;
		bool any = false;
		while (i < s.size() && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); ++i; any = true; }
		if (i < s.size() && s[i] == '.') {
			++i; double f = 0.1;
			while (i < s.size() && s[i] >= '0' && s[i] <= '9') { v += (s[i] - '0') * f; f *= 0.1; ++i; any = true; }
		}
		(void)any;
		return a->emit(node{NUM, v, -1, -1});
	}

	constexpr int factor() {
		if (peek() == '(') { ++i; int e = expr(); ws(); if (peek() == ')') { ++i; } return e; }
		return number();
	}
	constexpr int term() {
		int lhs = factor();
		for (;;) {
			char c = peek();
			if (c == '*' || c == '/') { ++i; int rhs = factor(); lhs = a->emit(node{c, 0, lhs, rhs}); }
			else { break; }
		}
		return lhs;
	}
	constexpr int expr() {
		int lhs = term();
		for (;;) {
			char c = peek();
			if (c == '+' || c == '-') { ++i; int rhs = term(); lhs = a->emit(node{c, 0, lhs, rhs}); }
			else { break; }
		}
		return lhs;
	}
};

template <std::size_t CAP>
constexpr ast<CAP> parse(std::string_view src) {
	ast<CAP> a;
	parser<CAP> p{src, 0, &a};
	a.root = p.expr();
	return a;
}

// Iterative eval: children are always emitted before their parent, so a single
// forward pass over the flat array evaluates the whole tree (depth-independent).
template <std::size_t CAP>
constexpr double eval(const ast<CAP> & a, int root) {
	std::array<double, CAP> val{};
	for (int i = 0; i < a.count; ++i) {
		const node & n = a.nodes[static_cast<std::size_t>(i)];
		double v = 0;
		switch (n.op) {
		case NUM: v = n.num; break;
		case ADD: v = val[static_cast<std::size_t>(n.lhs)] + val[static_cast<std::size_t>(n.rhs)]; break;
		case SUB: v = val[static_cast<std::size_t>(n.lhs)] - val[static_cast<std::size_t>(n.rhs)]; break;
		case MUL: v = val[static_cast<std::size_t>(n.lhs)] * val[static_cast<std::size_t>(n.rhs)]; break;
		case DIV: v = val[static_cast<std::size_t>(n.lhs)] / val[static_cast<std::size_t>(n.rhs)]; break;
		}
		val[static_cast<std::size_t>(i)] = v;
	}
	return val[static_cast<std::size_t>(root)];
}

// ---- build an N-term input "1+1+...+1" at compile time ----------------------
template <std::size_t Cnt>
constexpr auto make_input() {
	std::array<char, Cnt * 2> buf{};
	std::size_t k = 0;
	for (std::size_t t = 0; t < Cnt; ++t) {
		if (t) { buf[k++] = '+'; }
		buf[k++] = '1';
	}
	return std::pair<std::array<char, Cnt * 2>, std::size_t>{buf, k};
}

} // namespace pv

#ifndef N
#define N 2000
#endif

// storage for the generated input, and the parsed AST - ALL at compile time
constexpr auto INPUT = pv::make_input<N>();
constexpr std::string_view SRC{INPUT.first.data(), INPUT.second};
// capacity: N numbers + (N-1) operators
constexpr auto TREE = pv::parse<2 * N>(SRC);
constexpr double RESULT = pv::eval(TREE, TREE.root);

static_assert(RESULT == double(N), "sum of N ones is N - parsed & evaluated by value at compile time");

int main() {
	std::printf("N=%d nodes=%d result=%.1f (parsed by value, O(n), at compile time)\n",
	            N, TREE.count, RESULT);
	return 0;
}
