#ifndef CTJS__VINTERP__HPP
#define CTJS__VINTERP__HPP

#include "value.hpp"
#include "builtins.hpp"
#include "vparse.hpp"
#ifndef CTJS_IN_A_MODULE
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#endif

// Value tree-walking interpreter over vparse.hpp's flat value AST.
//
// The counterpart to the type-driven program_runner: instead of a template
// instantiation per node (which compiles the interpreter afresh for every
// script), this is ONE piece of code that walks the AST as data at RUNTIME.
// It reuses ctjs's runtime value machinery verbatim - `value`, `environment`,
// `context`, call_value, get_member/set_member, and make_globals - so host
// bindings, builtins and semantics are shared with the established path.
// Compiling a script therefore costs only a value-parse (see vparse.hpp);
// nothing about the program is baked into types.

namespace ctjs::vp {

using ctjs::value;
using ctjs::rc;
using ctjs::object_t;
using ctjs::array_t;
using ctjs::function_t;
using ctjs::environment;
using ctjs::env_ptr;
using ctjs::context;
using ctjs::js_throw;
namespace d = ctjs::detail;

enum class flow { normal, brk, cont, ret };

// a host-provided global (same shape as ctjs::binding; defined here so vinterp
// need not pull in script.hpp / the Earley grammar)
struct binding {
	std::string name;
	value v;
};

// numeric literal -> double (int / hex / float / exponent)
inline double num_lit(std::string_view s) {
	if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		unsigned long long u = 0;
		for (std::size_t i = 2; i < s.size(); ++i) {
			char c = s[i];
			unsigned dg = (c <= '9') ? static_cast<unsigned>(c - '0')
			                         : static_cast<unsigned>((c | 0x20) - 'a' + 10);
			u = u * 16 + dg;
		}
		return static_cast<double>(u);
	}
	return d::string_to_number(s);
}

// cook a quoted string literal: strip the quotes, resolve escapes
inline std::string cook_str(std::string_view raw) {
	std::string out;
	if (raw.size() < 2) { return out; }
	std::size_t i = 1, end = raw.size() - 1;
	while (i < end) {
		char c = raw[i];
		if (c != '\\') { out += c; ++i; continue; }
		char e = raw[i + 1]; i += 2;
		switch (e) {
		case 'n': out += '\n'; break;
		case 't': out += '\t'; break;
		case 'r': out += '\r'; break;
		case 'b': out += '\b'; break;
		case 'f': out += '\f'; break;
		case 'v': out += '\v'; break;
		case '0': out += '\0'; break;
		case '\n': break;   // line continuation
		default: out += e;
		}
	}
	return out;
}

inline std::int32_t to_i32(const value & v) {
	double n = v.to_number();
	if (std::isnan(n) || std::isinf(n)) { return 0; }
	return static_cast<std::int32_t>(static_cast<std::int64_t>(n));
}

struct vm {
	ast tree;                       // the owned program AST
	env_ptr globals;                // builtins + host bindings
	env_ptr scope;                  // the program's top-level scope (child of globals);
	                                // top-level declarations land here so a host can
	                                // call them back (onFrame, onClick, ...) after run()

	explicit vm(ast t) : tree(std::move(t)) {
		globals = ctjs::make_globals();
		scope = child_env(globals, true);
	}

	const node & N(int i) const { return tree.nodes[static_cast<std::size_t>(i)]; }
	int child(int list, int k) const { return tree.pool[static_cast<std::size_t>(list + k)]; }

	static env_ptr child_env(const env_ptr & parent, bool fn_scope = false) {
		env_ptr e = rc<environment>::make();
		e->parent = parent;
		e->function_scope = fn_scope;
		return e;
	}

	// ---- run the whole program --------------------------------------------
	value run(context & cx) {
		hoist(tree.root, scope, cx);
		value ret;
		exec(tree.root, scope, cx, ret);
		return cx.last;
	}

	// hoist function declarations (so `foo()` before `function foo(){}` works)
	void hoist(int blk, const env_ptr & env, context & cx) {
		const node & b = N(blk);
		for (int k = 0; k < b.list_len; ++k) {
			int si = child(b.list, k);
			if (N(si).kind == nk::func_decl) { env->declare(N(si).text, make_fn(si, env, cx)); }
		}
	}

