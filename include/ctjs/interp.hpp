#ifndef CTJS__INTERP__HPP
#define CTJS__INTERP__HPP

#include "ast.hpp"
#include "value.hpp"
#include "builtins.hpp"
#ifndef CTJS_IN_A_MODULE
#include <cstddef>
#include <cstdint>
#include <memory>
#include <functional>
#include <charconv>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>
#endif

// The runtime interpreter, specialized over the type-level AST. Every
// eval_/exec_ specialization below is instantiated per script node and
// compiled by the C++ optimizer, so a ctjs script executes as code
// generated for THAT script - the tree walk happens in the compiler's
// inliner, not in a loop over node tags.
//
// Semantics notes (v0.2, V8-aligned): `var` is function-scoped and
// hoists (pre-declared undefined at function entry, recursing through
// nested blocks); let/const enter a TEMPORAL DEAD ZONE at block entry
// and `const` rejects reassignment (TypeError, V8's message); classic
// `for` with `let` creates PER-ITERATION bindings (the step runs in
// the next iteration's copy); method calls bind `this` to the receiver
// (plain calls see undefined - module semantics; sloppy-mode
// globalThis is NOT modeled). All documented in the README.

namespace ctjs::detail {

using namespace ctjs::ast;

enum class flow { normal, brk, cont, ret };

// --- literal cooking: once per node type, at first use

// parse a numeric literal. Integer and hex forms are exact and
// constexpr; a fractional/exponent literal parses with from_chars
// (non-constexpr), so such a literal is only reachable at runtime.
template <typename Text> constexpr double num_of() {
	const std::string_view s = Text::view();
	if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		std::uint64_t u = 0;
		for (std::size_t i = 2; i < s.size(); ++i) {
			const char c = s[i];
			const std::uint32_t d = c <= '9' ? static_cast<std::uint32_t>(c - '0')
			                            : static_cast<std::uint32_t>((c | 0x20) - 'a' + 10);
			u = u * 16 + d;
		}
		return static_cast<double>(u);
	}
	bool intlit = !s.empty();
	for (const char c : s) {
		if (c < '0' || c > '9') { intlit = false; break; }
	}
	if (intlit) {
		std::uint64_t u = 0;
		for (const char c : s) { u = u * 10 + static_cast<std::uint32_t>(c - '0'); }
		return static_cast<double>(u);
	}
	double d = 0;
	std::from_chars(s.data(), s.data() + s.size(), d);
	return d;
}

constexpr void push_utf8(std::string & out, char32_t cp) {
	if (cp < 0x80) {
		out += static_cast<char>(cp);
	} else if (cp < 0x800) {
		out += static_cast<char>(0xC0 | (cp >> 6));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		out += static_cast<char>(0xE0 | (cp >> 12));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	} else {
		out += static_cast<char>(0xF0 | (cp >> 18));
		out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
		out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (cp & 0x3F));
	}
}

constexpr std::string cook_string(std::string_view raw) {
	std::string out;
	std::size_t i = 1;                    // skip the opening quote
	const std::size_t end = raw.size() - 1; // and the closing one
	while (i < end) {
		const char c = raw[i];
		if (c != '\\') {
			out += c;
			++i;
			continue;
		}
		const char e = raw[i + 1];
		i += 2;
		switch (e) {
			case 'n': out += '\n'; break;
			case 't': out += '\t'; break;
			case 'r': out += '\r'; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case 'v': out += '\v'; break;
			case '0': out += '\0'; break;
			case 'x': {
				char32_t cp = 0;
				for (std::int32_t k = 0; k < 2 && i < end; ++k, ++i) {
					cp = cp * 16 + static_cast<char32_t>(
					                   raw[i] <= '9' ? raw[i] - '0' : (raw[i] | 0x20) - 'a' + 10);
				}
				push_utf8(out, cp);
				break;
			}
			case 'u': {
				char32_t cp = 0;
				for (std::int32_t k = 0; k < 4 && i < end; ++k, ++i) {
					cp = cp * 16 + static_cast<char32_t>(
					                   raw[i] <= '9' ? raw[i] - '0' : (raw[i] | 0x20) - 'a' + 10);
				}
				push_utf8(out, cp);
				break;
			}
			case '\n': break; // line continuation
			default: out += e;
		}
	}
	return out;
}

template <typename Text> constexpr std::string str_of() {
	return cook_string(Text::view());
}

// --- forward declarations

template <typename E> struct eval_;
template <typename S> struct exec_;

template <typename E> constexpr value ev(const env_ptr & env, context & cx) {
	return eval_<E>::go(env, cx);
}

// --- JS operator semantics

// declared ahead of the eval_ specializations that call it with explicit
// template arguments (two-phase lookup cannot see the later definition)
template <typename... Args>
constexpr std::vector<value> gather_args(const env_ptr & env, context & cx);

inline constexpr value to_primitive(const value & v) {
	if (v.is_array() || v.is_object() || v.is_function()) { return value{v.to_string()}; }
	return v;
}

inline constexpr value js_add(const value & l, const value & r) {
	const value lp = to_primitive(l);
	const value rp = to_primitive(r);
	if (lp.is_string() || rp.is_string()) { return value{lp.to_string() + rp.to_string()}; }
	return value{lp.to_number() + rp.to_number()};
}

template <typename Op> constexpr value binary_arith(const value & l, const value & r) {
	if constexpr (std::is_same_v<Op, op_add>) {
		return js_add(l, r);
	} else if constexpr (std::is_same_v<Op, op_sub>) {
		return value{l.to_number() - r.to_number()};
	} else if constexpr (std::is_same_v<Op, op_mul>) {
		return value{l.to_number() * r.to_number()};
	} else if constexpr (std::is_same_v<Op, op_div>) {
		return value{l.to_number() / r.to_number()};
	} else if constexpr (std::is_same_v<Op, op_mod>) {
		return value{std::fmod(l.to_number(), r.to_number())};
	} else {
		static_assert(std::is_same_v<Op, op_pow>);
		return value{std::pow(l.to_number(), r.to_number())};
	}
}

template <typename Op> constexpr bool compare_rel(const value & l, const value & r) {
	const value lp = to_primitive(l);
	const value rp = to_primitive(r);
	if (lp.is_string() && rp.is_string()) {
		const std::int32_t c = lp.as_string().compare(rp.as_string());
		if constexpr (std::is_same_v<Op, op_lt>) { return c < 0; }
		else if constexpr (std::is_same_v<Op, op_gt>) { return c > 0; }
		else if constexpr (std::is_same_v<Op, op_le>) { return c <= 0; }
		else { return c >= 0; }
	}
	const double a = lp.to_number();
	const double b = rp.to_number();
	if (std::isnan(a) || std::isnan(b)) { return false; }
	if constexpr (std::is_same_v<Op, op_lt>) { return a < b; }
	else if constexpr (std::is_same_v<Op, op_gt>) { return a > b; }
	else if constexpr (std::is_same_v<Op, op_le>) { return a <= b; }
	else { return a >= b; }
}

template <typename Op> constexpr value apply_binary(const value & l, const value & r) {
	if constexpr (std::is_same_v<Op, op_eq>) { return value{loose_equals(l, r)}; }
	else if constexpr (std::is_same_v<Op, op_ne>) { return value{!loose_equals(l, r)}; }
	else if constexpr (std::is_same_v<Op, op_seq>) { return value{strict_equals(l, r)}; }
	else if constexpr (std::is_same_v<Op, op_sne>) { return value{!strict_equals(l, r)}; }
	else if constexpr (std::is_same_v<Op, op_lt> || std::is_same_v<Op, op_gt> ||
	                   std::is_same_v<Op, op_le> || std::is_same_v<Op, op_ge>) {
		return value{compare_rel<Op>(l, r)};
	} else {
		return binary_arith<Op>(l, r);
	}
}

// --- expressions

