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
// Semantics notes (v0.1): `var` behaves like `let` (block-scoped, no
// hoisting); `const` does not reject reassignment; classic `for` uses
// one binding for the whole loop (for-of binds per iteration); there
// is no `this` - object properties holding functions are callable but
// see only their closure. All documented in the README.

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
		if (const value * slot = env->find(Text::view())) { return *slot; }
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

template <typename... Es> struct eval_<array_lit<Es...>> {
	static value go(const env_ptr & env, context & cx) {
		array_t out;
		out.reserve(sizeof...(Es));
		(out.push_back(ev<Es>(env, cx)), ...);
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

template <typename Fn, typename... Args> struct eval_<call<Fn, Args...>> {
	static value go(const env_ptr & env, context & cx) {
		std::vector<value> args;
		args.reserve(sizeof...(Args));
		const value fn = ev<Fn>(env, cx);
		(args.push_back(ev<Args>(env, cx)), ...);
		return call_value(cx, fn, std::move(args));
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
		if (value * slot = env->find(T::view())) {
			*slot = std::move(v);
			return;
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

template <typename Params> struct param_binder;
template <typename... Names> struct param_binder<plist<Names...>> {
	static void bind(const env_ptr & local, const std::vector<value> & args) {
		size_t i = 0;
		((local->declare(Names::view(), i < args.size() ? args[i] : value{}), ++i), ...);
	}
};

template <typename Params, typename Body, bool ExprBody> struct fn_maker {
	static value make(const env_ptr & env, std::string name) {
		return value::function(
		    [env](context & cx, const std::vector<value> & args) -> value {
			    auto local = std::make_shared<environment>();
			    local->parent = env;
			    param_binder<Params>::bind(local, args);
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

template <typename... Ds> struct declare_all;
template <> struct declare_all<> {
	static void go(const env_ptr &, context &) { }
};
template <typename N, typename Init, typename... Rest>
struct declare_all<declarator<N, Init>, Rest...> {
	static void go(const env_ptr & env, context & cx) {
		if constexpr (std::is_void_v<Init>) {
			env->declare(N::view(), value{});
		} else {
			env->declare(N::view(), ev<Init>(env, cx));
		}
		declare_all<Rest...>::go(env, cx);
	}
};

template <typename... Ds> struct exec_<let_stmt<Ds...>> {
	static flow go(const env_ptr & env, context & cx, value &) {
		declare_all<Ds...>::go(env, cx);
		return flow::normal;
	}
};
template <typename... Ds> struct exec_<const_stmt<Ds...>> {
	static flow go(const env_ptr & env, context & cx, value &) {
		declare_all<Ds...>::go(env, cx);
		return flow::normal;
	}
};
template <typename... Ds> struct exec_<var_stmt<Ds...>> {
	static flow go(const env_ptr & env, context & cx, value &) {
		declare_all<Ds...>::go(env, cx);
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

template <typename Init, typename Cond, typename Step, typename B>
struct exec_<for_stmt<Init, Cond, Step, B>> {
	static flow go(const env_ptr & env, context & cx, value & ret) {
		auto scope = std::make_shared<environment>();
		scope->parent = env;
		if constexpr (!std::is_void_v<Init>) {
			value ignored;
			(void)exec_<Init>::go(scope, cx, ignored);
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

// --- suites: hoist function declarations, then run in order

template <typename S> void hoist_one(const env_ptr & env, context & cx) {
	(void)env;
	(void)cx;
}
template <typename S> struct hoist_pick {
	static void go(const env_ptr &, context &) { }
};
template <typename N, typename P, typename B> struct hoist_pick<fn_decl<N, P, B>> {
	static void go(const env_ptr & env, context &) {
		env->declare(N::view(), fn_maker<P, B, false>::make(env, std::string{N::view()}));
	}
};
template <typename... Ss> void hoist_functions(const env_ptr & env, context & cx) {
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