	// ---- expressions -------------------------------------------------------
	value eval(int i, const env_ptr & env, context & cx) {
		const node & n = N(i);
		switch (n.kind) {
		case nk::num: return value{num_lit(n.text)};
		case nk::str: return value{cook_str(n.text)};
		case nk::tmpl: return eval_template(n.text, env, cx);
		case nk::regex: return eval_regex(n.text, cx);
		case nk::true_lit: return value{true};
		case nk::false_lit: return value{false};
		case nk::null_lit: return value::null();
		case nk::this_lit: return cx.current_this;
		case nk::ident: {
			bool tdz = false;
			value * slot = env->find_checked(n.text, tdz);
			if (slot) { return *slot; }
			ctjs::throw_error("ReferenceError", std::string{n.text} + " is not defined");
		}
		case nk::array: return eval_array(n, env, cx);
		case nk::object: return eval_object(n, env, cx);
		case nk::arrow:
		case nk::func_expr: return make_fn(i, env, cx);
		case nk::unary: return eval_unary(n, env, cx);
		case nk::update: return eval_update(n, env, cx);
		case nk::binary: return eval_binary(n.text, eval(n.a, env, cx), eval(n.b, env, cx));
		case nk::logical: return eval_logical(n, env, cx);
		case nk::assign: return eval_assign(n, env, cx);
		case nk::ternary: return eval(n.a, env, cx).truthy() ? eval(n.b, env, cx) : eval(n.c, env, cx);
		case nk::member: return ctjs::get_member(cx, eval(n.a, env, cx), n.text);
		case nk::index: {
			value obj = eval(n.a, env, cx);
			return get_index(obj, eval(n.b, env, cx), cx);
		}
		case nk::opt_member: {
			value obj = eval(n.a, env, cx);
			return obj.is_nullish() ? value{} : ctjs::get_member(cx, obj, n.text);
		}
		case nk::opt_index: {
			value obj = eval(n.a, env, cx);
			return obj.is_nullish() ? value{} : get_index(obj, eval(n.b, env, cx), cx);
		}
		case nk::call:
		case nk::opt_call: return eval_call(n, env, cx);
		case nk::new_expr: return eval_new(n, env, cx);
		case nk::seq: { eval(n.a, env, cx); return eval(n.b, env, cx); }
		default: return value{};
		}
	}

	// a[b] : arrays and strings index directly by integer; everything else
	// (incl. array METHODS like .length/.push) goes through get_member
	value get_index(const value & obj, const value & key, context & cx) {
		if (obj.is_array()) {
			double d0 = key.to_number();
			if (d0 >= 0 && d0 == static_cast<double>(static_cast<long long>(d0))) {
				std::size_t idx = static_cast<std::size_t>(d0);
				const rc<array_t> & arr = obj.as_array();
				return idx < arr->size() ? (*arr)[idx] : value{};
			}
		}
		if (obj.is_string()) {
			double d0 = key.to_number();
			if (d0 >= 0 && d0 == static_cast<double>(static_cast<long long>(d0))) {
				const std::string & s = obj.as_string();
				std::size_t idx = static_cast<std::size_t>(d0);
				return idx < s.size() ? value{std::string(1, s[idx])} : value{};
			}
		}
		return ctjs::get_member(cx, obj, key.to_string());
	}
	void set_index(const value & obj, const value & key, const value & v, context & cx) {
		if (obj.is_array()) {
			double d0 = key.to_number();
			if (d0 >= 0 && d0 == static_cast<double>(static_cast<long long>(d0))) {
				std::size_t idx = static_cast<std::size_t>(d0);
				const rc<array_t> & arr = obj.as_array();
				if (idx >= arr->size()) { arr->resize(idx + 1); }
				(*arr)[idx] = v;
				return;
			}
		}
		ctjs::set_member(cx, obj, key.to_string(), v);
	}

	value eval_array(const node & n, const env_ptr & env, context & cx) {
		array_t out;
		for (int k = 0; k < n.list_len; ++k) {
			int ei = child(n.list, k);
			if (N(ei).kind == nk::spread) {
				value s = eval(N(ei).a, env, cx);
				if (s.is_array()) { for (const value & e : *s.as_array()) { out.push_back(e); } }
			} else {
				out.push_back(eval(ei, env, cx));
			}
		}
		return value::array(std::move(out));
	}

	value eval_object(const node & n, const env_ptr & env, context & cx) {
		auto o = rc<object_t>::make();
		for (int k = 0; k < n.list_len; ++k) {
			int pi = child(n.list, k);
			const node & p = N(pi);
			if (p.kind == nk::spread) {
				value s = eval(p.a, env, cx);
				if (s.is_object()) { for (auto & [k2, v2] : s.as_object()->props) { o->set(k2, v2); } }
				continue;
			}
			std::string key = (p.d == 1) ? eval(p.a, env, cx).to_string() : std::string{p.text};
			value v;
			if (p.c == 1) { v = make_fn(p.b, env, cx); }         // method
			else if (p.c == 2) {                                 // shorthand { x }
				bool tdz = false; value * slot = env->find_checked(p.text, tdz);
				v = slot ? *slot : value{};
			} else { v = eval(p.b, env, cx); }                   // key: value
			o->set(key, std::move(v));
		}
		return value{o};
	}

