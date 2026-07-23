#ifndef CTJS__FOLD__HPP
#define CTJS__FOLD__HPP

#include "ast.hpp"
#ifndef CTJS_IN_A_MODULE
#include <cstdint>
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
		std::uint64_t u = 0;
		for (std::size_t i = 2; i < s.size(); ++i) {
			const char c = s[i];
			std::uint32_t d = 0;
			if (c >= '0' && c <= '9') { d = static_cast<std::uint32_t>(c - '0'); }
			else if (c >= 'a' && c <= 'f') { d = static_cast<std::uint32_t>(c - 'a' + 10); }
			else if (c >= 'A' && c <= 'F') { d = static_cast<std::uint32_t>(c - 'A' + 10); }
			else { return fold_none(); }
			u = u * 16 + d;
		}
		return fold_num(static_cast<double>(u));
	}
	std::uint64_t u = 0;
	for (const char c : s) {
		if (c < '0' || c > '9') { return fold_none(); } // '.'/'e' => not folded
		u = u * 10 + static_cast<std::uint32_t>(c - '0');
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
	std::int64_t e = static_cast<std::int64_t>(exp);
	double r = 1.0;
	for (std::int64_t i = 0; i < e; ++i) { r *= base; }
	return r;
}
constexpr bool is_int(double d) { return d == static_cast<double>(static_cast<std::int64_t>(d)); }

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
			                                                        static_cast<std::int64_t>(a / b)));
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
constexpr std::size_t fold_push_utf8(char * out, char32_t cp) {
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
constexpr std::size_t fold_cook(std::string_view raw, char * out) {
	std::size_t n = 0, i = 1;
	const std::size_t end = raw.size() - 1;
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
			char32_t cp = 0;
			for (std::int32_t k = 0; k < 2 && i < end; ++k, ++i) {
				cp = cp * 16 + static_cast<char32_t>(
				                   raw[i] <= '9' ? raw[i] - '0' : (raw[i] | 0x20) - 'a' + 10);
			}
			n += fold_push_utf8(out + n, cp);
			break;
		}
		case 'u': {
			char32_t cp = 0;
			for (std::int32_t k = 0; k < 4 && i < end; ++k, ++i) {
				cp = cp * 16 + static_cast<char32_t>(
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
constexpr std::size_t fold_itoa(std::int64_t v, char * out) {
	std::size_t n = 0;
	std::uint64_t u = v < 0 ? (out[n++] = '-', static_cast<std::uint64_t>(-(v + 1)) + 1)
	                             : static_cast<std::uint64_t>(v);
	char tmp[24] = {};
	std::size_t t = 0;
	if (u == 0) { tmp[t++] = '0'; }
	while (u) { tmp[t++] = static_cast<char>('0' + u % 10); u /= 10; }
	for (std::size_t k = 0; k < t; ++k) { out[n++] = tmp[t - 1 - k]; }
	return n;
}

// string-form of a coercible constant node: len() + write(out).
// `str` = it is itself a string; `coercible` = usable in string concat.
template <typename N> struct sform {
	static constexpr bool str = false;
	static constexpr bool coercible = false;
	static constexpr std::size_t len() { return 0; }
	static constexpr std::size_t write(char *) { return 0; }
};
template <typename T> struct sform<str_lit<T>> {
	static constexpr bool str = true, coercible = true;
	static constexpr std::size_t raw = T::view().size();
	static constexpr std::size_t len() {
		std::array<char, raw ? raw : 1> b{};
		return fold_cook(T::view(), b.data());
	}
	static constexpr std::size_t write(char * out) { return fold_cook(T::view(), out); }
};
template <char... Cs> struct sform<const_str<Cs...>> {
	static constexpr bool str = true, coercible = true;
	static constexpr std::size_t len() { return sizeof...(Cs); }
	static constexpr std::size_t write(char * out) {
		std::size_t n = 0;
		((out[n++] = Cs), ...);
		return n;
	}
};
template <double V> struct sform<const_num<V>> {
	static constexpr bool str = false;
	static constexpr bool coercible = is_int(V);
	static constexpr std::size_t len() {
		char b[24] = {};
		return fold_itoa(static_cast<std::int64_t>(V), b);
	}
	static constexpr std::size_t write(char * out) {
		return fold_itoa(static_cast<std::int64_t>(V), out);
	}
};
template <typename T> struct sform<num_lit<T>> {
	static constexpr folded fv = parse_int_literal(T::view());
	static constexpr bool str = false;
	static constexpr bool coercible = fv.ok();
	static constexpr std::size_t len() {
		char b[24] = {};
		return fold_itoa(static_cast<std::int64_t>(fv.num), b);
	}
	static constexpr std::size_t write(char * out) {
		return fold_itoa(static_cast<std::int64_t>(fv.num), out);
	}
};
template <> struct sform<true_lit> {
	static constexpr bool str = false, coercible = true;
	static constexpr std::size_t len() { return 4; }
	static constexpr std::size_t write(char * o) { o[0]='t';o[1]='r';o[2]='u';o[3]='e'; return 4; }
};
template <> struct sform<false_lit> {
	static constexpr bool str = false, coercible = true;
	static constexpr std::size_t len() { return 5; }
	static constexpr std::size_t write(char * o) {
		o[0]='f';o[1]='a';o[2]='l';o[3]='s';o[4]='e'; return 5;
	}
};
template <> struct sform<null_lit> {
	static constexpr bool str = false, coercible = true;
	static constexpr std::size_t len() { return 4; }
	static constexpr std::size_t write(char * o) { o[0]='n';o[1]='u';o[2]='l';o[3]='l'; return 4; }
};

template <auto Arr, std::size_t... I>
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

// --- folding pure function application: an immediately-applied
// function/arrow with a single-expression body and constant arguments
// is evaluated at compile time. `(x => x * x)(5)` -> 25,
// `(function (a, b) { return a + b; })(2, 3)` -> 5. We substitute the
// constant args for the parameters in the body expression, re-fold, and
// keep the result only if it collapses to a constant - so a body that
// touches anything dynamic (or has side effects) simply doesn't fold,
// which keeps this sound without a separate purity analysis.

template <typename N> struct fold_node;                             // (below)
template <typename N> using fold_node_t = typename fold_node<N>::type;

template <typename... Ts> struct tlist { };

// is this node a constant leaf (a literal or a folded constant)?
template <typename N> struct is_const_leaf : std::false_type { };
template <double V> struct is_const_leaf<const_num<V>> : std::true_type { };
template <char... Cs> struct is_const_leaf<const_str<Cs...>> : std::true_type { };
template <typename T> struct is_const_leaf<num_lit<T>> : std::true_type { };
template <typename T> struct is_const_leaf<str_lit<T>> : std::true_type { };
template <> struct is_const_leaf<true_lit> : std::true_type { };
template <> struct is_const_leaf<false_lit> : std::true_type { };
template <> struct is_const_leaf<null_lit> : std::true_type { };

// only plain params (a bare name text) can be substituted; a default or
// rest param disables the fold
template <typename P> struct is_plain_param : std::true_type { };
template <typename N, typename D> struct is_plain_param<param_default<N, D>> : std::false_type { };
template <typename N> struct is_plain_param<param_rest<N>> : std::false_type { };

// look a name up in parallel (names, args) lists; unmatched -> the ident
template <typename NameText, typename Names, typename Args> struct pmap {
	using type = ident<NameText>;
};
template <typename NameText, typename N0, typename... Ns, typename A0, typename... As>
struct pmap<NameText, tlist<N0, Ns...>, tlist<A0, As...>> {
	using type = std::conditional_t<NameText::view() == N0::view(), A0,
	                                typename pmap<NameText, tlist<Ns...>, tlist<As...>>::type>;
};

// substitute the parameters (Names -> Args) through an expression:
// replace each `ident<param>` with its argument, recursing through every
// type-parameterised node generically. Nodes with non-type parameters
// (const_num, const_str, a nested fn_expr) are left as-is - which is
// exactly right: we do not substitute into inner functions.
template <typename E, typename Names, typename Args> struct subst {
	using type = E;
};
template <typename T, typename Names, typename Args> struct subst<ident<T>, Names, Args> {
	using type = typename pmap<T, Names, Args>::type;
};
template <template <typename...> class Tmpl, typename... Ks, typename Names, typename Args>
struct subst<Tmpl<Ks...>, Names, Args> {
	using type = Tmpl<typename subst<Ks, Names, Args>::type...>;
};

// the body EXPRESSION of a single-expression function/arrow, or void
template <typename Callee> struct iife_body {
	using type = void;
};
template <typename Params, typename E> // arrow with an expression body
struct iife_body<fn_expr<Params, E, true, false, false>> {
	using type = E;
};
template <typename Params, typename E> // { return E; }
struct iife_body<fn_expr<Params, block<return_stmt<E>>, false, false, false>> {
	using type = E;
};

template <typename Params> struct param_names {
	using type = tlist<>;
	static constexpr bool all_plain = false;
};
template <typename... Ps> struct param_names<plist<Ps...>> {
	using type = tlist<Ps...>; // Ps are the bare name texts for plain params
	static constexpr bool all_plain = (... && is_plain_param<Ps>::value);
};

// fold an immediately-applied function; `type` is the folded constant or
// the original call if it does not reduce
template <typename N> struct iife_fold {
	using type = N;
};
template <typename Params, typename Body, bool Eb, typename... Args>
struct iife_fold<call<fn_expr<Params, Body, Eb, false, false>, Args...>> {
	using Callee = fn_expr<Params, Body, Eb, false, false>;
	using BodyExpr = typename iife_body<Callee>::type;
	static constexpr bool ok = !std::is_void_v<BodyExpr> && param_names<Params>::all_plain &&
	                           sizeof...(Args) > 0 && (... && is_const_leaf<Args>::value);
	static constexpr auto pick() {
		if constexpr (ok) {
			using folded_body =
			    fold_node_t<typename subst<BodyExpr, typename param_names<Params>::type,
			                               tlist<Args...>>::type>;
			if constexpr (is_const_leaf<folded_body>::value) {
				return folded_body{};
			} else {
				return call<Callee, Args...>{};
			}
		} else {
			return call<Callee, Args...>{};
		}
	}
	using type = decltype(pick());
};

// --- constant evaluation of NAMED functions. A whole-program pass
// collects top-level `function name(...) { return <expr>; }` definitions
// (plain params, single return) and, wherever such a function is called
// with constant arguments, substitutes and folds its body - recursively,
// so `function fact(n){ return n<=1 ? 1 : n*fact(n-1); } fact(5)` folds
// to 120. Recursion is bounded by a depth budget; anything that does not
// reduce to a constant is left for the interpreter, so it stays sound.

template <typename NameText, typename Params, typename BodyExpr> struct fn_def { };
template <typename D> struct def_params;
template <typename N, typename P, typename E> struct def_params<fn_def<N, P, E>> {
	using type = P;
};
template <typename D> struct def_body;
template <typename N, typename P, typename E> struct def_body<fn_def<N, P, E>> {
	using type = E;
};
template <typename D> struct def_name;
template <typename N, typename P, typename E> struct def_name<fn_def<N, P, E>> {
	using type = N;
};
template <typename P> struct param_count {
	static constexpr std::size_t value = 0;
};
template <typename... Ps> struct param_count<plist<Ps...>> {
	static constexpr std::size_t value = sizeof...(Ps);
};

// a top-level fn_decl -> fn_def (or void if not foldable)
template <typename S> struct fn_def_of {
	using type = void;
};
template <typename N, typename P, typename E>
struct fn_def_of<fn_decl<N, P, block<return_stmt<E>>, false, false>> {
	using type = std::conditional_t<param_names<P>::all_plain, fn_def<N, P, E>, void>;
};

// build the function table by scanning a program's top-level statements
template <typename List, typename X> struct tappend {
	using type = List;
};
template <typename... Ls, typename X> struct tappend<tlist<Ls...>, X> {
	using type = tlist<Ls..., X>;
};
template <typename... Ls> struct tappend<tlist<Ls...>, void> {
	using type = tlist<Ls...>;
};
template <typename Prog> struct collect_fns {
	using type = tlist<>;
};
template <typename... Ss> struct collect_fns<program<Ss...>> {
	template <typename Acc, typename... Rest> struct go {
		using type = Acc;
	};
	template <typename Acc, typename S0, typename... Rest> struct go<Acc, S0, Rest...> {
		using type = typename go<typename tappend<Acc, typename fn_def_of<S0>::type>::type,
		                         Rest...>::type;
	};
	using type = typename go<tlist<>, Ss...>::type;
};

template <typename Name, typename FT> struct fn_lookup {
	using type = void;
};
template <typename Name, typename D0, typename... Ds> struct fn_lookup<Name, tlist<D0, Ds...>> {
	using type = std::conditional_t<Name::view() == def_name<D0>::type::view(), D0,
	                                typename fn_lookup<Name, tlist<Ds...>>::type>;
};

// the rewrite: fold constant-argument calls to table functions,
// recursing through the whole tree generically (re-folding each node).
template <typename N, typename FT, std::int32_t Depth> struct rewrite {
	using type = N;
};
template <template <typename...> class Tmpl, typename... Ks, typename FT, std::int32_t Depth>
struct rewrite<Tmpl<Ks...>, FT, Depth> {
	using type = fold_node_t<Tmpl<typename rewrite<Ks, FT, Depth>::type...>>;
};
template <typename NameText, typename... Args, typename FT, std::int32_t Depth>
struct rewrite<call<ident<NameText>, Args...>, FT, Depth> {
	using def = typename fn_lookup<NameText, FT>::type;
	static constexpr auto pick() {
		using rebuilt = call<ident<NameText>, typename rewrite<Args, FT, Depth>::type...>;
		if constexpr (std::is_void_v<def> || Depth <= 0) {
			return rebuilt{};
		} else {
			using P = typename def_params<def>::type;
			constexpr bool ok = sizeof...(Args) > 0 &&
			                    param_count<P>::value == sizeof...(Args) &&
			                    param_names<P>::all_plain &&
			                    (is_const_leaf<typename rewrite<Args, FT, Depth>::type>::value && ...);
			if constexpr (ok) {
				using B = typename subst<typename def_body<def>::type,
				                         typename param_names<P>::type,
				                         tlist<typename rewrite<Args, FT, Depth>::type...>>::type;
				using Bf = fold_node_t<typename rewrite<B, FT, Depth - 1>::type>;
				if constexpr (is_const_leaf<Bf>::value) {
					return Bf{};
				} else {
					return rebuilt{};
				}
			} else {
				return rebuilt{};
			}
		}
	}
	using type = decltype(pick());
};
// depth 16 bounds compile time: linear recursion (factorial-shaped)
// folds to that depth, branching recursion (fibonacci-shaped) costs at
// most ~2^16 instantiations before bailing to the interpreter.
// A program with no foldable top-level functions skips the pass entirely
// (empty table) - so scripts that don't use them pay nothing.
template <typename Node, typename FT> struct apply_named {
	using type = typename rewrite<Node, FT, 16>::type;
};
template <typename Node> struct apply_named<Node, tlist<>> {
	using type = Node;
};
template <typename Node, typename FT> using rewrite_t = typename apply_named<Node, FT>::type;

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
		} else if constexpr (!std::is_same_v<typename iife_fold<N>::type, N>) {
			return typename iife_fold<N>::type{}; // pure function applied to constants
		} else if constexpr (!std::is_same_v<typename str_fold<N>::type, N>) {
			return typename str_fold<N>::type{}; // pure string concat
		} else {
			return typename simplify<N>::type{};
		}
	}
	using type = decltype(go());
};

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
