#ifndef CTJS__FOLD__HPP
#define CTJS__FOLD__HPP

#include "ast.hpp"
#ifndef CTJS_IN_A_MODULE
#include <cstddef>
#include <string_view>
#include <type_traits>
#endif

// Compile-time constant folding / partial evaluation. Lowering runs
// this bottom-up over each expression (lower.hpp), so any subtree that
// is a compile-time constant collapses to a single node before the
// interpreter ever sees it: `2 + 3 * 4` becomes `const_num<14>`,
// `true ? a : b` becomes `a`, `false && sideEffect()` becomes `false`.
// The runtime then only does the genuinely dynamic work.
//
// SOUNDNESS: a fold must produce EXACTLY what the runtime interpreter
// would. We therefore fold only values we can reproduce bit-for-bit -
// INTEGER numeric literals (decimal or hex), booleans and null - never
// fractional/exponent literals (whose runtime `from_chars` value we
// will not second-guess) and never strings/objects. Operators are the
// IEEE-deterministic ones; `**` folds only for a non-negative integer
// exponent via exact integer multiplication (no std::pow).

namespace ctjs::detail {

using namespace ctjs::ast;

struct folded {
	enum kind_t { kNo, kNum, kBool, kNul } kind = kNo;
	double num = 0;
	constexpr bool ok() const { return kind != kNo; }
};
constexpr folded fold_none() { return {}; }
constexpr folded fold_num(double v) { return {folded::kNum, v}; }
constexpr folded fold_bool(bool b) { return {folded::kBool, b ? 1.0 : 0.0}; }
constexpr folded fold_nul() { return {folded::kNul, 0.0}; }

// integer literal (decimal or 0x hex) -> exact double; anything with a
// '.', 'e'/'E' or a stray char is left unfolded (kind == no)
constexpr folded parse_int_literal(std::string_view s) {
	if (s.empty()) { return fold_none(); }
	if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		unsigned long long u = 0;
		for (size_t i = 2; i < s.size(); ++i) {
			const char c = s[i];
			unsigned d = 0;
			if (c >= '0' && c <= '9') { d = static_cast<unsigned>(c - '0'); }
			else if (c >= 'a' && c <= 'f') { d = static_cast<unsigned>(c - 'a' + 10); }
			else if (c >= 'A' && c <= 'F') { d = static_cast<unsigned>(c - 'A' + 10); }
			else { return fold_none(); }
			u = u * 16 + d;
		}
		return fold_num(static_cast<double>(u));
	}
	unsigned long long u = 0;
	for (const char c : s) {
		if (c < '0' || c > '9') { return fold_none(); } // '.'/'e' => not folded
		u = u * 10 + static_cast<unsigned>(c - '0');
	}
	return fold_num(static_cast<double>(u));
}

constexpr bool fold_truthy(const folded & f) {
	if (f.kind == folded::kNum) { return f.num != 0.0 && !(f.num != f.num); }
	if (f.kind == folded::kBool) { return f.num != 0.0; }
	return false; // null is falsy
}
constexpr double fold_numify(const folded & f) {
	return f.kind == folded::kNul ? 0.0 : f.num; // null -> 0, bool already 0/1
}

// the fold value of ONE node. Children are folded already (bottom-up),
// so a constant child is a leaf here - this stays O(1) per node.
template <typename A> constexpr folded fold_val(A) { return fold_none(); }
template <double V> constexpr folded fold_val(const_num<V>) { return fold_num(V); }
template <typename T> constexpr folded fold_val(num_lit<T>) {
	return parse_int_literal(T::view());
}
constexpr folded fold_val(true_lit) { return fold_bool(true); }
constexpr folded fold_val(false_lit) { return fold_bool(false); }
constexpr folded fold_val(null_lit) { return fold_nul(); }

template <typename Op, typename E> constexpr folded fold_val(unary<Op, E>) {
	const folded c = fold_val(E{});
	if (!c.ok()) { return fold_none(); }
	if constexpr (std::is_same_v<Op, op_neg>) { return fold_num(-fold_numify(c)); }
	else if constexpr (std::is_same_v<Op, op_pos>) { return fold_num(fold_numify(c)); }
	else if constexpr (std::is_same_v<Op, op_not>) { return fold_bool(!fold_truthy(c)); }
	else { return fold_none(); } // typeof/await: not folded
}

constexpr double fold_ipow(double base, double exp) {
	// exact only for a non-negative integer exponent
	long long e = static_cast<long long>(exp);
	double r = 1.0;
	for (long long i = 0; i < e; ++i) { r *= base; }
	return r;
}
constexpr bool is_int(double d) { return d == static_cast<double>(static_cast<long long>(d)); }