	value eval_unary(const node & n, const env_ptr & env, context & cx) {
		std::string_view op = n.text;
		if (op == "typeof") {
			if (N(n.a).kind == nk::ident) {
				bool tdz = false; value * s = env->find_checked(N(n.a).text, tdz);
				if (!s) { return value{std::string{"undefined"}}; }
				return value{std::string{s->typeof_string()}};
			}
			return value{std::string{eval(n.a, env, cx).typeof_string()}};
		}
		if (op == "delete") {
			const node & t = N(n.a);
			if (t.kind == nk::member || t.kind == nk::index) {
				value obj = eval(t.a, env, cx);
				std::string key = (t.kind == nk::member) ? std::string{t.text} : eval(t.b, env, cx).to_string();
				if (obj.is_object()) {
					auto & ps = obj.as_object()->props;
					for (std::size_t j = 0; j < ps.size(); ++j) { if (ps[j].first == key) { ps.erase(ps.begin() + static_cast<std::ptrdiff_t>(j)); break; } }
				}
			}
			return value{true};
		}
		value v = eval(n.a, env, cx);
		if (op == "!") { return value{!v.truthy()}; }
		if (op == "-") { return value{-v.to_number()}; }
		if (op == "+") { return value{v.to_number()}; }
		if (op == "~") { return value{static_cast<double>(~to_i32(v))}; }
		if (op == "void") { return value{}; }
		if (op == "await") {
			// settled-promise subset: unwrap a fulfilled promise's value, or
			// re-throw a rejected one; a non-promise awaits to itself
			if (ctjs::is_promise(v)) {
				value st = ctjs::get_member(cx, v, "__state");
				value inner = ctjs::get_member(cx, v, "__value");
				if (st.to_string() == "rejected") { throw js_throw{inner}; }
				return inner;
			}
			return v;
		}
		return value{};
	}

	value eval_update(const node & n, const env_ptr & env, context & cx) {
		value old = eval(n.a, env, cx);
		double nv = old.to_number() + (n.text == "++" ? 1 : -1);
		store(n.a, value{nv}, env, cx);
		return value{n.b == 1 /*prefix*/ ? nv : old.to_number()};
	}

	value eval_logical(const node & n, const env_ptr & env, context & cx) {
		value l = eval(n.a, env, cx);
		if (n.text == "&&") { return l.truthy() ? eval(n.b, env, cx) : l; }
		if (n.text == "||") { return l.truthy() ? l : eval(n.b, env, cx); }
		return l.is_nullish() ? eval(n.b, env, cx) : l;   // ??
	}

	value eval_assign(const node & n, const env_ptr & env, context & cx) {
		if (n.text == "=") { value v = eval(n.b, env, cx); store(n.a, v, env, cx); return v; }
		// compound: x op= y
		value cur = eval(n.a, env, cx);
		std::string_view o = n.text;
		std::string bin = std::string{o.substr(0, o.size() - 1)}; // strip '='
		value rhs = eval(n.b, env, cx);
		value res;
		if (bin == "&&") { res = cur.truthy() ? rhs : cur; }
		else if (bin == "||") { res = cur.truthy() ? cur : rhs; }
		else if (bin == "??") { res = cur.is_nullish() ? rhs : cur; }
		else { res = eval_binary(bin, cur, rhs); }
		store(n.a, res, env, cx);
		return res;
	}

	// assign `v` into the lvalue at node `t`
	void store(int ti, const value & v, const env_ptr & env, context & cx) {
		const node & t = N(ti);
		if (t.kind == nk::ident) {
			environment * o = env->owner(t.text);
			if (o) {
				if (o->is_const(t.text)) { ctjs::throw_error("TypeError", "Assignment to constant variable."); }
				*o->local(t.text) = v;
			} else { globals->declare(t.text, v); }   // implicit global
			return;
		}
		if (t.kind == nk::member) { ctjs::set_member(cx, eval(t.a, env, cx), t.text, v); return; }
		if (t.kind == nk::index) {
			value obj = eval(t.a, env, cx);
			set_index(obj, eval(t.b, env, cx), v, cx);
			return;
		}
	}

