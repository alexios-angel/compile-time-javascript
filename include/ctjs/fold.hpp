#ifndef CTJS__FOLD__HPP
#define CTJS__FOLD__HPP

#include "ast.hpp"
#ifndef CTJS_IN_A_MODULE
#include <array>
#include <cstddef>
#include <string_view>
#include <type_traits>
#include <utility>
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

// --- string folding: pure string expressions computed at compile time.
// A string is carried byte-exact as ast::const_str<Cs...>. Only the
// pieces we reproduce EXACTLY are folded: string literals, folded
// strings, and - via JS string coercion - integer, boolean and null
// constants (never fractional numbers, whose Number::toString we will
// not second-guess). `+` is a string concat as soon as one side is a
// string, matching the interpreter.

// mirror interp's push_utf8, writing into a buffer
constexpr size_t fold_push_utf8(char * out, unsigned long cp) {
	if (cp < 0x80) { out[0] = static_cast<char>(cp); return 1; }
	if (cp < 0x800) {
		out[0] = static_cast<char>(0xC0 | (cp >> 6));
		out[1] = static_cast<char>(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = static_cast<char>(0xE0 | (cp >> 12));
		out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out[2] = static_cast<char>(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = static_cast<char>(0xF0 | (cp >> 18));
	out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
	out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
	out[3] = static_cast<char>(0x80 | (cp & 0x3F));
	return 4;
}
// cook a quoted string literal into `out`, byte-for-byte as interp's
// cook_string; returns the cooked length
constexpr size_t fold_cook(std::string_view raw, char * out) {
	size_t n = 0, i = 1;
	const size_t end = raw.size() - 1;
	while (i < end) {
		const char c = raw[i];
		if (c != '\\') { out[n++] = c; ++i; continue; }
		const char e = raw[i + 1];
		i += 2;
		switch (e) {
		case 'n': out[n++] = '\n'; break;
		case 't': out[n++] = '\t'; break;
		case 'r': out[n++] = '\r'; break;
		case 'b': out[n++] = '\b'; break;
		case 'f': out[n++] = '\f'; break;
		case 'v': out[n++] = '\v'; break;
		case '0': out[n++] = '\0'; break;
		case 'x': {
			unsigned long cp = 0;
			for (int k = 0; k < 2 && i < end; ++k, ++i) {
				cp = cp * 16 + static_cast<unsigned long>(
				                   raw[i] <= '9' ? raw[i] - '0' : (raw[i] | 0x20) - 'a' + 10);
			}
			n += fold_push_utf8(out + n, cp);
			break;
		}
		case 'u': {
			unsigned long cp = 0;
			for (int k = 0; k < 4 && i < end; ++k, ++i) {
				cp = cp * 16 + static_cast<unsigned long>(
				                   raw[i] <= '9' ? raw[i] - '0' : (raw[i] | 0x20) - 'a' + 10);
			}
			n += fold_push_utf8(out + n, cp);
			break;
		}
		case '\n': break;
		default: out[n++] = e;
		}
	}
	return n;
}
constexpr size_t fold_itoa(long long v, char * out) {
	size_t n = 0;
	unsigned long long u = v < 0 ? (out[n++] = '-', static_cast<unsigned long long>(-(v + 1)) + 1)
	                             : static_cast<unsigned long long>(v);
	char tmp[24] = {};
	size_t t = 0;
	if (u == 0) { tmp[t++] = '0'; }
	while (u) { tmp[t++] = static_cast<char>('0' + u % 10); u /= 10; }
	for (size_t k = 0; k < t; ++k) { out[n++] = tmp[t - 1 - k]; }
	return n;
}

// string-form of a coercible constant node: len() + write(out).
// `str` = it is itself a string; `coercible` = usable in string concat.
template <typename N> struct sform {
	static constexpr bool str = false;
	static constexpr bool coercible = false;
	static constexpr size_t len() { return 0; }
	static constexpr size_t write(char *) { return 0; }
};
template <typename T> struct sform<str_lit<T>> {
	static constexpr bool str = true, coercible = true;
	static constexpr size_t raw = T::view().size();
	static constexpr size_t len() {
		std::array<char, raw ? raw : 1> b{};
		return fold_cook(T::view(), b.data());
	}
	static constexpr size_t write(char * out) { return fold_cook(T::view(), out); }
};
template <char... Cs> struct sform<const_str<Cs...>> {
	static constexpr bool str = true, coercible = true;
	static constexpr size_t len() { return sizeof...(Cs); }
	static constexpr size_t write(char * out) {
		size_t n = 0;
		((out[n++] = Cs), ...);
		return n;
	}
};
template <double V> struct sform<const_num<V>> {
	static constexpr bool str = false;
	static constexpr bool coercible = is_int(V);
	static constexpr size_t len() {
		char b[24] = {};
		return fold_itoa(static_cast<long long>(V), b);
	}
	static constexpr size_t write(char * out) {
		return fold_itoa(static_cast<long long>(V), out);
	}
};
template <typename T> struct sform<num_lit<T>> {
	static constexpr folded fv = parse_int_literal(T::view());
	static constexpr bool str = false;
	static constexpr bool coercible = fv.ok();
	static constexpr size_t len() {
		char b[24] = {};
		return fold_itoa(static_cast<long long>(fv.num), b);
	}
	static constexpr size_t write(char * out) {
		return fold_itoa(static_cast<long long>(fv.num), out);
	}
};
template <> struct sform<true_lit> {
	static constexpr bool str = false, coercible = true;
	static constexpr size_t len() { return 4; }
	static constexpr size_t write(char * o) { o[0]='t';o[1]='r';o[2]='u';o[3]='e'; return 4; }
};
template <> struct sform<false_lit> {
	static constexpr bool str = false, coercible = true;
	static constexpr size_t len() { return 5; }
	static constexpr size_t write(char * o) {
		o[0]='f';o[1]='a';o[2]='l';o[3]='s';o[4]='e'; return 5;
	}
};
template <> struct sform<null_lit> {
	static constexpr bool str = false, coercible = true;
	static constexpr size_t len() { return 4; }
	static constexpr size_t write(char * o) { o[0]='n';o[1]='u';o[2]='l';o[3]='l'; return 4; }
};

template <auto Arr, size_t... I>
constexpr auto arr_to_const_str(std::index_sequence<I...>) {
	return const_str<Arr[I]...>{};
}
template <typename L, typename R> constexpr auto add_str_bytes() {
	std::array<char, sform<L>::len() + sform<R>::len()> a{};
	sform<L>::write(a.data());
	sform<R>::write(a.data() + sform<L>::len());
	return a;
}
template <typename L, typename R>
using fold_add_str_t = decltype(arr_to_const_str<add_str_bytes<L, R>()>(
    std::make_index_sequence<sform<L>::len() + sform<R>::len()>{}));

// `a + b` is a string concat when both sides are coercible constants and
// at least one is actually a string
template <typename N> struct str_fold {
	using type = N;
};
template <typename L, typename R> struct str_fold<binary<op_add, L, R>> {
	static constexpr bool concat =
	    sform<L>::coercible && sform<R>::coercible && (sform<L>::str || sform<R>::str);
	using type = std::conditional_t<concat, fold_add_str_t<L, R>, binary<op_add, L, R>>;
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
		} else if constexpr (!std::is_same_v<typename str_fold<N>::type, N>) {
			return typename str_fold<N>::type{}; // pure string concat
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