template <typename Op, typename L, typename R> constexpr folded fold_val(binary<Op, L, R>) {
	const folded l = fold_val(L{});
	// logical / nullish short-circuit: a constant LEFT may decide the
	// result even when the right side is not itself constant - but here
	// we only return a CONSTANT (the subtree-simplification cases live
	// in `simplify` below).
	if constexpr (std::is_same_v<Op, op_and>) {
		if (l.ok() && !fold_truthy(l)) { return l; }
		const folded r = fold_val(R{});
		if (l.ok() && r.ok()) { return r; }
		return fold_none();
	} else if constexpr (std::is_same_v<Op, op_or>) {
		if (l.ok() && fold_truthy(l)) { return l; }
		const folded r = fold_val(R{});
		if (l.ok() && r.ok()) { return r; }
		return fold_none();
	} else if constexpr (std::is_same_v<Op, op_nullish>) {
		if (l.ok() && l.kind != folded::kNul) { return l; }
		const folded r = fold_val(R{});
		if (l.ok() && r.ok()) { return r; }
		return fold_none();
	} else {
		const folded r = fold_val(R{});
		if (!l.ok() || !r.ok()) { return fold_none(); }
		const double a = fold_numify(l), b = fold_numify(r);
		if constexpr (std::is_same_v<Op, op_add>) { return fold_num(a + b); }
		else if constexpr (std::is_same_v<Op, op_sub>) { return fold_num(a - b); }
		else if constexpr (std::is_same_v<Op, op_mul>) { return fold_num(a * b); }
		else if constexpr (std::is_same_v<Op, op_div>) { return fold_num(a / b); }
		else if constexpr (std::is_same_v<Op, op_mod>) {
			return b == 0.0 ? fold_num(a - a) : fold_num(a - b * static_cast<double>(
			                                                        static_cast<long long>(a / b)));
		} else if constexpr (std::is_same_v<Op, op_pow>) {
			if (is_int(b) && b >= 0) { return fold_num(fold_ipow(a, b)); }
			return fold_none();
		} else if constexpr (std::is_same_v<Op, op_lt>) { return fold_bool(a < b); }
		else if constexpr (std::is_same_v<Op, op_gt>) { return fold_bool(a > b); }
		else if constexpr (std::is_same_v<Op, op_le>) { return fold_bool(a <= b); }
		else if constexpr (std::is_same_v<Op, op_ge>) { return fold_bool(a >= b); }
		else if constexpr (std::is_same_v<Op, op_seq> || std::is_same_v<Op, op_sne>) {
			// strict: equal only if same kind (num/bool/null) and equal
			bool eq = l.kind == r.kind &&
			          (l.kind == folded::kNul || l.num == r.num);
			return fold_bool(std::is_same_v<Op, op_seq> ? eq : !eq);
		} else if constexpr (std::is_same_v<Op, op_eq> || std::is_same_v<Op, op_ne>) {
			// loose: null equals only null; otherwise numeric compare
			bool eq;
			if (l.kind == folded::kNul || r.kind == folded::kNul) {
				eq = l.kind == folded::kNul && r.kind == folded::kNul;
			} else {
				eq = a == b;
			}
			return fold_bool(std::is_same_v<Op, op_eq> ? eq : !eq);
		} else {
			return fold_none();
		}
	}
}

template <typename C, typename T, typename F> constexpr folded fold_val(ternary<C, T, F>) {
	const folded c = fold_val(C{});
	if (!c.ok()) { return fold_none(); }
	const folded chosen = fold_truthy(c) ? fold_val(T{}) : fold_val(F{});
	return chosen; // ok() iff the taken branch is itself constant
}
template <typename L, typename R> constexpr folded fold_val(comma_op<L, R>) {
	// a, b == b when `a` has no observable effect (a constant does not)
	if (!fold_val(L{}).ok()) { return fold_none(); }
	return fold_val(R{});
}

// subtree simplification: reduce to a (already-folded) child even when
// the whole expression is not itself constant. Only fires when fold_val
// did NOT already produce a constant.
template <typename N> struct simplify {
	using type = N;
};
template <typename C, typename T, typename F> struct simplify<ternary<C, T, F>> {
	static constexpr auto go() {
		constexpr folded c = fold_val(C{});
		if constexpr (c.ok()) {
			if constexpr (fold_truthy(c)) { return T{}; }
			else { return F{}; }
		} else {
			return ternary<C, T, F>{};
		}
	}
	using type = decltype(go());
};
template <typename L, typename R> struct simplify<binary<op_and, L, R>> {
	static constexpr auto go() {
		constexpr folded l = fold_val(L{});
		if constexpr (l.ok()) { // truthy -> R, falsy -> L (already constant)
			if constexpr (fold_truthy(l)) { return R{}; }
			else { return L{}; }
		} else {
			return binary<op_and, L, R>{};
		}
	}
	using type = decltype(go());
};
template <typename L, typename R> struct simplify<binary<op_or, L, R>> {
	static constexpr auto go() {
		constexpr folded l = fold_val(L{});
		if constexpr (l.ok()) { // truthy -> L, falsy -> R
			if constexpr (fold_truthy(l)) { return L{}; }
			else { return R{}; }
		} else {
			return binary<op_or, L, R>{};
		}
	}
	using type = decltype(go());
};
template <typename L, typename R> struct simplify<binary<op_nullish, L, R>> {
	static constexpr auto go() {
		constexpr folded l = fold_val(L{});
		if constexpr (l.ok()) { return l.kind == folded::kNul ? R{} : L{}; }
		else { return binary<op_nullish, L, R>{}; }
	}
	using type = decltype(go());
};

// the lowering hook: fold a node to a constant, else simplify it
template <typename N> struct fold_node {
	static constexpr auto go() {
		constexpr folded f = fold_val(N{});
		if constexpr (f.kind == folded::kNum) {
			return const_num<f.num>{};
		} else if constexpr (f.kind == folded::kBool) {
			if constexpr (f.num != 0.0) { return true_lit{}; }
			else { return false_lit{}; }
		} else if constexpr (f.kind == folded::kNul) {
			return null_lit{};
		} else {
			return typename simplify<N>::type{};
		}
	}
	using type = decltype(go());
};
template <typename N> using fold_node_t = typename fold_node<N>::type;

// a whole program that is a single expression statement, folded: this
// is what `ctjs::is_constant`/`ctjs::constant` expose so a script's
// compile-time value is usable in a static_assert
template <typename Prog> struct program_constant {
	static constexpr folded value = fold_none();
};
template <typename E> struct program_constant<program<expr_stmt<E>>> {
	static constexpr folded value = fold_val(E{});
};

} // namespace ctjs::detail

#endif