	value eval_binary(std::string_view op, value a, value b) {
		if (op == "+") {
			bool sa = a.is_string() || a.is_array() || a.is_object();
			bool sb = b.is_string() || b.is_array() || b.is_object();
			if (sa || sb) { return value{a.to_string() + b.to_string()}; }
			return value{a.to_number() + b.to_number()};
		}
		if (op == "-") { return value{a.to_number() - b.to_number()}; }
		if (op == "*") { return value{a.to_number() * b.to_number()}; }
		if (op == "/") { return value{a.to_number() / b.to_number()}; }
		if (op == "%") { return value{std::fmod(a.to_number(), b.to_number())}; }
		if (op == "**") { return value{std::pow(a.to_number(), b.to_number())}; }
		if (op == "==") { return value{loose_equals(a, b)}; }
		if (op == "!=") { return value{!loose_equals(a, b)}; }
		if (op == "===") { return value{strict_equals(a, b)}; }
		if (op == "!==") { return value{!strict_equals(a, b)}; }
		if (op == "<" || op == ">" || op == "<=" || op == ">=") {
			if (a.is_string() && b.is_string()) {
				int c = a.as_string().compare(b.as_string());
				if (op == "<") { return value{c < 0}; }
				if (op == ">") { return value{c > 0}; }
				if (op == "<=") { return value{c <= 0}; }
				return value{c >= 0};
			}
			double x = a.to_number(), y = b.to_number();
			if (op == "<") { return value{x < y}; }
			if (op == ">") { return value{x > y}; }
			if (op == "<=") { return value{x <= y}; }
			return value{x >= y};
		}
		if (op == "&") { return value{static_cast<double>(to_i32(a) & to_i32(b))}; }
		if (op == "|") { return value{static_cast<double>(to_i32(a) | to_i32(b))}; }
		if (op == "^") { return value{static_cast<double>(to_i32(a) ^ to_i32(b))}; }
		if (op == "<<") { return value{static_cast<double>(to_i32(a) << (to_i32(b) & 31))}; }
		if (op == ">>") { return value{static_cast<double>(to_i32(a) >> (to_i32(b) & 31))}; }
		if (op == ">>>") { return value{static_cast<double>(static_cast<std::uint32_t>(to_i32(a)) >> (to_i32(b) & 31))}; }
		if (op == "instanceof") { return value{instance_of(a, b)}; }
		if (op == "in") { return value{b.is_object() && b.as_object()->find(a.to_string()) != nullptr}; }
		return value{};
	}

	static bool instance_of(const value & obj, const value & ctor) {
		if (!obj.is_object() || !ctor.is_function()) { return false; }
		const value * proto = nullptr;
		if (ctor.as_function()->props) { proto = ctor.as_function()->props->find("prototype"); }
		if (!proto || !proto->is_object()) { return false; }
		for (rc<object_t> p = obj.as_object()->proto; p; p = p->proto) {
			if (p == proto->as_object()) { return true; }
		}
		return false;
	}

	std::vector<value> eval_args(int list, int len, const env_ptr & env, context & cx) {
		std::vector<value> out;
		for (int k = 0; k < len; ++k) {
			int ai = child(list, k);
			if (N(ai).kind == nk::spread) {
				value s = eval(N(ai).a, env, cx);
				if (s.is_array()) { for (const value & e : *s.as_array()) { out.push_back(e); } }
			} else { out.push_back(eval(ai, env, cx)); }
		}
		return out;
	}

	value eval_call(const node & n, const env_ptr & env, context & cx) {
		const node & callee = N(n.a);
		value fn;
		value this_for_call; // the `this` to bind; committed to cx.pending_this AFTER args
		std::string cname;   // the callee's name, for a helpful "X is not a function"
		if (callee.kind == nk::super_lit) {
			// super(...) - call the parent constructor on the current instance
			fn = cx.current_super;
			this_for_call = cx.current_this;
			cname = "super";
		} else if ((callee.kind == nk::member || callee.kind == nk::opt_member) &&
		           N(callee.a).kind == nk::super_lit) {
			// super.method(...) - resolve on the parent prototype, run on `this`
			value proto;
			if (cx.current_super.is_function() && cx.current_super.as_function()->props) {
				if (const value * p = cx.current_super.as_function()->props->find("prototype")) { proto = *p; }
			}
			cname = std::string{callee.text};
			fn = ctjs::get_member(cx, proto, cname);
			this_for_call = cx.current_this;
		} else if (callee.kind == nk::member || callee.kind == nk::index || callee.kind == nk::opt_member) {
			value recv = eval(callee.a, env, cx);
			if ((n.kind == nk::opt_call || callee.kind == nk::opt_member) && recv.is_nullish()) { return value{}; }
			std::string key = (callee.kind == nk::index) ? eval(callee.b, env, cx).to_string() : std::string{callee.text};
			cname = key;
			fn = ctjs::get_member(cx, recv, key);
			if (n.kind == nk::opt_call && fn.is_nullish()) { return value{}; }
			this_for_call = recv;
		} else {
			if (callee.kind == nk::ident) { cname = std::string{callee.text}; }
			fn = eval(n.a, env, cx);
			if (n.kind == nk::opt_call && fn.is_nullish()) { return value{}; }
			this_for_call = value{};
		}
		// Evaluate args BEFORE binding `this`: an argument that is itself a call or
		// `new` (e.g. obj.method(new Foo())) sets cx.pending_this during its own
		// evaluation, which would otherwise clobber the receiver we resolved above.
		std::vector<value> args = eval_args(n.list, n.list_len, env, cx);
		if (!fn.is_function()) {
			ctjs::throw_error("TypeError", (cname.empty() ? fn.to_string() : cname) + " is not a function");
		}
		cx.pending_this = std::move(this_for_call);
		return ctjs::call_value(cx, fn, std::move(args));
	}