template <typename Text> struct eval_<ident<Text>> {
	static constexpr value go(const env_ptr & env, context &) {
		bool tdz = false;
		if (const value * slot = env->find_checked(Text::view(), tdz)) { return *slot; }
		if (tdz) {
			throw_error("ReferenceError", "Cannot access '" + std::string{Text::view()} +
			                                  "' before initialization");
		}
		throw_error("ReferenceError", std::string{Text::view()} + " is not defined");
	}
};
template <> struct eval_<this_lit> {
	static constexpr value go(const env_ptr & env, context &) {
		if (const value * t = env->find("this")) { return *t; }
		return value{};
	}
};
// bare `super` is never a value; only super(...) / super.x are legal
template <> struct eval_<super_lit> {
	static constexpr value go(const env_ptr &, context &) {
		throw_error("SyntaxError", "'super' keyword unexpected here");
	}
};
// super.x — read a property off the parent prototype, `this` unchanged
template <typename Name> struct eval_<member<super_lit, Name>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		const value * sp = env->find("__super_proto__");
		if (sp == nullptr || !sp->is_object()) {
			throw_error("SyntaxError", "'super' keyword unexpected here");
		}
		return get_member(cx, *sp, Name::view());
	}
};
// super(args) — run the parent constructor against the current `this`
template <typename... Args> struct eval_<call<super_lit, Args...>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		const value * sc = env->find("__super_ctor__");
		if (sc == nullptr || !sc->is_function()) {
			throw_error("SyntaxError", "'super' call outside a derived constructor");
		}
		const value * self = env->find("this");
		std::vector<value> args = gather_args<Args...>(env, cx);
		cx.pending_this = self != nullptr ? *self : value{};
		(void)call_value(cx, *sc, std::move(args));
		return value{};
	}
};
// super.method(args) — parent-prototype method, current `this`
template <typename Name, typename... Args>
struct eval_<call<member<super_lit, Name>, Args...>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		const value * sp = env->find("__super_proto__");
		if (sp == nullptr || !sp->is_object()) {
			throw_error("SyntaxError", "'super' keyword unexpected here");
		}
		const value fn = get_member(cx, *sp, Name::view());
		const value * self = env->find("this");
		std::vector<value> args = gather_args<Args...>(env, cx);
		cx.pending_this = self != nullptr ? *self : value{};
		return call_value(cx, fn, std::move(args));
	}
};

template <typename Text> struct eval_<num_lit<Text>> {
	static constexpr value go(const env_ptr &, context &) { return value{num_of<Text>()}; }
};
// a constant the folder computed at compile time (fold.hpp) - the value
// rides in the type, so the runtime just loads it
template <double V> struct eval_<const_num<V>> {
	static constexpr value go(const env_ptr &, context &) { return value{V}; }
};
// a string the folder computed at compile time - bytes ride in the type
template <char... Cs> struct eval_<const_str<Cs...>> {
	static constexpr value go(const env_ptr &, context &) { return value{std::string{Cs...}}; }
};
template <typename Text> struct eval_<str_lit<Text>> {
	static constexpr value go(const env_ptr &, context &) { return value{str_of<Text>()}; }
};
template <> struct eval_<true_lit> {
	static constexpr value go(const env_ptr &, context &) { return value{true}; }
};
template <> struct eval_<false_lit> {
	static constexpr value go(const env_ptr &, context &) { return value{false}; }
};
template <> struct eval_<null_lit> {
	static constexpr value go(const env_ptr &, context &) { return value::null(); }
};

template <typename E> struct spread_into {
	static constexpr void go(array_t & out, const env_ptr & env, context & cx) {
		out.push_back(ev<E>(env, cx));
	}
};
template <typename E> struct spread_into<spread_arg<E>> {
	static constexpr void go(array_t & out, const env_ptr & env, context & cx) {
		const value v = ev<E>(env, cx);
		if (v.is_array()) {
			for (const value & el : *v.as_array()) { out.push_back(el); }
		} else if (v.is_string()) {
			for (const char c : v.as_string()) { out.push_back(value{std::string(1, c)}); }
		}
	}
};
template <typename... Es> struct eval_<array_lit<Es...>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		array_t out;
		(spread_into<Es>::go(out, env, cx), ...);
		return value::array(std::move(out));
	}
};

template <typename K> struct key_of;
template <typename T> struct key_of<ident<T>> {
	static constexpr std::string get() { return std::string{T::view()}; }
};
template <typename T> struct key_of<str_lit<T>> {
	static constexpr std::string get() { return str_of<T>(); }
};

template <typename... Ps> struct eval_<object_lit<Ps...>> {
	template <typename K, typename V>
	static void put(object_t & o, const env_ptr & env, context & cx, prop<K, V>) {
		o.set(key_of<K>::get(), ev<V>(env, cx));
	}
	template <typename K, typename V>
	static void put(object_t & o, const env_ptr & env, context & cx, computed_prop<K, V>) {
		const std::string key = ev<K>(env, cx).to_string();
		o.set(key, ev<V>(env, cx));
	}
	template <char Kd, typename N, typename P, typename B>
	static void put(object_t & o, const env_ptr & env, context & cx,
	                accessor_prop<Kd, N, P, B>) {
		attach_accessor(o, N::view(), Kd,
		                ev<fn_expr<P, B, false>>(env, cx));
	}
	// { ...src }: copy own enumerable props; arrays/strings spread as
	// index keys; other primitives contribute nothing, like the spec
	template <typename E>
	static void put(object_t & o, const env_ptr & env, context & cx, spread_prop<E>) {
		const value src = ev<E>(env, cx);
		if (src.is_object()) {
			for (const auto & [k, pv] : src.as_object()->props) { o.set(k, pv); }
		} else if (src.is_array()) {
			const array_t & arr = *src.as_array();
			for (std::size_t i = 0; i < arr.size(); ++i) { o.set(std::to_string(i), arr[i]); }
		} else if (src.is_string()) {
			const std::string & s = src.as_string();
			for (std::size_t i = 0; i < s.size(); ++i) {
				o.set(std::to_string(i), value{std::string(1, s[i])});
			}
		}
	}
	static constexpr value go(const env_ptr & env, context & cx) {
		object_t o;
		(put(o, env, cx, Ps{}), ...);
		return value::object(std::move(o));
	}
};

template <typename Op, typename L, typename R> struct eval_<binary<Op, L, R>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		if constexpr (std::is_same_v<Op, op_and>) {
			value l = ev<L>(env, cx);
			return l.truthy() ? ev<R>(env, cx) : l;
		} else if constexpr (std::is_same_v<Op, op_or>) {
			value l = ev<L>(env, cx);
			return l.truthy() ? l : ev<R>(env, cx);
		} else if constexpr (std::is_same_v<Op, op_nullish>) {
			value l = ev<L>(env, cx);
			return l.is_nullish() ? ev<R>(env, cx) : l;
		} else {
			const value l = ev<L>(env, cx);
			const value r = ev<R>(env, cx);
			return apply_binary<Op>(l, r);
		}
	}
};

template <typename Op, typename E> struct eval_<unary<Op, E>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		if constexpr (std::is_same_v<Op, op_typeof>) {
			return value{typeof_value(env, cx)};
		} else {
			const value v = ev<E>(env, cx);
			if constexpr (std::is_same_v<Op, op_not>) { return value{!v.truthy()}; }
			else if constexpr (std::is_same_v<Op, op_neg>) { return value{-v.to_number()}; }
			else if constexpr (std::is_same_v<Op, op_await>) { return await_value(v); }
			else { return value{v.to_number()}; }
		}
	}
	// typeof never throws on undeclared names
	static std::string_view typeof_value(const env_ptr & env, context & cx) {
		if constexpr (is_ident<E>::value) {
			if (const value * slot = env->find(ident_text<E>::view())) {
				return slot->typeof_string();
			}
			return "undefined";
		} else {
			return ev<E>(env, cx).typeof_string();
		}
	}
	template <typename X> struct is_ident : std::false_type { };
	template <typename T> struct is_ident<ident<T>> : std::true_type { };
	template <typename X> struct ident_text_impl;
	template <typename T> struct ident_text_impl<ident<T>> {
		using type = T;
	};
	template <typename X> using ident_text = typename ident_text_impl<X>::type;
};

template <typename C, typename T, typename F> struct eval_<ternary<C, T, F>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		return ev<C>(env, cx).truthy() ? ev<T>(env, cx) : ev<F>(env, cx);
	}
};

template <typename Obj, typename NameText> struct eval_<member<Obj, NameText>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		return get_member(cx, ev<Obj>(env, cx), NameText::view());
	}
};
template <typename Obj, typename Index> struct eval_<index<Obj, Index>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		const value recv = ev<Obj>(env, cx);
		return get_index(cx, recv, ev<Index>(env, cx));
	}
};

