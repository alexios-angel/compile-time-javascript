#ifndef CTJS__INTERP__HPP
#define CTJS__INTERP__HPP

#include "ast.hpp"
#include "value.hpp"
#include "builtins.hpp"
#ifndef CTJS_IN_A_MODULE
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

template <typename Text> const double & num_of() {
	static const double v = [] {
		const std::string_view s = Text::view();
		if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
			unsigned long long u = 0;
			std::from_chars(s.data() + 2, s.data() + s.size(), u, 16);
			return static_cast<double>(u);
		}
		double d = 0;
		std::from_chars(s.data(), s.data() + s.size(), d);
		return d;
	}();
	return v;
}

inline void push_utf8(std::string & out, unsigned long cp) {
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

inline std::string cook_string(std::string_view raw) {
	std::string out;
	size_t i = 1;                    // skip the opening quote
	const size_t end = raw.size() - 1; // and the closing one
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
				unsigned long cp = 0;
				for (int k = 0; k < 2 && i < end; ++k, ++i) {
					cp = cp * 16 + static_cast<unsigned long>(
					                   raw[i] <= '9' ? raw[i] - '0' : (raw[i] | 0x20) - 'a' + 10);
				}
				push_utf8(out, cp);
				break;
			}
			case 'u': {
				unsigned long cp = 0;
				for (int k = 0; k < 4 && i < end; ++k, ++i) {
					cp = cp * 16 + static_cast<unsigned long>(
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

template <typename Text> const std::string & str_of() {
	static const std::string v = cook_string(Text::view());
	return v;
}

// --- forward declarations

template <typename E> struct eval_;
template <typename S> struct exec_;

template <typename E> value ev(const env_ptr & env, context & cx) {
	return eval_<E>::go(env, cx);
}

// --- JS operator semantics

inline value to_primitive(const value & v) {
	if (v.is_array() || v.is_object() || v.is_function()) { return value{v.to_string()}; }
	return v;
}

inline value js_add(const value & l, const value & r) {
	const value lp = to_primitive(l);
	const value rp = to_primitive(r);
	if (lp.is_string() || rp.is_string()) { return value{lp.to_string() + rp.to_string()}; }
	return value{lp.to_number() + rp.to_number()};
}

template <typename Op> value binary_arith(const value & l, const value & r) {
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

template <typename Op> bool compare_rel(const value & l, const value & r) {
	const value lp = to_primitive(l);
	const value rp = to_primitive(r);
	if (lp.is_string() && rp.is_string()) {
		const int c = lp.as_string().compare(rp.as_string());
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

template <typename Op> value apply_binary(const value & l, const value & r) {
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
	static value go(const env_ptr & env, context &) {
		bool tdz = false;
		if (const value * slot = env->find_checked(Text::view(), tdz)) { return *slot; }
		if (tdz) {
			throw_error("ReferenceError", "Cannot access '" + std::string{Text::view()} +
			                                  "' before initialization");
		}
		throw_error("ReferenceError", std::string{Text::view()} + " is not defined");
	}
};
template <typename Text> struct eval_<num_lit<Text>> {
	static value go(const env_ptr &, context &) { return value{num_of<Text>()}; }
};
template <typename Text> struct eval_<str_lit<Text>> {
	static value go(const env_ptr &, context &) { return value{str_of<Text>()}; }
};
template <> struct eval_<true_lit> {
	static value go(const env_ptr &, context &) { return value{true}; }
};
template <> struct eval_<false_lit> {
	static value go(const env_ptr &, context &) { return value{false}; }
};
template <> struct eval_<null_lit> {
	static value go(const env_ptr &, context &) { return value::null(); }
};

template <typename E> struct spread_into {
	static void go(array_t & out, const env_ptr & env, context & cx) {
		out.push_back(ev<E>(env, cx));
	}
};
template <typename E> struct spread_into<spread_arg<E>> {
	static void go(array_t & out, const env_ptr & env, context & cx) {
		const value v = ev<E>(env, cx);
		if (v.is_array()) {
			for (const value & el : *v.as_array()) { out.push_back(el); }
		} else if (v.is_string()) {
			for (const char c : v.as_string()) { out.push_back(value{std::string(1, c)}); }
		}
	}
};
template <typename... Es> struct eval_<array_lit<Es...>> {
	static value go(const env_ptr & env, context & cx) {
		array_t out;
		(spread_into<Es>::go(out, env, cx), ...);
		return value::array(std::move(out));
	}
};

template <typename K> struct key_of;
template <typename T> struct key_of<ident<T>> {
	static std::string_view get() { return T::view(); }
};
template <typename T> struct key_of<str_lit<T>> {
	static std::string_view get() { return str_of<T>(); }
};

template <typename... Ps> struct eval_<object_lit<Ps...>> {
	template <typename K, typename V>
	static void put(object_t & o, const env_ptr & env, context & cx, prop<K, V>) {
		o.set(key_of<K>::get(), ev<V>(env, cx));
	}
	static value go(const env_ptr & env, context & cx) {
		object_t o;
		(put(o, env, cx, Ps{}), ...);
		return value::object(std::move(o));
	}
};

template <typename Op, typename L, typename R> struct eval_<binary<Op, L, R>> {
	static value go(const env_ptr & env, context & cx) {
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
	static value go(const env_ptr & env, context & cx) {
		if constexpr (std::is_same_v<Op, op_typeof>) {
			return value{typeof_value(env, cx)};
		} else {
			const value v = ev<E>(env, cx);
			if constexpr (std::is_same_v<Op, op_not>) { return value{!v.truthy()}; }
			else if constexpr (std::is_same_v<Op, op_neg>) { return value{-v.to_number()}; }
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
	static value go(const env_ptr & env, context & cx) {
		return ev<C>(env, cx).truthy() ? ev<T>(env, cx) : ev<F>(env, cx);
	}
};

template <typename Obj, typename NameText> struct eval_<member<Obj, NameText>> {
	static value go(const env_ptr & env, context & cx) {
		return get_member(cx, ev<Obj>(env, cx), NameText::view());
	}
};
template <typename Obj, typename Index> struct eval_<index<Obj, Index>> {
	static value go(const env_ptr & env, context & cx) {
		const value recv = ev<Obj>(env, cx);
		return get_index(cx, recv, ev<Index>(env, cx));
	}
};

// calls: a member/index callee evaluates its receiver ONCE and parks it
// in cx.pending_this so the callee (if a script function) sees it as
// `this` - V8 method-call semantics; plain calls leave it undefined
template <typename... Args>
inline std::vector<value> gather_args(const env_ptr & env, context & cx) {
	std::vector<value> args;
	(spread_into<Args>::go(args, env, cx), ...);
	return args;
}
template <typename Fn, typename... Args> struct eval_<call<Fn, Args...>> {
	static value go(const env_ptr & env, context & cx) {
		const value fn = ev<Fn>(env, cx);
		return call_value(cx, fn, gather_args<Args...>(env, cx));
	}
};
template <typename Obj, typename NameText, typename... Args>
struct eval_<call<member<Obj, NameText>, Args...>> {
	static value go(const env_ptr & env, context & cx) {
		value recv = ev<Obj>(env, cx);
		const value fn = get_member(cx, recv, NameText::view());
		std::vector<value> args = gather_args<Args...>(env, cx);
		cx.pending_this = std::move(recv);
		return call_value(cx, fn, std::move(args));
	}
};
template <typename Obj, typename Index, typename... Args>
struct eval_<call<index<Obj, Index>, Args...>> {
	static value go(const env_ptr & env, context & cx) {
		value recv = ev<Obj>(env, cx);
		const value fn = get_index(cx, recv, ev<Index>(env, cx));
		std::vector<value> args = gather_args<Args...>(env, cx);
		cx.pending_this = std::move(recv);
		return call_value(cx, fn, std::move(args));
	}
};

template <typename L, typename R> struct eval_<comma_op<L, R>> {
	static value go(const env_ptr & env, context & cx) {
		(void)ev<L>(env, cx);
		return ev<R>(env, cx);
	}
};
template <typename L, typename R> struct eval_<in_op<L, R>> {
	static value go(const env_ptr & env, context & cx) {
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
	static value go(const env_ptr & env, context & cx) {
		(void)ev<T>(env, cx);
		return value{true};
	}
};
template <typename O, typename N> struct eval_<delete_op<member<O, N>>> {
	static value go(const env_ptr & env, context & cx) {
		const value recv = ev<O>(env, cx);
		if (recv.is_object()) {
			auto & props = recv.as_object()->props;
			std::erase_if(props, [](const auto & kv) { return kv.first == N::view(); });
		}
		return value{true};
	}
};
template <typename O, typename I> struct eval_<delete_op<index<O, I>>> {
	static value go(const env_ptr & env, context & cx) {
		const value recv = ev<O>(env, cx);
		const value k = ev<I>(env, cx);
		if (recv.is_object()) {
			auto & props = recv.as_object()->props;
			const std::string key = k.to_string();
			std::erase_if(props, [&](const auto & kv) { return kv.first == key; });
		} else if (recv.is_array()) {
			const double d = k.to_number();
			if (d >= 0 && d < static_cast<double>(recv.as_array()->size())) {
				(*recv.as_array())[static_cast<size_t>(d)] = value{};
			}
		}
		return value{true};
	}
};

// template segments arrive with their delimiters on: ` or } in
// front, ` or ${ behind; escapes cook like strings (minus the quotes)
inline std::string cook_template_segment(std::string_view raw) {
	size_t from = 1; // leading ` or }
	size_t to = raw.size();
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
	static value go(const env_ptr & env, context & cx) {
		std::string out;
		(tpl_eval<Parts>::append(out, env, cx), ...);
		return value{std::move(out)};
	}
};

// new F(...): fresh object becomes `this` for the call; if the
// function returns an object, that wins (spec); else the fresh object
template <typename Callee, typename... Args> struct eval_<new_op<Callee, Args...>> {
	static value go(const env_ptr & env, context & cx) {
		const value fn = ev<Callee>(env, cx);
		std::vector<value> args = gather_args<Args...>(env, cx);
		value obj = value::object();
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
			    own != nullptr && own->consts.contains(T::view())) {
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
	static void set_to(const value & r, value v) { set_member(r, N::view(), std::move(v)); }
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
		place<Target>::set_to(r, nv);
		return nv;
	} else {
		const value r = place<Target>::recv(env, cx);
		const value key = ev<typename index_parts_of<Target>::key_expr>(env, cx);
		value nv = update([&] { return get_index(cx, r, key); });
		set_index(r, key, nv);
		return nv;
	}
}

template <typename Op, typename Target, typename V> struct eval_<assign<Op, Target, V>> {
	static value go(const env_ptr & env, context & cx) {
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
	static value go(const env_ptr & env, context & cx) {
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

template <typename... Ss> flow exec_all(const env_ptr & env, context & cx, value & ret);
template <typename... Ss> void hoist_functions(const env_ptr & env, context & cx);

// one parameter binds and advances the positional cursor; a plain
// param is a bare name text, param_default supplies a value when the
// arg is missing (or undefined), param_rest sweeps the tail into array
template <typename P> struct bind_param {
	static void go(const env_ptr & local, const std::vector<value> & args, context &,
	               size_t & i) {
		local->declare(P::view(), i < args.size() ? args[i] : value{});
		++i;
	}
};
template <typename N, typename Def> struct bind_param<param_default<N, Def>> {
	static void go(const env_ptr & local, const std::vector<value> & args, context & cx,
	               size_t & i) {
		value v = i < args.size() ? args[i] : value{};
		if (v.is_undefined()) { v = ev<Def>(local, cx); } // default sees prior params
		local->declare(N::view(), std::move(v));
		++i;
	}
};
template <typename N> struct bind_param<param_rest<N>> {
	static void go(const env_ptr & local, const std::vector<value> & args, context &,
	               size_t & i) {
		array_t rest;
		for (; i < args.size(); ++i) { rest.push_back(args[i]); }
		local->declare(N::view(), value::array(std::move(rest)));
	}
};

template <typename Params> struct param_binder;
template <typename... Ps> struct param_binder<plist<Ps...>> {
	static void bind(const env_ptr & local, const std::vector<value> & args, context & cx) {
		size_t i = 0;
		(bind_param<Ps>::go(local, args, cx, i), ...);
	}
};

template <typename Params, typename Body, bool ExprBody> struct fn_maker {
	static value make(const env_ptr & env, std::string name) {
		return value::function(
		    [env](context & cx, const std::vector<value> & args) -> value {
			    auto local = std::make_shared<environment>();
			    local->parent = env;
			    local->function_scope = true; // var declarations land here
			    // `this` = the method-call receiver (call_value routed it
			    // into current_this), undefined for plain calls
			    local->declare("this", cx.current_this);
			    param_binder<Params>::bind(local, args, cx);
			    if constexpr (ExprBody) {
				    return ev<Body>(local, cx);
			    } else {
				    value ret;
				    const flow f = body_flow(local, cx, ret, Body{});
				    return f == flow::ret ? ret : value{};
			    }
		    },
		    std::move(name));
	}
	template <typename... Ss>
	static flow body_flow(const env_ptr & local, context & cx, value & ret, block<Ss...>) {
		hoist_functions<Ss...>(local, cx);
		return exec_all<Ss...>(local, cx, ret);
	}
};

template <typename Params, typename Body, bool ExprBody>
struct eval_<fn_expr<Params, Body, ExprBody>> {
	static value go(const env_ptr & env, context & cx) {
		(void)cx;
		return fn_maker<Params, Body, ExprBody>::make(env, "");
	}
};

// --- statements

template <typename S> struct exec_;

template <typename E> struct exec_<expr_stmt<E>> {
	static flow go(const env_ptr & env, context & cx, value &) {
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
	static void go(const env_ptr &, context &) { }
};
// a declared binding lands in the right scope for its kind
template <decl_kind K>
inline void destr_declare(const env_ptr & env, std::string_view name, value v) {
	if constexpr (K == decl_kind::hoisted_var) {
		env->hoist_target().declare(name, std::move(v));
	} else {
		env->declare(name, std::move(v));
		if constexpr (K == decl_kind::constant) { env->consts.insert(std::string{name}); }
	}
}

// [a, b, c] = init : positional, missing -> undefined
template <decl_kind K, typename Init, typename... Names, typename... Rest>
struct declare_all<K, destr_array<Init, Names...>, Rest...> {
	static void go(const env_ptr & env, context & cx) {
		const value src = ev<Init>(env, cx);
		size_t i = 0;
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
	static void go(const env_ptr & env, context & cx) {
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
	static void go(const env_ptr & env, context & cx) {
		if constexpr (K == decl_kind::hoisted_var) {
			environment & tgt = env->hoist_target();
			if constexpr (std::is_void_v<Init>) {
				// `var x;` never clobbers an existing value
				if (tgt.vars.find(N::view()) == tgt.vars.end()) {
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
				env->consts.insert(std::string{N::view()});
			}
		}
		declare_all<K, Rest...>::go(env, cx);
	}
};

template <typename... Ds> struct exec_<let_stmt<Ds...>> {
	static flow go(const env_ptr & env, context & cx, value &) {
		declare_all<decl_kind::lexical, Ds...>::go(env, cx);
		return flow::normal;
	}
};
template <typename... Ds> struct exec_<const_stmt<Ds...>> {
	static flow go(const env_ptr & env, context & cx, value &) {
		declare_all<decl_kind::constant, Ds...>::go(env, cx);
		return flow::normal;
	}
};
template <typename... Ds> struct exec_<var_stmt<Ds...>> {
	static flow go(const env_ptr & env, context & cx, value &) {
		declare_all<decl_kind::hoisted_var, Ds...>::go(env, cx);
		return flow::normal;
	}
};

template <typename N, typename P, typename B> struct exec_<fn_decl<N, P, B>> {
	static flow go(const env_ptr & env, context &, value &) {
		// usually already hoisted; declaring again is harmless
		if (env->vars.find(N::view()) == env->vars.end()) {
			env->declare(N::view(), fn_maker<P, B, false>::make(env, std::string{N::view()}));
		}
		return flow::normal;
	}
};

// class C { constructor(){} m(){} }: C becomes a function that (with
// new) attaches every method to the fresh instance, then runs the
// constructor body with `this` bound. Methods are per-instance
// closures - prototype-equivalent for observable behavior.
template <typename M> struct attach_method;
template <typename N, typename P, typename B> struct attach_method<class_method<N, P, B>> {
	static constexpr bool is_ctor = false;
	static void attach(const env_ptr & defn_env, const value & obj) {
		obj.as_object()->set(N::view(), fn_maker<P, B, false>::make(defn_env,
		                                                            std::string{N::view()}));
	}
	static value ctor(const env_ptr &) { return value{}; }
};
template <typename P, typename B> struct ctor_of {
	static value make(const env_ptr & env) { return fn_maker<P, B, false>::make(env, "constructor"); }
};

template <typename Name, typename... Methods> struct exec_<class_decl<Name, Methods...>> {
	static flow go(const env_ptr & env, context & cx, value &) {
		(void)cx;
		env_ptr defn = env;
		value cls = value::function(
		    [defn](context & cx2, const std::vector<value> & args) -> value {
			    // `this` (the fresh instance) was parked by new_op
			    value self = cx2.current_this;
			    if (!self.is_object()) { self = value::object(); }
			    (attach_one<Methods>(defn, self), ...);
			    value ctor_fn;
			    if (const value * c = self.as_object()->find("constructor")) {
				    ctor_fn = *c;
			    }
			    if (ctor_fn.is_function()) {
				    cx2.pending_this = self;
				    (void)call_value(cx2, ctor_fn, args);
			    }
			    return self;
		    },
		    std::string{Name::view()});
		env->declare(Name::view(), std::move(cls));
		return flow::normal;
	}
	template <typename M> static void attach_one(const env_ptr & defn, const value & self) {
		attach_method<M>::attach(defn, self);
	}
};

template <typename... Ss> struct exec_<block<Ss...>> {
	static flow go(const env_ptr & env, context & cx, value & ret) {
		auto scope = std::make_shared<environment>();
		scope->parent = env;
		hoist_functions<Ss...>(scope, cx);
		return exec_all<Ss...>(scope, cx, ret);
	}
};

template <typename C, typename T, typename E> struct exec_<if_stmt<C, T, E>> {
	static flow go(const env_ptr & env, context & cx, value & ret) {
		if (ev<C>(env, cx).truthy()) { return exec_<T>::go(env, cx, ret); }
		if constexpr (!std::is_void_v<E>) { return exec_<E>::go(env, cx, ret); }
		return flow::normal;
	}
};

template <typename C, typename B> struct exec_<while_stmt<C, B>> {
	static flow go(const env_ptr & env, context & cx, value & ret) {
		while (ev<C>(env, cx).truthy()) {
			const flow f = exec_<B>::go(env, cx, ret);
			if (f == flow::brk) { break; }
			if (f == flow::ret) { return f; }
		}
		return flow::normal;
	}
};

template <typename B, typename C> struct exec_<do_stmt<B, C>> {
	static flow go(const env_ptr & env, context & cx, value & ret) {
		do {
			const flow f = exec_<B>::go(env, cx, ret);
			if (f == flow::brk) { break; }
			if (f == flow::ret) { return f; }
		} while (ev<C>(env, cx).truthy());
		return flow::normal;
	}
};

// classic for: a `let` init gets V8's PER-ITERATION bindings - each
// iteration runs in a fresh environment seeded from the previous one,
// so closures created in the body capture that iteration's values
template <typename Init> struct loop_bindings {
	static constexpr bool per_iteration = false;
	static void copy(environment &, environment &) { }
};
template <typename... Ds> struct loop_bindings<let_stmt<Ds...>> {
	static constexpr bool per_iteration = true;
	static void copy(environment & from, environment & to) {
		const auto one = [&](std::string_view n) {
			if (const auto it = from.vars.find(n); it != from.vars.end()) {
				to.vars.insert_or_assign(std::string{n}, it->second);
			}
		};
		(one(decl_name<Ds>::view()), ...);
	}
};

template <typename Init, typename Cond, typename Step, typename B>
struct exec_<for_stmt<Init, Cond, Step, B>> {
	static flow go(const env_ptr & env, context & cx, value & ret) {
		auto scope = std::make_shared<environment>();
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
			auto iter = std::make_shared<environment>();
			iter->parent = env;
			loop_bindings<Init>::copy(*scope, *iter);
			while (true) {
				if constexpr (!std::is_void_v<Cond>) {
					if (!ev<Cond>(iter, cx).truthy()) { break; }
				}
				const flow f = exec_<B>::go(iter, cx, ret);
				if (f == flow::ret) { return f; }
				if (f == flow::brk) { break; }
				auto next = std::make_shared<environment>();
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
			if (f == flow::brk) { break; }
			if (f == flow::ret) { return f; }
			if constexpr (!std::is_void_v<Step>) { (void)ev<Step>(scope, cx); }
		}
		return flow::normal;
	}
};

template <typename N, typename Iter, typename B> struct exec_<forof_stmt<N, Iter, B>> {
	static flow go(const env_ptr & env, context & cx, value & ret) {
		const value seq = ev<Iter>(env, cx);
		const auto step = [&](value element) -> flow {
			auto scope = std::make_shared<environment>();
			scope->parent = env;
			scope->declare(N::view(), std::move(element)); // per-iteration binding
			return exec_<B>::go(scope, cx, ret);
		};
		if (seq.is_array()) {
			const std::shared_ptr<array_t> arr = seq.as_array();
			for (size_t i = 0; i < arr->size(); ++i) {
				const flow f = step((*arr)[i]);
				if (f == flow::brk) { break; }
				if (f == flow::ret) { return f; }
			}
			return flow::normal;
		}
		if (seq.is_string()) {
			for (const char c : seq.as_string()) {
				const flow f = step(value{std::string(1, c)});
				if (f == flow::brk) { break; }
				if (f == flow::ret) { return f; }
			}
			return flow::normal;
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
	static flow run(const env_ptr & env, context & cx, value & ret) {
		return exec_all<Ss...>(env, cx, ret);
	}
	static constexpr bool is_default = false;
};
template <typename... Ss> struct clause_match<default_clause<Ss...>> {
	static bool matches(const env_ptr &, context &, const value &) { return false; }
	static flow run(const env_ptr & env, context & cx, value & ret) {
		return exec_all<Ss...>(env, cx, ret);
	}
	static constexpr bool is_default = true;
};
template <typename D, typename... Clauses> struct exec_<switch_stmt<D, Clauses...>> {
	static flow go(const env_ptr & env, context & cx, value & ret) {
		const value disc = ev<D>(env, cx);
		auto scope = std::make_shared<environment>();
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
		if (f == flow::brk) { f = flow::normal; }
		return f;
	}
};

template <typename E> struct exec_<return_stmt<E>> {
	static flow go(const env_ptr & env, context & cx, value & ret) {
		if constexpr (std::is_void_v<E>) {
			ret = value{};
		} else {
			ret = ev<E>(env, cx);
		}
		return flow::ret;
	}
};
template <> struct exec_<break_stmt> {
	static flow go(const env_ptr &, context &, value &) { return flow::brk; }
};
template <> struct exec_<continue_stmt> {
	static flow go(const env_ptr &, context &, value &) { return flow::cont; }
};
template <> struct exec_<empty_stmt> {
	static flow go(const env_ptr &, context &, value &) { return flow::normal; }
};

template <typename E> struct exec_<throw_stmt<E>> {
	static flow go(const env_ptr & env, context & cx, value &) {
		throw js_throw{ev<E>(env, cx)};
	}
};

template <typename Body, typename CatchName, typename Handler, typename Finally>
struct exec_<try_stmt<Body, CatchName, Handler, Finally>> {
	static flow go(const env_ptr & env, context & cx, value & ret) {
		flow f = flow::normal;
		bool rethrow = false;
		js_throw pending{};
		try {
			f = exec_<Body>::go(env, cx, ret);
		} catch (js_throw & t) {
			if constexpr (!std::is_void_v<Handler>) {
				auto scope = std::make_shared<environment>();
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
	static void go(environment &) { }
};
// hoist every name a declarator BINDS (destructuring binds several)
template <typename D> struct hoist_decl_names {
	static void go(environment & fnscope) {
		const auto one = [&](std::string_view n) {
			if (fnscope.vars.find(n) == fnscope.vars.end()) { fnscope.declare(n, value{}); }
		};
		one(decl_name<D>::view());
	}
};
template <typename Init, typename... Names>
struct hoist_decl_names<destr_array<Init, Names...>> {
	static void go(environment & fnscope) {
		const auto one = [&](std::string_view n) {
			if (fnscope.vars.find(n) == fnscope.vars.end()) { fnscope.declare(n, value{}); }
		};
		(one(Names::view()), ...);
	}
};
template <typename Init, typename... Props>
struct hoist_decl_names<destr_object<Init, Props...>> {
	static void go(environment & fnscope) {
		const auto one = [&](std::string_view n) {
			if (fnscope.vars.find(n) == fnscope.vars.end()) { fnscope.declare(n, value{}); }
		};
		(one(dprop_key<Props>::bind()), ...);
	}
};
template <typename... Ds> struct hoist_vars<var_stmt<Ds...>> {
	static void go(environment & fnscope) {
		(hoist_decl_names<Ds>::go(fnscope), ...);
	}
};
template <typename... Ss> struct hoist_vars<block<Ss...>> {
	static void go(environment & fnscope) { (hoist_vars<Ss>::go(fnscope), ...); }
};
template <typename C, typename T, typename E> struct hoist_vars<if_stmt<C, T, E>> {
	static void go(environment & fnscope) {
		hoist_vars<T>::go(fnscope);
		if constexpr (!std::is_void_v<E>) { hoist_vars<E>::go(fnscope); }
	}
};
template <typename C, typename B> struct hoist_vars<while_stmt<C, B>> {
	static void go(environment & fnscope) { hoist_vars<B>::go(fnscope); }
};
template <typename B, typename C> struct hoist_vars<do_stmt<B, C>> {
	static void go(environment & fnscope) { hoist_vars<B>::go(fnscope); }
};
template <typename Init, typename Cond, typename Step, typename B>
struct hoist_vars<for_stmt<Init, Cond, Step, B>> {
	static void go(environment & fnscope) {
		if constexpr (!std::is_void_v<Init>) { hoist_vars<Init>::go(fnscope); }
		hoist_vars<B>::go(fnscope);
	}
};
template <typename N, typename Iter, typename B> struct hoist_vars<forof_stmt<N, Iter, B>> {
	static void go(environment & fnscope) { hoist_vars<B>::go(fnscope); }
};
template <typename Clause> struct clause_vars;
template <typename E, typename... Ss> struct clause_vars<case_clause<E, Ss...>> {
	static void go(environment & fnscope) { (hoist_vars<Ss>::go(fnscope), ...); }
};
template <typename... Ss> struct clause_vars<default_clause<Ss...>> {
	static void go(environment & fnscope) { (hoist_vars<Ss>::go(fnscope), ...); }
};
template <typename D, typename... Cs> struct hoist_vars<switch_stmt<D, Cs...>> {
	static void go(environment & fnscope) { (clause_vars<Cs>::go(fnscope), ...); }
};
template <typename B, typename CN, typename H, typename F>
struct hoist_vars<try_stmt<B, CN, H, F>> {
	static void go(environment & fnscope) {
		hoist_vars<B>::go(fnscope);
		if constexpr (!std::is_void_v<H>) { hoist_vars<H>::go(fnscope); }
		if constexpr (!std::is_void_v<F>) { hoist_vars<F>::go(fnscope); }
	}
};

template <typename S> struct hoist_pick {
	static void go(const env_ptr &, context &) { }
};
template <typename N, typename P, typename B> struct hoist_pick<fn_decl<N, P, B>> {
	static void go(const env_ptr & env, context &) {
		env->declare(N::view(), fn_maker<P, B, false>::make(env, std::string{N::view()}));
	}
};
template <typename... Ds> struct hoist_pick<let_stmt<Ds...>> {
	static void go(const env_ptr & env, context &) {
		(env->tdz.insert(std::string{decl_name<Ds>::view()}), ...);
	}
};
template <typename... Ds> struct hoist_pick<const_stmt<Ds...>> {
	static void go(const env_ptr & env, context &) {
		(env->tdz.insert(std::string{decl_name<Ds>::view()}), ...);
	}
};
template <typename... Ss> void hoist_functions(const env_ptr & env, context & cx) {
	environment & fnscope = env->hoist_target();
	(hoist_vars<Ss>::go(fnscope), ...);
	(hoist_pick<Ss>::go(env, cx), ...);
}

template <typename... Ss> flow exec_all(const env_ptr & env, context & cx, value & ret) {
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