	value eval_new(const node & n, const env_ptr & env, context & cx) {
		value ctor = eval(n.a, env, cx);
		std::vector<value> args = (n.d == 1) ? eval_args(n.list, n.list_len, env, cx) : std::vector<value>{};
		if (!ctor.is_function()) { ctjs::throw_error("TypeError", ctor.to_string() + " is not a constructor"); }
		auto obj = rc<object_t>::make();
		if (ctor.as_function()->props) {
			if (const value * proto = ctor.as_function()->props->find("prototype")) {
				if (proto->is_object()) { obj->proto = proto->as_object(); }
			}
		}
		cx.pending_this = value{obj};
		value r = ctjs::call_value(cx, ctor, std::move(args));
		return r.is_object() ? r : value{obj};   // ctor may return its own object
	}

	value eval_template(std::string_view raw, const env_ptr & env, context & cx) {
		std::string out;
		std::size_t i = 1, end = raw.size() - 1;   // strip backticks
		while (i < end) {
			char c = raw[i];
			if (c == '\\' && i + 1 < end) { out += raw[i + 1]; i += 2; continue; }
			if (c == '$' && i + 1 < end && raw[i + 1] == '{') {
				std::size_t j = i + 2; int depth = 1;
				while (j < end && depth) { if (raw[j] == '{') { ++depth; } else if (raw[j] == '}') { --depth; } if (depth) { ++j; } }
				std::string_view expr = raw.substr(i + 2, j - (i + 2));
				ast sub = parse(std::string{"("} + std::string{expr} + std::string{")"});
				if (sub.ok && sub.root >= 0) {
					vm tmp{std::move(sub)};   // parse-only reuse of eval via a scratch walker sharing env is awkward;
					// evaluate in THIS vm by re-parsing into this tree would shift indices - instead eval on a fresh sub-vm
					// sharing the current environment & context:
					(void)tmp;
				}
				// Simpler: evaluate the sub-expression via a nested parse+walk on a temporary vm that shares env/cx.
				out += eval_subexpr(expr, env, cx).to_string();
				i = j + 1;
				continue;
			}
			out += c; ++i;
		}
		return value{out};
	}

	// parse+evaluate a standalone expression string against the current env/cx
	value eval_subexpr(std::string_view expr, const env_ptr & env, context & cx);

	value eval_regex(std::string_view raw, context & cx) {
		// build via the RegExp global if present, else a plain marker object
		std::size_t slash = raw.rfind('/');
		std::string body = std::string{raw.substr(1, slash - 1)};
		std::string flags = std::string{raw.substr(slash + 1)};
		if (const value * re = globals->find("RegExp")) {
			if (re->is_function()) {
				cx.pending_this = value{};
				return ctjs::call_value(cx, *re, {value{body}, value{flags}});
			}
		}
		auto o = rc<object_t>::make();
		o->set("source", value{body}); o->set("flags", value{flags});
		return value{o};
	}

	// ---- functions ---------------------------------------------------------
	value make_fn(int fn_node, const env_ptr & closure, context & cx) {
		const node & fn = N(fn_node);
		bool is_arrow = (fn.kind == nk::arrow);
		value lexical_this = cx.current_this;
		vm * self = this;
		ctjs::native_fn callable =
		    [self, fn_node, closure, is_arrow, lexical_this](context & c, const std::vector<value> & args) -> value {
			return self->call_user(fn_node, closure, args, c, is_arrow, lexical_this);
		};
		value f = value::function(std::move(callable), std::string{fn.text});
		// every function gets a fresh `.prototype` object so `new` works
		if (!f.as_function()->props) { f.as_function()->props = rc<object_t>::make(); }
		f.as_function()->props->set("prototype", value{rc<object_t>::make()});
		return f;
	}