// calls: a member/index callee evaluates its receiver ONCE and parks it
// in cx.pending_this so the callee (if a script function) sees it as
// `this` - V8 method-call semantics; plain calls leave it undefined
template <typename... Args>
constexpr std::vector<value> gather_args(const env_ptr & env, context & cx) {
	std::vector<value> args;
	(spread_into<Args>::go(args, env, cx), ...);
	return args;
}
template <typename Fn, typename... Args> struct eval_<call<Fn, Args...>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		const value fn = ev<Fn>(env, cx);
		return call_value(cx, fn, gather_args<Args...>(env, cx));
	}
};
template <typename Obj, typename NameText, typename... Args>
struct eval_<call<member<Obj, NameText>, Args...>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		value recv = ev<Obj>(env, cx);
		const value fn = get_member(cx, recv, NameText::view());
		std::vector<value> args = gather_args<Args...>(env, cx);
		cx.pending_this = std::move(recv);
		return call_value(cx, fn, std::move(args));
	}
};
template <typename Obj, typename Index, typename... Args>
struct eval_<call<index<Obj, Index>, Args...>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		value recv = ev<Obj>(env, cx);
		const value fn = get_index(cx, recv, ev<Index>(env, cx));
		std::vector<value> args = gather_args<Args...>(env, cx);
		cx.pending_this = std::move(recv);
		return call_value(cx, fn, std::move(args));
	}
};

// --- optional chaining: a nullish receiver (or callee, for ?.())
// yields undefined instead of throwing. Short-circuit is PER LINK -
// a?.b.c still throws when a?.b is undefined (write a?.b?.c), unlike
// V8's whole-chain skip; documented deviation.
template <typename Obj, typename NameText> struct eval_<opt_member<Obj, NameText>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		const value recv = ev<Obj>(env, cx);
		return recv.is_nullish() ? value{} : get_member(cx, recv, NameText::view());
	}
};
template <typename Obj, typename Index> struct eval_<opt_index<Obj, Index>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		const value recv = ev<Obj>(env, cx);
		if (recv.is_nullish()) { return value{}; }
		return get_index(cx, recv, ev<Index>(env, cx));
	}
};
template <typename Fn, typename... Args> struct eval_<opt_call<Fn, Args...>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		const value fn = ev<Fn>(env, cx);
		if (fn.is_nullish()) { return value{}; }
		return call_value(cx, fn, gather_args<Args...>(env, cx));
	}
};
// method flavours keep the receiver as `this`, like their call<>
// counterparts; with opt_member the receiver may legally be nullish
template <typename Obj, typename NameText, typename... Args>
struct eval_<opt_call<member<Obj, NameText>, Args...>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		value recv = ev<Obj>(env, cx);
		const value fn = get_member(cx, recv, NameText::view());
		if (fn.is_nullish()) { return value{}; }
		std::vector<value> args = gather_args<Args...>(env, cx);
		cx.pending_this = std::move(recv);
		return call_value(cx, fn, std::move(args));
	}
};
template <typename Obj, typename NameText, typename... Args>
struct eval_<opt_call<opt_member<Obj, NameText>, Args...>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		value recv = ev<Obj>(env, cx);
		if (recv.is_nullish()) { return value{}; }
		const value fn = get_member(cx, recv, NameText::view());
		if (fn.is_nullish()) { return value{}; }
		std::vector<value> args = gather_args<Args...>(env, cx);
		cx.pending_this = std::move(recv);
		return call_value(cx, fn, std::move(args));
	}
};
template <typename Obj, typename Index, typename... Args>
struct eval_<opt_call<index<Obj, Index>, Args...>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		value recv = ev<Obj>(env, cx);
		const value fn = get_index(cx, recv, ev<Index>(env, cx));
		if (fn.is_nullish()) { return value{}; }
		std::vector<value> args = gather_args<Args...>(env, cx);
		cx.pending_this = std::move(recv);
		return call_value(cx, fn, std::move(args));
	}
};
template <typename Obj, typename Index, typename... Args>
struct eval_<opt_call<opt_index<Obj, Index>, Args...>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		value recv = ev<Obj>(env, cx);
		if (recv.is_nullish()) { return value{}; }
		const value fn = get_index(cx, recv, ev<Index>(env, cx));
		if (fn.is_nullish()) { return value{}; }
		std::vector<value> args = gather_args<Args...>(env, cx);
		cx.pending_this = std::move(recv);
		return call_value(cx, fn, std::move(args));
	}
};

// yield: only meaningful while a generator body is running (eager
// subset: the value buffers up and the expression is undefined - a
// caller cannot feed values back in through next())
template <typename E> struct eval_<yield_op<E>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		if (cx.gen_sink == nullptr) {
			throw_error("SyntaxError", "yield outside a generator");
		}
		cx.gen_sink->push_back(ev<E>(env, cx));
		return value{};
	}
};
template <> struct eval_<yield_op<void>> {
	static constexpr value go(const env_ptr &, context & cx) {
		if (cx.gen_sink == nullptr) {
			throw_error("SyntaxError", "yield outside a generator");
		}
		cx.gen_sink->push_back(value{});
		return value{};
	}
};

// instanceof: `new C()` stamps the instance with its constructor (a
// hidden __ctor prop); identity of the function object decides. There
// is no prototype chain and no extends, so this IS the whole answer.
template <typename L, typename R> struct eval_<instanceof_op<L, R>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		const value lhs = ev<L>(env, cx);
		const value rhs = ev<R>(env, cx);
		if (!rhs.is_function()) {
			throw_error("TypeError",
			            "Right-hand side of 'instanceof' is not callable");
		}
		if (!lhs.is_object()) { return value{false}; }
		// primary check: is rhs.prototype anywhere in lhs's [[Prototype]]
		// chain? (this is what covers class inheritance)
		if (rhs.as_function()->props) {
			if (const value * pt = rhs.as_function()->props->find("prototype");
			    pt != nullptr && pt->is_object()) {
				const object_t * target = pt->as_object().get();
				for (object_t * o = lhs.as_object()->proto.get(); o != nullptr;
				     o = o->proto.get()) {
					if (o == target) { return value{true}; }
				}
			}
		}
		// fallback: constructor identity (Date, plain-function `new`)
		if (const value * ctor = lhs.as_object()->find("__ctor");
		    ctor != nullptr && ctor->is_function() &&
		    ctor->as_function().get() == rhs.as_function().get()) {
			return value{true};
		}
		return value{false};
	}
};

template <typename Text> struct eval_<regex_lit<Text>> {
	static constexpr value go(const env_ptr &, context &) {
		// spelling is /body/flags; split and hand to the runtime engine
		constexpr std::string_view raw = Text::view();
		constexpr std::size_t close = raw.rfind('/');
		return make_regex(std::string{raw.substr(1, close - 1)},
		                  std::string{raw.substr(close + 1)});
	}
};

template <typename L, typename R> struct eval_<comma_op<L, R>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		(void)ev<L>(env, cx);
		return ev<R>(env, cx);
	}
};
template <typename L, typename R> struct eval_<in_op<L, R>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		const value key = ev<L>(env, cx);
		const value obj = ev<R>(env, cx);
		if (obj.is_object()) {
			return value{obj.as_object()->find(key.to_string()) != nullptr};
		}
		if (obj.is_array()) {
			const double d = key.to_number();
			return value{d >= 0 && d < static_cast<double>(obj.as_array()->size()) &&
			             d == std::floor(d)};
		}
		throw_error("TypeError", "Cannot use 'in' operator to search for '" +
		                             key.to_string() + "' in " + obj.to_string());
	}
};
// delete on a member/index removes the property (true); anything else
// is true without effect, like sloppy-mode V8 on non-references
template <typename T> struct eval_<delete_op<T>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		(void)ev<T>(env, cx);
		return value{true};
	}
};
template <typename O, typename N> struct eval_<delete_op<member<O, N>>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		const value recv = ev<O>(env, cx);
		if (recv.is_object()) {
			auto & props = recv.as_object()->props;
			std::erase_if(props, [](const auto & kv) { return kv.first == N::view(); });
		}
		return value{true};
	}
};
template <typename O, typename I> struct eval_<delete_op<index<O, I>>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		const value recv = ev<O>(env, cx);
		const value k = ev<I>(env, cx);
		if (recv.is_object()) {
			auto & props = recv.as_object()->props;
			const std::string key = k.to_string();
			std::erase_if(props, [&](const auto & kv) { return kv.first == key; });
		} else if (recv.is_array()) {
			const double d = k.to_number();
			if (d >= 0 && d < static_cast<double>(recv.as_array()->size())) {
				(*recv.as_array())[static_cast<std::size_t>(d)] = value{};
			}
		}
		return value{true};
	}
};