	value call_user(int fn_node, const env_ptr & closure, const std::vector<value> & args,
	                context & cx, bool is_arrow, const value & lexical_this) {
		const node & fn = N(fn_node);
		env_ptr local = child_env(closure, true);
		if (is_arrow) { cx.current_this = lexical_this; }   // arrows: lexical `this`
		bind_params(fn.list, fn.list_len, args, local, cx);
		// `arguments`
		if (!is_arrow) {
			array_t ar(args.begin(), args.end());
			local->declare("arguments", value::array(std::move(ar)));
		}
		const node & body = N(fn.a);
		// `async` functions/methods/arrows (fn.c == 1) return a settled promise:
		// the completion value is wrapped (unless it already is one), and a throw
		// becomes a rejected promise - so `asyncFn().then(...)` works
		if (fn.c == 1) {
			try {
				value ret;
				if (body.kind != nk::block) {
					ret = eval(fn.a, local, cx);
				} else {
					hoist(fn.a, local, cx);
					if (exec(fn.a, local, cx, ret) != flow::ret) { ret = value{}; }
				}
				return ctjs::is_promise(ret) ? ret : ctjs::make_promise(ret, false);
			} catch (ctjs::js_throw & t) {
				return ctjs::make_promise(t.thrown, true);
			}
		}
		if (body.kind != nk::block) { return eval(fn.a, local, cx); }   // arrow expression body
		hoist(fn.a, local, cx);
		value ret;
		flow f = exec(fn.a, local, cx, ret);
		return (f == flow::ret) ? ret : value{};
	}

	void bind_params(int list, int len, const std::vector<value> & args, const env_ptr & env, context & cx) {
		for (int k = 0; k < len; ++k) {
			const node & p = N(child(list, k));
			if (p.d == 1) {   // rest
				array_t rest;
				for (std::size_t j = static_cast<std::size_t>(k); j < args.size(); ++j) { rest.push_back(args[j]); }
				env->declare(p.text, value::array(std::move(rest)));
				return;
			}
			value v = (static_cast<std::size_t>(k) < args.size()) ? args[static_cast<std::size_t>(k)] : value{};
			if (v.is_undefined() && p.a >= 0) { v = eval(p.a, env, cx); }   // default
			env->declare(p.text, std::move(v));
		}
	}

	value build_class(int ci, const env_ptr & env, context & cx) {
		const node & c = N(ci);
		auto proto = rc<object_t>::make();
		value super;
		if (c.a >= 0) {
			super = eval(c.a, env, cx);
			if (super.is_function() && super.as_function()->props) {
				if (const value * sp = super.as_function()->props->find("prototype")) {
					if (sp->is_object()) { proto->proto = sp->as_object(); }
				}
			}
		}
		int ctor_node = -1;
		std::vector<int> fields;
		for (int k = 0; k < c.list_len; ++k) {
			int mi = child(c.list, k);
			const node & m = N(mi);
			bool is_static = (m.d & 1);
			if (m.c == 1) {   // method
				if (m.text == "constructor") { ctor_node = m.b; continue; }
				value fnv = make_fn(m.b, env, cx);
				if (is_static) { /* attached after ctor built */ }
				else { proto->set(std::string{m.text}, std::move(fnv)); }
			} else if (m.c == 0) {   // field
				if (!is_static) { fields.push_back(mi); }
			} else if (m.c == 2 && !is_static) {   // instance getter/setter
				ctjs::attach_accessor(*proto, m.text, (m.d & 4) ? 's' : 'g', make_fn(m.b, env, cx));
			}
		}
		// constructor function
		vm * self = this;
		env_ptr closure = env;
		int cn = ctor_node;
		value protov{proto};
		std::vector<int> fset = fields;
		value superv = super;
		ctjs::native_fn ctor = [self, cn, closure, protov, fset, superv](context & c2, const std::vector<value> & args) -> value {
			value thisv = c2.current_this;
			// NB: the instance prototype is set by eval_new to the ACTUAL class's
			// prototype; we must NOT reset it here, or a super(...) call (which
			// runs the parent ctor on the same instance) would clobber it.
			(void)protov;
			// initialise instance fields
			for (int fi : fset) {
				const node & f = self->N(fi);
				value fv = (f.b >= 0) ? self->eval(f.b, closure, c2) : value{};
				if (thisv.is_object()) { thisv.as_object()->set(std::string{f.text}, std::move(fv)); }
			}
			if (cn >= 0) {
				// the ctor body may call super(...) - make the parent ctor reachable
				value saved_super = c2.current_super;
				c2.current_super = superv;
				(void)self->call_user(cn, closure, args, c2, false, thisv);
				c2.current_super = saved_super;
			} else if (superv.is_function()) {
				// implicit constructor: forward args to the parent
				c2.pending_this = thisv;
				(void)ctjs::call_value(c2, superv, args);
			}
			return thisv;
		};
		value ctorv = value::function(std::move(ctor), std::string{c.text});
		if (!ctorv.as_function()->props) { ctorv.as_function()->props = rc<object_t>::make(); }
		ctorv.as_function()->props->set("prototype", protov);
		// static members
		for (int k = 0; k < c.list_len; ++k) {
			int mi = child(c.list, k);
			const node & m = N(mi);
			if (!(m.d & 1)) { continue; }
			if (m.c == 1) { ctorv.as_function()->props->set(std::string{m.text}, make_fn(m.b, env, cx)); }
			else if (m.c == 0) { ctorv.as_function()->props->set(std::string{m.text}, m.b >= 0 ? eval(m.b, env, cx) : value{}); }
			else if (m.c == 2) { ctjs::attach_accessor(*ctorv.as_function()->props, m.text, (m.d & 4) ? 's' : 'g', make_fn(m.b, env, cx)); }
		}
		return ctorv;
	}

	// ---- statements --------------------------------------------------------
	flow exec_block(const node & b, const env_ptr & env, context & cx, value & ret) {
		for (int k = 0; k < b.list_len; ++k) {
			flow f = exec(child(b.list, k), env, cx, ret);
			if (f != flow::normal) { return f; }
		}
		return flow::normal;
	}

	flow exec(int i, const env_ptr & env, context & cx, value & ret) {
		const node & n = N(i);
		switch (n.kind) {
		case nk::program: return exec_block(n, env, cx, ret);
		case nk::block: { env_ptr be = child_env(env); return exec_block(n, be, cx, ret); }
		case nk::empty: return flow::normal;
		case nk::expr_stmt: cx.last = eval(n.a, env, cx); return flow::normal;
		case nk::var_decl: {
			for (int k = 0; k < n.list_len; ++k) {
				const node & dcl = N(child(n.list, k));
				value v = (dcl.a >= 0) ? eval(dcl.a, env, cx) : value{};
				env->declare(dcl.text, std::move(v));
				if (n.text == "const") { env->consts.push_back(std::string{dcl.text}); }
			}
			return flow::normal;
		}
		case nk::func_decl: env->declare(n.text, make_fn(i, env, cx)); return flow::normal;
		case nk::class_decl: env->declare(n.text, build_class(i, env, cx)); return flow::normal;
		case nk::if_stmt:
			if (eval(n.a, env, cx).truthy()) { return exec(n.b, env, cx, ret); }
			else if (n.c >= 0) { return exec(n.c, env, cx, ret); }
			return flow::normal;
		case nk::while_stmt:
			while (eval(n.a, env, cx).truthy()) {
				flow f = exec(n.b, env, cx, ret);
				if (f == flow::brk) { break; }
				if (f == flow::ret) { return f; }
			}
			return flow::normal;
		case nk::do_stmt:
			do {
				flow f = exec(n.a, env, cx, ret);
				if (f == flow::brk) { break; }
				if (f == flow::ret) { return f; }
			} while (eval(n.b, env, cx).truthy());
			return flow::normal;
		case nk::for_stmt: return exec_for(n, env, cx, ret);
		case nk::forof_stmt: return exec_forof(n, env, cx, ret);
		case nk::return_stmt: ret = (n.a >= 0) ? eval(n.a, env, cx) : value{}; return flow::ret;
		case nk::break_stmt: return flow::brk;
		case nk::continue_stmt: return flow::cont;
		case nk::throw_stmt: throw js_throw{eval(n.a, env, cx)};
		case nk::try_stmt: return exec_try(n, env, cx, ret);
		case nk::switch_stmt: return exec_switch(n, env, cx, ret);
		case nk::labeled: return exec(n.a, env, cx, ret);   // labels ignored (best-effort)
		default: cx.last = eval(i, env, cx); return flow::normal;
		}
	}

	flow exec_for(const node & n, const env_ptr & env, context & cx, value & ret) {
		env_ptr fe = child_env(env);
		if (n.a >= 0) { value tmp; exec(n.a, fe, cx, tmp); }
		while (n.b < 0 || eval(n.b, fe, cx).truthy()) {
			flow f = exec(n.d, fe, cx, ret);
			if (f == flow::brk) { break; }
			if (f == flow::ret) { return f; }
			if (n.c >= 0) { eval(n.c, fe, cx); }
		}
		return flow::normal;
	}