// template segments arrive with their delimiters on: ` or } in
// front, ` or ${ behind; escapes cook like strings (minus the quotes)
inline std::string cook_template_segment(std::string_view raw) {
	std::size_t from = 1; // leading ` or }
	std::size_t to = raw.size();
	if (raw.size() >= 2 && raw.substr(raw.size() - 2) == "${") { to -= 2; }
	else if (!raw.empty()) { to -= 1; } // trailing `
	std::string quoted = "\"";
	quoted.append(raw.substr(from, to - from));
	quoted += '\"';
	return cook_string(quoted);
}

template <typename P> struct tpl_eval {
	static void append(std::string & out, const env_ptr & env, context & cx) {
		out += ev<P>(env, cx).to_string();
	}
};
template <typename Text> struct tpl_eval<tpl_text<Text>> {
	static void append(std::string & out, const env_ptr &, context &) {
		static const std::string cooked = cook_template_segment(Text::view());
		out += cooked;
	}
};
template <typename... Parts> struct eval_<template_lit<Parts...>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		std::string out;
		(tpl_eval<Parts>::append(out, env, cx), ...);
		return value{std::move(out)};
	}
};

// new F(...): fresh object becomes `this` for the call; if the
// function returns an object, that wins (spec); else the fresh object
template <typename Callee, typename... Args> struct eval_<new_op<Callee, Args...>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		const value fn = ev<Callee>(env, cx);
		std::vector<value> args = gather_args<Args...>(env, cx);
		value obj = value::object();
		obj.as_object()->set("__ctor", fn); // instanceof fallback (Date, plain fns)
		// the instance's [[Prototype]] is the constructor's .prototype
		if (fn.is_function() && fn.as_function()->props) {
			if (const value * pt = fn.as_function()->props->find("prototype");
			    pt != nullptr && pt->is_object()) {
				obj.as_object()->proto = pt->as_object();
			}
		}
		cx.pending_this = obj;
		const value ret = call_value(cx, fn, std::move(args));
		return ret.is_object() ? ret : obj;
	}
};

// assignment-target classification
template <typename X> struct is_ident_node : std::false_type { };
template <typename T> struct is_ident_node<ident<T>> : std::true_type { };
template <typename X> struct is_member_node : std::false_type { };
template <typename O, typename N> struct is_member_node<member<O, N>> : std::true_type { };
template <typename X> struct index_parts_of;
template <typename O, typename I> struct index_parts_of<index<O, I>> {
	using key_expr = I;
};

// assignment targets: ident, member, index
template <typename Target> struct place;
template <typename T> struct place<ident<T>> {
	static value get(const env_ptr & env, context & cx) { return ev<ident<T>>(env, cx); }
	static void set(const env_ptr & env, context &, value v) {
		bool tdz = false;
		if (value * slot = env->find_checked(T::view(), tdz)) {
			if (environment * own = env->owner(T::view());
			    own != nullptr && own->is_const(T::view())) {
				throw_error("TypeError", "Assignment to constant variable.");
			}
			*slot = std::move(v);
			return;
		}
		if (tdz) {
			throw_error("ReferenceError", "Cannot access '" + std::string{T::view()} +
			                                  "' before initialization");
		}
		throw_error("ReferenceError", std::string{T::view()} + " is not defined");
	}
};
template <typename O, typename N> struct place<member<O, N>> {
	// evaluate the receiver once per operation
	static value recv(const env_ptr & env, context & cx) { return ev<O>(env, cx); }
	static value get_from(context & cx, const value & r) {
		return get_member(cx, r, N::view());
	}
	static void set_to(context & cx, const value & r, value v) {
		set_member(cx, r, N::view(), std::move(v));
	}
};
template <typename O, typename I> struct place<index<O, I>> {
	static value recv(const env_ptr & env, context & cx) { return ev<O>(env, cx); }
	template <typename K>
	static value get_from(context & cx, const value & r, const K & key) {
		return get_index(cx, r, key);
	}
};

// read-modify-write on any assignment target, shared by assignment
// operators and ++/--; Update maps the current value to the new one
template <typename Target, typename Update>
value modify_place(const env_ptr & env, context & cx, Update update) {
	if constexpr (is_ident_node<Target>::value) {
		value nv = update([&] { return place<Target>::get(env, cx); });
		place<Target>::set(env, cx, nv);
		return nv;
	} else if constexpr (is_member_node<Target>::value) {
		const value r = place<Target>::recv(env, cx);
		value nv = update([&] { return place<Target>::get_from(cx, r); });
		place<Target>::set_to(cx, r, nv);
		return nv;
	} else {
		const value r = place<Target>::recv(env, cx);
		const value key = ev<typename index_parts_of<Target>::key_expr>(env, cx);
		value nv = update([&] { return get_index(cx, r, key); });
		set_index(cx, r, key, nv);
		return nv;
	}
}

template <typename Op, typename Target, typename V> struct eval_<assign<Op, Target, V>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		return modify_place<Target>(env, cx, [&](auto cur) {
			if constexpr (std::is_same_v<Op, op_none>) {
				return ev<V>(env, cx);
			} else {
				return apply_binary<Op>(cur(), ev<V>(env, cx));
			}
		});
	}
};

template <typename Target, bool Pre, bool Inc> struct eval_<incdec<Target, Pre, Inc>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		// the old value coerces to a number first, like ToNumeric
		double old = 0;
		const value nv = modify_place<Target>(env, cx, [&](auto cur) {
			old = cur().to_number();
			return value{Inc ? old + 1 : old - 1};
		});
		return Pre ? nv : value{old};
	}
};

// --- functions: closures capture the defining environment chain

template <typename... Ss> constexpr flow exec_all(const env_ptr & env, context & cx, value & ret);
template <typename... Ss> constexpr void hoist_functions(const env_ptr & env, context & cx);

// one parameter binds and advances the positional cursor; a plain
// param is a bare name text, param_default supplies a value when the
// arg is missing (or undefined), param_rest sweeps the tail into array
template <typename P> struct bind_param {
	static constexpr void go(const env_ptr & local, const std::vector<value> & args, context &,
	               std::size_t & i) {
		local->declare(P::view(), i < args.size() ? args[i] : value{});
		++i;
	}
};
template <typename N, typename Def> struct bind_param<param_default<N, Def>> {
	static constexpr void go(const env_ptr & local, const std::vector<value> & args, context & cx,
	               std::size_t & i) {
		value v = i < args.size() ? args[i] : value{};
		if (v.is_undefined()) { v = ev<Def>(local, cx); } // default sees prior params
		local->declare(N::view(), std::move(v));
		++i;
	}
};
template <typename N> struct bind_param<param_rest<N>> {
	static constexpr void go(const env_ptr & local, const std::vector<value> & args, context &,
	               std::size_t & i) {
		array_t rest;
		for (; i < args.size(); ++i) { rest.push_back(args[i]); }
		local->declare(N::view(), value::array(std::move(rest)));
	}
};

template <typename Params> struct param_binder;
template <typename... Ps> struct param_binder<plist<Ps...>> {
	static constexpr void bind(const env_ptr & local, const std::vector<value> & args, context & cx) {
		std::size_t i = 0;
		(bind_param<Ps>::go(local, args, cx, i), ...);
	}
};

template <typename Params, typename Body, bool ExprBody, bool IsAsync = false,
          bool IsGen = false>