	flow exec_forof(const node & n, const env_ptr & env, context & cx, value & ret) {
		value iter = eval(n.b, env, cx);
		std::vector<value> items;
		if (n.text == "of") {
			if (iter.is_array()) { items = *iter.as_array(); }
			else if (iter.is_string()) { for (char ch : iter.as_string()) { items.push_back(value{std::string(1, ch)}); } }
		} else {   // for-in: keys
			if (iter.is_object()) { for (auto & [k, v] : iter.as_object()->props) { (void)v; items.push_back(value{k}); } }
			else if (iter.is_array()) { for (std::size_t j = 0; j < iter.as_array()->size(); ++j) { items.push_back(value{d::number_to_string(static_cast<double>(j))}); } }
		}
		const node & decl = N(n.a);
		for (value & it : items) {
			env_ptr be = child_env(env);
			be->declare(decl.text, it);
			flow f = exec(n.c, be, cx, ret);
			if (f == flow::brk) { break; }
			if (f == flow::ret) { return f; }
		}
		return flow::normal;
	}

	flow exec_try(const node & n, const env_ptr & env, context & cx, value & ret) {
		flow f = flow::normal;
		try {
			f = exec(n.a, env, cx, ret);
		} catch (js_throw & ex) {
			if (n.b >= 0) {
				const node & cc = N(n.b);
				env_ptr ce = child_env(env);
				if (!cc.text.empty()) { ce->declare(cc.text, ex.thrown); }
				f = exec(cc.a, ce, cx, ret);
			} else if (n.c >= 0) {
				value fret; exec(n.c, env, cx, fret);
				throw;
			} else { throw; }
		}
		if (n.c >= 0) { value fret; flow ff = exec(n.c, env, cx, fret); if (ff != flow::normal) { return ff; } }
		return f;
	}

	flow exec_switch(const node & n, const env_ptr & env, context & cx, value & ret) {
		value disc = eval(n.a, env, cx);
		env_ptr se = child_env(env);
		int start = -1, deflt = -1;
		for (int k = 0; k < n.list_len; ++k) {
			const node & cl = N(child(n.list, k));
			if (cl.d == 1) { deflt = k; }
			else if (strict_equals(disc, eval(cl.a, se, cx))) { start = k; break; }
		}
		if (start < 0) { start = deflt; }
		if (start < 0) { return flow::normal; }
		for (int k = start; k < n.list_len; ++k) {
			const node & cl = N(child(n.list, k));
			for (int j = 0; j < cl.list_len; ++j) {
				flow f = exec(child(cl.list, j), se, cx, ret);
				if (f == flow::brk) { return flow::normal; }
				if (f == flow::ret) { return f; }
			}
		}
		return flow::normal;
	}
};

inline value vm::eval_subexpr(std::string_view expr, const env_ptr & env, context & cx) {
	// Evaluate a template ${...} sub-expression by parsing it into ITS OWN vm
	// that borrows this environment/context. Function values it creates close
	// over that vm; for template interpolation (typically simple reads) that
	// is fine within the interpolation's lifetime.
	auto sub = std::make_shared<vm>(parse(std::string{expr} + ";"));
	const node & prog = sub->N(sub->tree.root);
	if (prog.list_len == 0) { return value{}; }
	const node & st = sub->N(sub->child(prog.list, 0));
	int e = (st.kind == nk::expr_stmt) ? st.a : sub->child(prog.list, 0);
	// share environment: swap sub's globals for our env chain during eval
	return sub->eval(e, env, cx);
}

// Run a program AST (from vp::parse) to completion, reusing ctjs globals +
// host bindings. Returns the vm (kept alive so script functions stay callable).
struct vrun_result {
	std::shared_ptr<vm> machine;
	std::shared_ptr<context> cx;
	bool ok = true;
	value error;
	std::string_view console() const { return cx->console; }
	value get(std::string_view name) const {
		if (const value * v = machine->scope->find(name)) { return *v; }
		return value{};
	}
};

inline vrun_result vrun(ast tree, std::vector<binding> host = {}) {
	vrun_result r;
	r.machine = std::make_shared<vm>(std::move(tree));
	r.cx = std::make_shared<context>();
	for (binding & b : host) { r.machine->globals->declare(b.name, std::move(b.v)); }
	try {
		r.machine->run(*r.cx);
	} catch (js_throw & ex) {
		r.ok = false; r.error = ex.thrown;
	}
	return r;
}

inline vrun_result vrun(std::string_view src, std::vector<binding> host = {}) {
	return vrun(parse(src), std::move(host));
}

} // namespace ctjs::vp

#endif