struct fn_maker {
	static constexpr value make(const env_ptr & env, std::string name) {
		return value::function(
		    [env](context & cx, const std::vector<value> & args) -> value {
			    auto local = rc<environment>::make();
			    local->parent = env;
			    local->function_scope = true; // var declarations land here
			    // `this` = the method-call receiver (call_value routed it
			    // into current_this), undefined for plain calls
			    local->declare("this", cx.current_this);
			    param_binder<Params>::bind(local, args, cx);
			    // yield discipline: a generator installs its buffer, any
			    // other function blinds the sink so a nested plain
			    // function cannot leak yields into an outer generator
			    std::vector<value> * saved_sink = cx.gen_sink;
			    std::vector<value> buffer;
			    cx.gen_sink = nullptr;
			    if constexpr (IsGen) { cx.gen_sink = &buffer; }
			    value out;
			    try {
				    if constexpr (ExprBody) {
					    out = ev<Body>(local, cx);
				    } else {
					    value ret;
					    const flow f = body_flow(local, cx, ret, Body{});
					    out = f == flow::ret ? std::move(ret) : value{};
				    }
			    } catch (...) {
				    cx.gen_sink = saved_sink;
				    throw;
			    }
			    cx.gen_sink = saved_sink;
			    if constexpr (IsGen) {
				    return make_generator_object(std::move(buffer), std::move(out));
			    } else {
				    return wrap(std::move(out));
			    }
		    },
		    std::move(name));
	}
	// EAGER generator result: the body already ran, yields are buffered;
	// next() drains them, honoring the iterator protocol for...of speaks
	static value make_generator_object(std::vector<value> items, value ret) {
		auto buffered = std::make_shared<std::vector<value>>(std::move(items));
		auto pos = std::make_shared<std::size_t>(0);
		object_t it;
		it.set("next", value::function(
		                   [buffered, pos, ret](context &, const std::vector<value> &) {
			                   object_t step;
			                   if (*pos < buffered->size()) {
				                   step.set("value", (*buffered)[(*pos)++]);
				                   step.set("done", value{false});
			                   } else {
				                   step.set("value", ret);
				                   step.set("done", value{true});
			                   }
			                   return value::object(std::move(step));
		                   },
		                   "next"));
		return value::object(std::move(it));
	}
	// async functions hand back a promise: settled, because the body
	// just ran to completion; a returned promise is adopted as-is
	static value wrap(value v) {
		if constexpr (IsAsync) {
			return is_promise(v) ? v : make_promise(std::move(v), false);
		} else {
			return v;
		}
	}
	template <typename... Ss>
	static constexpr flow body_flow(const env_ptr & local, context & cx, value & ret, block<Ss...>) {
		hoist_functions<Ss...>(local, cx);
		return exec_all<Ss...>(local, cx, ret);
	}
};

template <typename Params, typename Body, bool ExprBody, bool IsAsync, bool IsGen>
struct eval_<fn_expr<Params, Body, ExprBody, IsAsync, IsGen>> {
	static constexpr value go(const env_ptr & env, context & cx) {
		(void)cx;
		return fn_maker<Params, Body, ExprBody, IsAsync, IsGen>::make(env, "");
	}
};

// --- statements

template <typename S> struct exec_;

template <typename E> struct exec_<expr_stmt<E>> {
	static constexpr flow go(const env_ptr & env, context & cx, value &) {
		cx.last = ev<E>(env, cx);
		return flow::normal;
	}
};

template <typename D> struct decl_name;
template <typename N, typename I> struct decl_name<declarator<N, I>> {
	static constexpr std::string_view view() { return N::view(); }
};

// V8-faithful declaration kinds: let/const are block-scoped (const
// additionally write-protected), var lands in the nearest FUNCTION or
// global scope - its initializer still evaluates in the current env
enum class decl_kind { lexical, constant, hoisted_var };

template <typename P> struct dprop_key;
template <typename Key, typename Bind> struct dprop_key<dprop<Key, Bind>> {
	static constexpr std::string_view key() { return Key::view(); }
	static constexpr std::string_view bind() { return Bind::view(); }
};

template <decl_kind K, typename... Ds> struct declare_all;
template <decl_kind K> struct declare_all<K> {
	static constexpr void go(const env_ptr &, context &) { }
};
// a declared binding lands in the right scope for its kind
template <decl_kind K>
inline constexpr void destr_declare(const env_ptr & env, std::string_view name, value v) {
	if constexpr (K == decl_kind::hoisted_var) {
		env->hoist_target().declare(name, std::move(v));
	} else {
		env->declare(name, std::move(v));
		if constexpr (K == decl_kind::constant) { env->consts.push_back(std::string{name}); }
	}
}

// [a, b, c] = init : positional, missing -> undefined
template <decl_kind K, typename Init, typename... Names, typename... Rest>
struct declare_all<K, destr_array<Init, Names...>, Rest...> {
	static constexpr void go(const env_ptr & env, context & cx) {
		const value src = ev<Init>(env, cx);
		std::size_t i = 0;
		const auto one = [&](std::string_view nm) {
			value v;
			if (src.is_array() && i < src.as_array()->size()) { v = (*src.as_array())[i]; }
			else if (src.is_string() && i < src.as_string().size()) {
				v = value{std::string(1, src.as_string()[i])};
			}
			++i;
			destr_declare<K>(env, nm, std::move(v));
		};
		(one(Names::view()), ...);
		declare_all<K, Rest...>::go(env, cx);
	}
};

// {x: dx, y} = init : by key (shorthand key==bind)
template <decl_kind K, typename Init, typename... Props, typename... Rest>
struct declare_all<K, destr_object<Init, Props...>, Rest...> {
	static constexpr void go(const env_ptr & env, context & cx) {
		const value src = ev<Init>(env, cx);
		const auto one = [&](std::string_view key, std::string_view bind) {
			value v;
			if (src.is_object()) {
				if (const value * slot = src.as_object()->find(key)) { v = *slot; }
			}
			destr_declare<K>(env, bind, std::move(v));
		};
		(one(dprop_key<Props>::key(), dprop_key<Props>::bind()), ...);
		declare_all<K, Rest...>::go(env, cx);
	}
};

template <decl_kind K, typename N, typename Init, typename... Rest>
struct declare_all<K, declarator<N, Init>, Rest...> {
	static constexpr void go(const env_ptr & env, context & cx) {
		if constexpr (K == decl_kind::hoisted_var) {
			environment & tgt = env->hoist_target();
			if constexpr (std::is_void_v<Init>) {
				// `var x;` never clobbers an existing value
				if (tgt.local(N::view()) == nullptr) {
					tgt.declare(N::view(), value{});
				}
			} else {
				tgt.declare(N::view(), ev<Init>(env, cx));
			}
		} else {
			if constexpr (std::is_void_v<Init>) {
				env->declare(N::view(), value{});
			} else {
				env->declare(N::view(), ev<Init>(env, cx));
			}
			if constexpr (K == decl_kind::constant) {
				env->consts.push_back(std::string{N::view()});
			}
		}
		declare_all<K, Rest...>::go(env, cx);
	}
};

template <typename... Ds> struct exec_<let_stmt<Ds...>> {
	static constexpr flow go(const env_ptr & env, context & cx, value &) {
		declare_all<decl_kind::lexical, Ds...>::go(env, cx);
		return flow::normal;
	}
};
template <typename... Ds> struct exec_<const_stmt<Ds...>> {
	static constexpr flow go(const env_ptr & env, context & cx, value &) {
		declare_all<decl_kind::constant, Ds...>::go(env, cx);
		return flow::normal;
	}
};
template <typename... Ds> struct exec_<var_stmt<Ds...>> {
	static constexpr flow go(const env_ptr & env, context & cx, value &) {
		declare_all<decl_kind::hoisted_var, Ds...>::go(env, cx);
		return flow::normal;
	}
};

template <typename N, typename P, typename B, bool A, bool G>
struct exec_<fn_decl<N, P, B, A, G>> {
	static constexpr flow go(const env_ptr & env, context &, value &) {
		// usually already hoisted; declaring again is harmless
		if (env->local(N::view()) == nullptr) {
			env->declare(N::view(),
			             fn_maker<P, B, false, A, G>::make(env, std::string{N::view()}));
		}
		return flow::normal;
	}
};

// class C { constructor(){} m(){} }: C becomes a function that (with
// new) attaches every method to the fresh instance, then runs the
// constructor body with `this` bound. Methods are per-instance
// closures - prototype-equivalent for observable behavior.
// --- classes with a real prototype chain, super, statics and fields.
// Methods live ONCE on a shared prototype object; the constructor
// function carries that prototype under its .prototype static, plus
// any static members. Instances get [[Prototype]] = C.prototype
// (new_op), so get_member walks up to the methods. `super` resolves
// through hidden __super_ctor__/__super_proto__ bindings the methods
// close over.

using field_init = std::function<value(context &, const value &)>;

struct class_build {
	object_t * proto;                          // methods land here
	rc<object_t> statics;         // becomes the ctor's .props
	env_ptr cenv;                              // methods' defn env (super bindings)
	env_ptr defn;                              // outer env (computed keys/static fields)
	context * cx;
	value ctor_closure;                        // set iff a `constructor` exists
	std::vector<std::pair<std::string, field_init>> ifields; // per-instance
	std::vector<std::pair<std::string, field_init>> sfields; // per-class
};

// install one member node onto the prototype or the statics object
template <typename Inner> struct node_installer;
template <typename N, typename P, typename B>
struct node_installer<class_method<N, P, B>> {
	static constexpr void go(class_build & cb, bool st) {
		if constexpr (N::view() == std::string_view{"constructor"}) {
			if (!st) { cb.ctor_closure = fn_maker<P, B, false>::make(cb.cenv, "constructor"); }
		} else {
			object_t & tgt = st ? *cb.statics : *cb.proto;
			tgt.set(N::view(), fn_maker<P, B, false>::make(cb.cenv, std::string{N::view()}));
		}
	}
};
template <char Kd, typename N, typename P, typename B>
struct node_installer<class_accessor<Kd, N, P, B>> {
	static constexpr void go(class_build & cb, bool st) {
		attach_accessor(st ? *cb.statics : *cb.proto, N::view(), Kd,
		                fn_maker<P, B, false>::make(cb.cenv, std::string{N::view()}));
	}
};
template <typename KeyE, typename P, typename B>
struct node_installer<class_computed_method<KeyE, P, B>> {
	static constexpr void go(class_build & cb, bool st) {
		const std::string key = ev<KeyE>(cb.defn, *cb.cx).to_string();
		(st ? *cb.statics : *cb.proto)
		    .set(key, fn_maker<P, B, false>::make(cb.cenv, key));
	}
};
template <typename N> struct node_installer<class_field<N, void>> {
	static constexpr void go(class_build & cb, bool st) {
		(st ? cb.sfields : cb.ifields)
		    .push_back({std::string{N::view()}, [](context &, const value &) { return value{}; }});
	}
};
template <typename N, typename Init> struct node_installer<class_field<N, Init>> {
	static constexpr void go(class_build & cb, bool st) {
		const env_ptr cenv = cb.cenv;
		(st ? cb.sfields : cb.ifields)
		    .push_back({std::string{N::view()}, [cenv](context & cx, const value & self) {
			                auto le = rc<environment>::make();
			                le->parent = cenv;
			                le->declare("this", self);
			                return ev<Init>(le, cx);
		                }});
	}
};
template <typename KeyE, typename Init>
struct node_installer<class_computed_field<KeyE, Init>> {
	static constexpr void go(class_build & cb, bool st) {
		const std::string key = ev<KeyE>(cb.defn, *cb.cx).to_string();
		const env_ptr cenv = cb.cenv;
		(st ? cb.sfields : cb.ifields)
		    .push_back({key, [cenv](context & cx, const value & self) {
			                auto le = rc<environment>::make();
			                le->parent = cenv;
			                le->declare("this", self);
			                return ev<Init>(le, cx);
		                }});
	}
};

template <typename M> struct member_installer {
	static constexpr void go(class_build & cb) { node_installer<M>::go(cb, false); }
};
template <typename Inner> struct member_installer<static_member<Inner>> {
	static constexpr void go(class_build & cb) { node_installer<Inner>::go(cb, true); }
};

template <typename M> struct method_is_ctor : std::false_type { };
template <typename N, typename P, typename B>
struct method_is_ctor<class_method<N, P, B>>
    : std::bool_constant<N::view() == std::string_view{"constructor"}> { };

// finish a class: wire the constructor value, run static-field inits,
// declare the binding. `base` is undefined for a base class.
inline constexpr value finish_class(std::string name, const rc<object_t> & proto,
                          const rc<object_t> & statics, class_build & cb,
                          context & cx, const value & base) {
	const value protoval = value{proto};
	auto ifields = std::make_shared<std::vector<std::pair<std::string, field_init>>>(
	    std::move(cb.ifields));
	const value ctor_closure = cb.ctor_closure;
	const bool has_ctor = ctor_closure.is_function();
	value cls = value::function(
	    [ctor_closure, protoval, ifields, base, has_ctor](
	        context & c2, const std::vector<value> & args) -> value {
		    value self = c2.current_this;
		    if (!self.is_object()) {
			    self = value::object();
			    self.as_object()->proto = protoval.as_object();
		    }
		    for (const auto & [nm, fn] : *ifields) {
			    value v = fn(c2, self);
			    set_member(c2, self, nm, std::move(v));
		    }
		    if (has_ctor) {
			    c2.pending_this = self;
			    (void)call_value(c2, ctor_closure, args); // super() inits base
		    } else if (base.is_function()) {
			    c2.pending_this = self; // implicit constructor: super(...args)
			    (void)call_value(c2, base, args);
		    }
		    return self;
	    },
	    std::move(name));
	cls.as_function()->props = statics;
	statics->set("prototype", protoval);
	proto->set("constructor", cls);
	for (auto & [nm, fn] : cb.sfields) {
		value v = fn(cx, cls);
		statics->set(nm, std::move(v));
	}
	return cls;
}

template <typename Name, typename... Members> struct exec_<class_decl<Name, Members...>> {
	static constexpr flow go(const env_ptr & env, context & cx, value &) {
		auto proto = rc<object_t>::make();
		auto statics = rc<object_t>::make();
		auto cenv = rc<environment>::make();
		cenv->parent = env;
		cenv->declare("__super_ctor__", value{});  // no super in a base class
		cenv->declare("__super_proto__", value{});
		class_build cb{proto.get(), statics, cenv, env, &cx, value{}, {}, {}};
		(member_installer<Members>::go(cb), ...);
		value cls = finish_class(std::string{Name::view()}, proto, statics, cb, cx, value{});
		env->declare(Name::view(), std::move(cls));
		return flow::normal;
	}
};

template <typename Name, typename SuperName, typename... Members>
struct exec_<class_ext<Name, SuperName, Members...>> {
	static constexpr flow go(const env_ptr & env, context & cx, value &) {
		const value base = ev<ident<SuperName>>(env, cx);
		if (!base.is_function()) {
			throw_error("TypeError", "Class extends value is not a constructor");
		}
		auto proto = rc<object_t>::make();
		value base_proto{};
		auto statics = rc<object_t>::make();
		if (base.as_function()->props) {
			if (const value * bp = base.as_function()->props->find("prototype");
			    bp != nullptr && bp->is_object()) {
				proto->proto = bp->as_object();     // method inheritance
				base_proto = *bp;
			}
			statics->proto = base.as_function()->props; // static inheritance
		}
		auto cenv = rc<environment>::make();
		cenv->parent = env;
		cenv->declare("__super_ctor__", base);
		cenv->declare("__super_proto__", base_proto);
		class_build cb{proto.get(), statics, cenv, env, &cx, value{}, {}, {}};
		(member_installer<Members>::go(cb), ...);
		value cls = finish_class(std::string{Name::view()}, proto, statics, cb, cx, base);
		env->declare(Name::view(), std::move(cls));
		return flow::normal;
	}
};

template <typename... Ss> struct exec_<block<Ss...>> {
	static constexpr flow go(const env_ptr & env, context & cx, value & ret) {
		auto scope = rc<environment>::make();
		scope->parent = env;
		hoist_functions<Ss...>(scope, cx);
		return exec_all<Ss...>(scope, cx, ret);
	}
};

template <typename C, typename T, typename E> struct exec_<if_stmt<C, T, E>> {
	static constexpr flow go(const env_ptr & env, context & cx, value & ret) {
		if (ev<C>(env, cx).truthy()) { return exec_<T>::go(env, cx, ret); }
		if constexpr (!std::is_void_v<E>) { return exec_<E>::go(env, cx, ret); }
		return flow::normal;
	}
};

// labeled-flow triage shared by every loop: a loop consumes unlabeled
// break/continue and ones naming a label that directly wraps it;
// anything else propagates so an OUTER loop (or labeled_stmt) takes it
struct loop_labels {
	std::vector<std::string> mine;
	explicit loop_labels(context & cx) : mine(std::move(cx.pending_labels)) {
		cx.pending_labels.clear();
	}
	bool owns(const context & cx) const {
		for (const std::string & l : mine) {
			if (l == cx.flow_label) { return true; }
		}
		return false;
	}
	// 0 = proceed to next iteration, 1 = stop loop, 2 = propagate f
	std::int32_t triage(flow f, context & cx) const {
		if (f == flow::ret) { return 2; }
		if (f == flow::brk) {
			if (cx.flow_label.empty() || owns(cx)) { cx.flow_label.clear(); return 1; }
			return 2;
		}
		if (f == flow::cont && !cx.flow_label.empty() && !owns(cx)) { return 2; }
		if (f == flow::cont) { cx.flow_label.clear(); }
		return 0;
	}
};

template <typename C, typename B> struct exec_<while_stmt<C, B>> {
	static constexpr flow go(const env_ptr & env, context & cx, value & ret) {
		const loop_labels labels(cx);
		while (ev<C>(env, cx).truthy()) {
			const flow f = exec_<B>::go(env, cx, ret);
			const std::int32_t t = labels.triage(f, cx);
			if (t == 1) { break; }
			if (t == 2) { return f; }
		}
		return flow::normal;
	}
};

template <typename B, typename C> struct exec_<do_stmt<B, C>> {
	static constexpr flow go(const env_ptr & env, context & cx, value & ret) {
		const loop_labels labels(cx);
		do {
			const flow f = exec_<B>::go(env, cx, ret);
			const std::int32_t t = labels.triage(f, cx);
			if (t == 1) { break; }
			if (t == 2) { return f; }
		} while (ev<C>(env, cx).truthy());
		return flow::normal;
	}
};

// classic for: a `let` init gets V8's PER-ITERATION bindings - each
// iteration runs in a fresh environment seeded from the previous one,
// so closures created in the body capture that iteration's values
template <typename Init> struct loop_bindings {
	static constexpr bool per_iteration = false;
	static constexpr void copy(environment &, environment &) { }
};
template <typename... Ds> struct loop_bindings<let_stmt<Ds...>> {
	static constexpr bool per_iteration = true;
	static constexpr void copy(environment & from, environment & to) {
		const auto one = [&](std::string_view n) {
			if (value * v = from.local(n)) { to.declare(n, *v); }
		};
		(one(decl_name<Ds>::view()), ...);
	}
};

template <typename Init, typename Cond, typename Step, typename B>
struct exec_<for_stmt<Init, Cond, Step, B>> {
	static constexpr flow go(const env_ptr & env, context & cx, value & ret) {
		const loop_labels labels(cx);
		auto scope = rc<environment>::make();
		scope->parent = env;
		if constexpr (!std::is_void_v<Init>) {
			value ignored;
			(void)exec_<Init>::go(scope, cx, ignored);
		}
		constexpr bool fresh_per_iter =
		    !std::is_void_v<Init> && loop_bindings<Init>::per_iteration;
		if constexpr (fresh_per_iter) {
			// per the spec, the STEP runs in the NEXT iteration's copy of
			// the bindings - closures made in the body keep the values
			// from before the step (fs[0]() === 0, not 1)
			auto iter = rc<environment>::make();
			iter->parent = env;
			loop_bindings<Init>::copy(*scope, *iter);
			while (true) {
				if constexpr (!std::is_void_v<Cond>) {
					if (!ev<Cond>(iter, cx).truthy()) { break; }
				}
				const flow f = exec_<B>::go(iter, cx, ret);
				const std::int32_t t = labels.triage(f, cx);
				if (t == 1) { break; }
				if (t == 2) { return f; }
				auto next = rc<environment>::make();
				next->parent = env;
				loop_bindings<Init>::copy(*iter, *next);
				if constexpr (!std::is_void_v<Step>) { (void)ev<Step>(next, cx); }
				iter = std::move(next);
			}
			return flow::normal;
		}
		while (true) {
			if constexpr (!std::is_void_v<Cond>) {
				if (!ev<Cond>(scope, cx).truthy()) { break; }
			}
			const flow f = exec_<B>::go(scope, cx, ret);
			const std::int32_t t = labels.triage(f, cx);
			if (t == 1) { break; }
			if (t == 2) { return f; }
			if constexpr (!std::is_void_v<Step>) { (void)ev<Step>(scope, cx); }
		}
		return flow::normal;
	}
};

template <typename N, typename Iter, typename B> struct exec_<forof_stmt<N, Iter, B>> {
	static constexpr flow go(const env_ptr & env, context & cx, value & ret) {
		const loop_labels labels(cx);
		const value seq = ev<Iter>(env, cx);
		const auto step = [&](value element) -> flow {
			auto scope = rc<environment>::make();
			scope->parent = env;
			scope->declare(N::view(), std::move(element)); // per-iteration binding
			return exec_<B>::go(scope, cx, ret);
		};
		if (seq.is_array()) {
			const rc<array_t> arr = seq.as_array();
			for (std::size_t i = 0; i < arr->size(); ++i) {
				const flow f = step((*arr)[i]);
				const std::int32_t t = labels.triage(f, cx);
				if (t == 1) { break; }
				if (t == 2) { return f; }
			}
			return flow::normal;
		}
		if (seq.is_string()) {
			for (const char c : seq.as_string()) {
				const flow f = step(value{std::string(1, c)});
				const std::int32_t t = labels.triage(f, cx);
				if (t == 1) { break; }
				if (t == 2) { return f; }
			}
			return flow::normal;
		}
		// the iterator protocol: anything with a callable next() -
		// generator objects and hand-rolled iterators alike
		if (seq.is_object()) {
			if (const value * next = seq.as_object()->find("next");
			    next != nullptr && next->is_function()) {
				while (true) {
					cx.pending_this = seq;
					const value r = call_value(cx, *next, {});
					if (!r.is_object()) { break; }
					const value * done = r.as_object()->find("done");
					if (done != nullptr && done->truthy()) { break; }
					const value * v = r.as_object()->find("value");
					const flow f = step(v != nullptr ? *v : value{});
					const std::int32_t t = labels.triage(f, cx);
					if (t == 1) { break; }
					if (t == 2) { return f; }
				}
				return flow::normal;
			}
		}
		throw_error("TypeError", seq.to_string() + " is not iterable");
	}
};

// switch: strict-equality dispatch with FALLTHROUGH; break consumes
// flow::brk, return/continue propagate; default runs when nothing
// matched (wherever it sits in clause order, entered in source order)
template <typename Clause> struct clause_match;
template <typename E, typename... Ss> struct clause_match<case_clause<E, Ss...>> {
	static bool matches(const env_ptr & env, context & cx, const value & disc) {
		return strict_equals(ev<E>(env, cx), disc);
	}
	static constexpr flow run(const env_ptr & env, context & cx, value & ret) {
		return exec_all<Ss...>(env, cx, ret);
	}
	static constexpr bool is_default = false;
};
template <typename... Ss> struct clause_match<default_clause<Ss...>> {
	static bool matches(const env_ptr &, context &, const value &) { return false; }
	static constexpr flow run(const env_ptr & env, context & cx, value & ret) {
		return exec_all<Ss...>(env, cx, ret);
	}
	static constexpr bool is_default = true;
};
template <typename D, typename... Clauses> struct exec_<switch_stmt<D, Clauses...>> {
	static constexpr flow go(const env_ptr & env, context & cx, value & ret) {
		const value disc = ev<D>(env, cx);
		auto scope = rc<environment>::make();
		scope->parent = env;
		flow f = flow::normal;
		bool taken = false;
		const auto step = [&](auto tag, const bool match_default) {
			using C = typename decltype(tag)::type;
			if (f != flow::normal) { return; }
			if (!taken) {
				if (match_default ? clause_match<C>::is_default
				                  : clause_match<C>::matches(scope, cx, disc)) {
					taken = true;
				}
			}
			if (taken) { f = clause_match<C>::run(scope, cx, ret); }
		};
		(step(std::type_identity<Clauses>{}, false), ...);
		if (!taken) { (step(std::type_identity<Clauses>{}, true), ...); }
		if (f == flow::brk && cx.flow_label.empty()) { f = flow::normal; }
		return f;
	}
};

template <typename E> struct exec_<return_stmt<E>> {
	static constexpr flow go(const env_ptr & env, context & cx, value & ret) {
		if constexpr (std::is_void_v<E>) {
			ret = value{};
		} else {
			ret = ev<E>(env, cx);
		}
		return flow::ret;
	}
};
template <> struct exec_<break_stmt> {
	static constexpr flow go(const env_ptr &, context &, value &) { return flow::brk; }
};
template <typename L> struct exec_<break_label<L>> {
	static constexpr flow go(const env_ptr &, context & cx, value &) {
		cx.flow_label = std::string{L::view()};
		return flow::brk;
	}
};
template <typename L> struct exec_<continue_label<L>> {
	static constexpr flow go(const env_ptr &, context & cx, value &) {
		cx.flow_label = std::string{L::view()};
		return flow::cont;
	}
};
template <typename L, typename S> struct exec_<labeled_stmt<L, S>> {
	static constexpr flow go(const env_ptr & env, context & cx, value & ret) {
		cx.pending_labels.push_back(std::string{L::view()});
		const flow f = exec_<S>::go(env, cx, ret);
		// a wrapped non-loop never consumed the pending label; drop it
		for (std::size_t i = cx.pending_labels.size(); i-- > 0;) {
			if (cx.pending_labels[i] == L::view()) {
				cx.pending_labels.erase(cx.pending_labels.begin() +
				                        static_cast<ptrdiff_t>(i));
				break;
			}
		}
		// `break label` targeting a labeled non-loop statement lands here
		if (f == flow::brk && cx.flow_label == L::view()) {
			cx.flow_label.clear();
			return flow::normal;
		}
		return f;
	}
};
template <> struct exec_<continue_stmt> {
	static constexpr flow go(const env_ptr &, context &, value &) { return flow::cont; }
};
template <> struct exec_<empty_stmt> {
	static constexpr flow go(const env_ptr &, context &, value &) { return flow::normal; }
};

template <typename E> struct exec_<throw_stmt<E>> {
	static constexpr flow go(const env_ptr & env, context & cx, value &) {
		throw js_throw{ev<E>(env, cx)};
	}
};

template <typename Body, typename CatchName, typename Handler, typename Finally>
struct exec_<try_stmt<Body, CatchName, Handler, Finally>> {
	static constexpr flow go(const env_ptr & env, context & cx, value & ret) {
		flow f = flow::normal;
		bool rethrow = false;
		js_throw pending{};
		try {
			f = exec_<Body>::go(env, cx, ret);
		} catch (js_throw & t) {
			if constexpr (!std::is_void_v<Handler>) {
				auto scope = rc<environment>::make();
				scope->parent = env;
				scope->declare(CatchName::view(), t.thrown);
				try {
					f = exec_<Handler>::go(scope, cx, ret);
				} catch (js_throw & t2) {
					rethrow = true;
					pending = t2;
				}
			} else {
				rethrow = true;
				pending = t;
			}
		}
		if constexpr (!std::is_void_v<Finally>) {
			const flow ff = exec_<Finally>::go(env, cx, ret);
			if (ff != flow::normal) { return ff; } // finally's completion wins
		}
		if (rethrow) { throw pending; }
		return f;
	}
};

// --- suites: hoisting, then run in order.
//
// Three hoisting behaviors, all V8-faithful:
//  - function declarations bind at scope entry (as before);
//  - let/const names enter their TEMPORAL DEAD ZONE at block entry
//    (reads/writes before the declaration throw, with V8's message);
//  - `var` names are pre-declared undefined in the nearest FUNCTION or
//    global scope, recursing through nested blocks/ifs/loops/trys (but
//    never into nested function bodies).

// recursive var collector: declares undefined-if-absent into fnscope
template <typename S> struct hoist_vars {
	static constexpr void go(environment &) { }
};
// hoist every name a declarator BINDS (destructuring binds several)
template <typename D> struct hoist_decl_names {
	static constexpr void go(environment & fnscope) {
		const auto one = [&](std::string_view n) {
			if (fnscope.local(n) == nullptr) { fnscope.declare(n, value{}); }
		};
		one(decl_name<D>::view());
	}
};
template <typename Init, typename... Names>
struct hoist_decl_names<destr_array<Init, Names...>> {
	static constexpr void go(environment & fnscope) {
		const auto one = [&](std::string_view n) {
			if (fnscope.local(n) == nullptr) { fnscope.declare(n, value{}); }
		};
		(one(Names::view()), ...);
	}
};
template <typename Init, typename... Props>
struct hoist_decl_names<destr_object<Init, Props...>> {
	static constexpr void go(environment & fnscope) {
		const auto one = [&](std::string_view n) {
			if (fnscope.local(n) == nullptr) { fnscope.declare(n, value{}); }
		};
		(one(dprop_key<Props>::bind()), ...);
	}
};
template <typename... Ds> struct hoist_vars<var_stmt<Ds...>> {
	static constexpr void go(environment & fnscope) {
		(hoist_decl_names<Ds>::go(fnscope), ...);
	}
};
template <typename... Ss> struct hoist_vars<block<Ss...>> {
	static constexpr void go(environment & fnscope) { (hoist_vars<Ss>::go(fnscope), ...); }
};
template <typename C, typename T, typename E> struct hoist_vars<if_stmt<C, T, E>> {
	static constexpr void go(environment & fnscope) {
		hoist_vars<T>::go(fnscope);
		if constexpr (!std::is_void_v<E>) { hoist_vars<E>::go(fnscope); }
	}
};
template <typename C, typename B> struct hoist_vars<while_stmt<C, B>> {
	static constexpr void go(environment & fnscope) { hoist_vars<B>::go(fnscope); }
};
template <typename B, typename C> struct hoist_vars<do_stmt<B, C>> {
	static constexpr void go(environment & fnscope) { hoist_vars<B>::go(fnscope); }
};
template <typename Init, typename Cond, typename Step, typename B>
struct hoist_vars<for_stmt<Init, Cond, Step, B>> {
	static constexpr void go(environment & fnscope) {
		if constexpr (!std::is_void_v<Init>) { hoist_vars<Init>::go(fnscope); }
		hoist_vars<B>::go(fnscope);
	}
};
template <typename N, typename Iter, typename B> struct hoist_vars<forof_stmt<N, Iter, B>> {
	static constexpr void go(environment & fnscope) { hoist_vars<B>::go(fnscope); }
};
template <typename Clause> struct clause_vars;
template <typename E, typename... Ss> struct clause_vars<case_clause<E, Ss...>> {
	static constexpr void go(environment & fnscope) { (hoist_vars<Ss>::go(fnscope), ...); }
};
template <typename... Ss> struct clause_vars<default_clause<Ss...>> {
	static constexpr void go(environment & fnscope) { (hoist_vars<Ss>::go(fnscope), ...); }
};
template <typename D, typename... Cs> struct hoist_vars<switch_stmt<D, Cs...>> {
	static constexpr void go(environment & fnscope) { (clause_vars<Cs>::go(fnscope), ...); }
};
template <typename B, typename CN, typename H, typename F>
struct hoist_vars<try_stmt<B, CN, H, F>> {
	static constexpr void go(environment & fnscope) {
		hoist_vars<B>::go(fnscope);
		if constexpr (!std::is_void_v<H>) { hoist_vars<H>::go(fnscope); }
		if constexpr (!std::is_void_v<F>) { hoist_vars<F>::go(fnscope); }
	}
};

template <typename S> struct hoist_pick {
	static constexpr void go(const env_ptr &, context &) { }
};
template <typename N, typename P, typename B, bool A, bool G>
struct hoist_pick<fn_decl<N, P, B, A, G>> {
	static constexpr void go(const env_ptr & env, context &) {
		env->declare(N::view(),
		             fn_maker<P, B, false, A, G>::make(env, std::string{N::view()}));
	}
};
template <typename... Ds> struct hoist_pick<let_stmt<Ds...>> {
	static constexpr void go(const env_ptr & env, context &) {
		(env->tdz.push_back(std::string{decl_name<Ds>::view()}), ...);
	}
};
template <typename... Ds> struct hoist_pick<const_stmt<Ds...>> {
	static constexpr void go(const env_ptr & env, context &) {
		(env->tdz.push_back(std::string{decl_name<Ds>::view()}), ...);
	}
};
template <typename... Ss> constexpr void hoist_functions(const env_ptr & env, context & cx) {
	environment & fnscope = env->hoist_target();
	(hoist_vars<Ss>::go(fnscope), ...);
	(hoist_pick<Ss>::go(env, cx), ...);
}

template <typename... Ss> constexpr flow exec_all(const env_ptr & env, context & cx, value & ret) {
	flow f = flow::normal;
	const auto one = [&](auto tag) {
		using S = typename decltype(tag)::type;
		if (f != flow::normal) { return; }
		f = exec_<S>::go(env, cx, ret);
	};
	(one(std::type_identity<Ss>{}), ...);
	return f;
}

} // namespace ctjs::detail

#endif
