#ifndef CTJS__VALUE__HPP
#define CTJS__VALUE__HPP

#include "rc.hpp"
#include "ctc/cfunction.hpp"
#ifndef CTJS_IN_A_MODULE
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#endif

// The value model. ctjs parses JavaScript at compile time (the AST is a
// type) and executes it - AT RUNTIME OR AT COMPILE TIME. Everything here
// is therefore CONSTEXPR-capable: numbers are IEEE-754 doubles, strings
// own their bytes (std::string is constexpr in C++23), arrays/objects/
// functions have JS reference semantics via `rc` (a constexpr refcounted
// pointer, since std::shared_ptr is not constexpr), native functions live
// behind `cfunction` (a constexpr type-erased callable, since
// std::function is not), and scopes chain through `rc`. Constant-
// evaluable scripts run at compile time; anything that reaches a non-
// constexpr operation (Math via <cmath>, Date via <chrono>, a throw,
// host natives) falls back to the runtime interpreter.
//
// The JS semantics live here too: truthiness, ToNumber/ToString
// coercion, strict (===) and loose (==) equality, typeof, and the
// ECMA-262 Number::toString algorithm.

namespace ctjs {

class value;
struct context;
struct object_t;
struct function_t;
struct environment;

using array_t = std::vector<value>;

// Cycle-collector opt-in for the heap types (definitions of gc_trace/gc_clear are
// further down, once the types are complete). Declared HERE - before any
// rc<T>::make is instantiated - so `if constexpr (gc::participates<T>)` in
// rc::make always sees the true specialization and installs the collector hooks.
namespace gc {
template <> inline constexpr bool participates<object_t> = true;
template <> inline constexpr bool participates<array_t> = true;
template <> inline constexpr bool participates<function_t> = true;
template <> inline constexpr bool participates<environment> = true;
} // namespace gc

// insertion-ordered property map, like a JS object. `proto` is the
// [[Prototype]] link: get_member/set_member walk it, so class methods
// live once on a shared prototype instead of on every instance.
struct object_t {
	std::vector<std::pair<std::string, value>> props;
	rc<object_t> proto; // null for a plain {} / the chain root

	constexpr value * find(std::string_view key);        // OWN property only
	constexpr const value * find(std::string_view key) const;
	constexpr void set(std::string_view key, value v);   // OWN property only
};

// one representation for JS functions and native (C++) functions: the
// closure environment, if any, is captured inside the cfunction
using ctc::cfunction; // the shared constexpr callable now lives in ctc
using native_fn = cfunction<value(context &, const std::vector<value> &)>;
struct function_t {
	native_fn fn;
	std::string name; // for typeof/printing; may be empty
	// statics riding on the function value (Date.now, class statics);
	// null for the overwhelming majority that carry none
	rc<object_t> props;
	// RAW, non-owning headers mirroring the closure environment and lexical `this`
	// that the type-erased `fn` lambda captures STRONGLY. The collector traces
	// these so closure cycles (the dominant leak - a closure captured back onto an
	// object it reaches) become collectable. A raw gc::header* is used rather than
	// an rc member because environment/value are not complete here (value ->
	// function_t -> environment -> value is a completion cycle) and because the
	// single strong reference already lives in the lambda: this pointer just
	// mirrors that one edge for tracing. Set by make_fn; null for natives.
	gc::header * env_hdr = nullptr;
	gc::header * this_hdr = nullptr;
};

struct undefined_t {
	constexpr bool operator==(const undefined_t &) const = default;
};
struct null_t {
	constexpr bool operator==(const null_t &) const = default;
};

// JS `throw` rides a C++ exception carrying the thrown value
struct js_throw;

namespace detail {

// integer decimal string, exact and constexpr (used by the compile-time
// path and as the fast common case)
constexpr std::string int_to_string(std::int64_t v) {
	if (v == 0) { return "0"; }
	std::string out;
	const bool neg = v < 0;
	std::uint64_t u = neg ? static_cast<std::uint64_t>(-(v + 1)) + 1
	                           : static_cast<std::uint64_t>(v);
	while (u) {
		out.push_back(static_cast<char>('0' + u % 10));
		u /= 10;
	}
	if (neg) { out.push_back('-'); }
	for (std::size_t i = 0, j = out.size() - 1; i < j; ++i, --j) {
		const char t = out[i];
		out[i] = out[j];
		out[j] = t;
	}
	return out;
}

// ECMA-262 Number::toString(x, 10). At runtime: shortest round-trip
// digits via to_chars, fixed/exponential per the spec. At COMPILE time
// (if consteval): exact for integers; a non-integer number stringifies
// via a non-constexpr call, so a script that stringifies a fractional
// number simply isn't constant-evaluable and runs at runtime.
inline std::string number_to_string(double x) {
	if (std::isnan(x)) { return "NaN"; }
	if (x == 0) { return "0"; } // -0 prints "0", like JS
	if (std::isinf(x)) { return x < 0 ? std::string{"-Infinity"} : std::string{"Infinity"}; }
	if (x == static_cast<double>(static_cast<std::int64_t>(x))) {
		return int_to_string(static_cast<std::int64_t>(x)); // exact & constexpr
	}
	std::string out;
	double m = x;
	if (m < 0) {
		out += '-';
		m = -m;
	}
	// shortest digits + decimal exponent via scientific to_chars
	char buf[40];
	const auto res = std::to_chars(buf, buf + sizeof(buf), m, std::chars_format::scientific);
	std::string_view sci{buf, static_cast<std::size_t>(res.ptr - buf)};
	const std::size_t epos = sci.find('e');
	std::string digits;
	for (const char c : sci.substr(0, epos)) {
		if (c != '.') { digits += c; }
	}
	while (digits.size() > 1 && digits.back() == '0') { digits.pop_back(); }
	std::int32_t exp10 = 0;
	const std::size_t esign = epos + 1 + (sci[epos + 1] == '+' ? 1 : 0);
	std::from_chars(sci.data() + esign, sci.data() + sci.size(), exp10);
	const std::int32_t k = static_cast<std::int32_t>(digits.size());
	const std::int32_t n = exp10 + 1;
	if (k <= n && n <= 21) {
		out += digits;
		out.append(static_cast<std::size_t>(n - k), '0');
	} else if (0 < n && n <= 21) {
		out += digits.substr(0, static_cast<std::size_t>(n));
		out += '.';
		out += digits.substr(static_cast<std::size_t>(n));
	} else if (-6 < n && n <= 0) {
		out += "0.";
		out.append(static_cast<std::size_t>(-n), '0');
		out += digits;
	} else {
		out += digits.substr(0, 1);
		if (k > 1) {
			out += '.';
			out += digits.substr(1);
		}
		out += 'e';
		out += (n - 1 >= 0) ? '+' : '-';
		out += int_to_string(n - 1 >= 0 ? n - 1 : -(n - 1));
	}
	return out;
}

// JS ToNumber for strings: trimmed; "" -> 0; decimal/hex; else NaN
constexpr double string_to_number(std::string_view s) {
	const auto blank = [](char c) {
		return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
	};
	while (!s.empty() && blank(s.front())) { s.remove_prefix(1); }
	while (!s.empty() && blank(s.back())) { s.remove_suffix(1); }
	if (s.empty()) { return 0; }
	bool neg = false;
	if (s.front() == '+' || s.front() == '-') {
		neg = s.front() == '-';
		s.remove_prefix(1);
		if (s.empty()) { return std::numeric_limits<double>::quiet_NaN(); }
	}
	if (s == "Infinity") {
		return neg ? -std::numeric_limits<double>::infinity()
		           : std::numeric_limits<double>::infinity();
	}
	// hex integer: exact and constexpr
	if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		std::uint64_t u = 0;
		for (std::size_t i = 2; i < s.size(); ++i) {
			const char c = s[i];
			std::uint32_t d = 0;
			if (c >= '0' && c <= '9') { d = static_cast<std::uint32_t>(c - '0'); }
			else if (c >= 'a' && c <= 'f') { d = static_cast<std::uint32_t>(c - 'a' + 10); }
			else if (c >= 'A' && c <= 'F') { d = static_cast<std::uint32_t>(c - 'A' + 10); }
			else { return std::numeric_limits<double>::quiet_NaN(); }
			u = u * 16 + d;
		}
		return neg ? -static_cast<double>(u) : static_cast<double>(u);
	}
	// decimal integer: exact and constexpr; anything else (fraction /
	// exponent) parses with from_chars (non-constexpr -> runtime only)
	bool all_digits = !s.empty();
	for (const char c : s) {
		if (c < '0' || c > '9') { all_digits = false; break; }
	}
	if (all_digits) {
		std::uint64_t u = 0;
		for (const char c : s) { u = u * 10 + static_cast<std::uint32_t>(c - '0'); }
		return neg ? -static_cast<double>(u) : static_cast<double>(u);
	}
	double d = 0;
	const auto r = std::from_chars(s.data(), s.data() + s.size(), d);
	if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) {
		return std::numeric_limits<double>::quiet_NaN();
	}
	return neg ? -d : d;
}

} // namespace detail

// --- the value

class value {
public:
	using storage = std::variant<undefined_t, null_t, bool, double, std::string, rc<array_t>,
	                             rc<object_t>, rc<function_t>>;

	constexpr value() noexcept = default; // undefined
	constexpr value(null_t) : v_(null_t{}) { }
	constexpr value(bool b) : v_(b) { }
	constexpr value(double d) : v_(d) { }
	constexpr value(std::int32_t i) : v_(static_cast<double>(i)) { }
	constexpr value(std::int64_t i) : v_(static_cast<double>(i)) { }
	constexpr value(std::size_t i) : v_(static_cast<double>(i)) { }
	constexpr value(const char * s) : v_(std::string{s}) { }
	constexpr value(std::string s) : v_(std::move(s)) { }
	constexpr value(std::string_view s) : v_(std::string{s}) { }
	constexpr value(rc<array_t> a) : v_(std::move(a)) { }
	constexpr value(rc<object_t> o) : v_(std::move(o)) { }
	constexpr value(rc<function_t> f) : v_(std::move(f)) { }

	static constexpr value undefined() { return value{}; }
	static constexpr value null() { return value{null_t{}}; }
	static constexpr value array(array_t init = {}) {
		return value{rc<array_t>::make(std::move(init))};
	}
	static constexpr value object(object_t init = {}) {
		return value{rc<object_t>::make(std::move(init))};
	}
	static constexpr value function(native_fn fn, std::string name = {}) {
		return value{rc<function_t>::make(function_t{std::move(fn), std::move(name), rc<object_t>{}})};
	}

	constexpr bool is_undefined() const { return std::holds_alternative<undefined_t>(v_); }
	constexpr bool is_null() const { return std::holds_alternative<null_t>(v_); }
	constexpr bool is_nullish() const { return is_undefined() || is_null(); }
	constexpr bool is_bool() const { return std::holds_alternative<bool>(v_); }
	constexpr bool is_number() const { return std::holds_alternative<double>(v_); }
	constexpr bool is_string() const { return std::holds_alternative<std::string>(v_); }
	constexpr bool is_array() const { return std::holds_alternative<rc<array_t>>(v_); }
	constexpr bool is_object() const { return std::holds_alternative<rc<object_t>>(v_); }
	constexpr bool is_function() const { return std::holds_alternative<rc<function_t>>(v_); }

	constexpr bool as_bool() const { return std::get<bool>(v_); }
	constexpr double as_number() const { return std::get<double>(v_); }
	constexpr const std::string & as_string() const { return std::get<std::string>(v_); }
	constexpr std::string & as_string() { return std::get<std::string>(v_); }
	constexpr const rc<array_t> & as_array() const { return std::get<rc<array_t>>(v_); }
	constexpr const rc<object_t> & as_object() const { return std::get<rc<object_t>>(v_); }
	constexpr const rc<function_t> & as_function() const { return std::get<rc<function_t>>(v_); }

	// --- JS coercions

	constexpr bool truthy() const {
		if (is_undefined() || is_null()) { return false; }
		if (is_bool()) { return as_bool(); }
		if (is_number()) {
			const double n = as_number();
			return n != 0 && !(n != n);
		}
		if (is_string()) { return !as_string().empty(); }
		return true; // arrays, objects, functions
	}

	constexpr double to_number() const {
		if (is_number()) { return as_number(); }
		if (is_bool()) { return as_bool() ? 1 : 0; }
		if (is_null()) { return 0; }
		if (is_undefined()) { return std::numeric_limits<double>::quiet_NaN(); }
		if (is_string()) { return detail::string_to_number(as_string()); }
		if (is_array()) { return detail::string_to_number(to_string()); } // via ToPrimitive
		return std::numeric_limits<double>::quiet_NaN();                   // objects, functions
	}

	// JS ToString (what `String(v)` and `+ ""` produce)
	constexpr std::string to_string() const {
		if (is_undefined()) { return "undefined"; }
		if (is_null()) { return "null"; }
		if (is_bool()) { return as_bool() ? "true" : "false"; }
		if (is_number()) { return detail::number_to_string(as_number()); }
		if (is_string()) { return as_string(); }
		if (is_array()) { // Array.prototype.join(",") - nullish elements print empty
			std::string out;
			bool first = true;
			for (const value & e : *as_array()) {
				if (!first) { out += ','; }
				first = false;
				if (!e.is_nullish()) { out += e.to_string(); }
			}
			return out;
		}
		if (is_function()) {
			std::string n = as_function()->name;
			return "function " + n + "() { [native code] }";
		}
		return "[object Object]";
	}

	constexpr std::string_view typeof_string() const {
		if (is_undefined()) { return "undefined"; }
		if (is_null()) { return "object"; } // yes, really
		if (is_bool()) { return "boolean"; }
		if (is_number()) { return "number"; }
		if (is_string()) { return "string"; }
		if (is_function()) { return "function"; }
		return "object";
	}

	// --- equality

	// ===: same type and same value; objects by reference
	friend constexpr bool strict_equals(const value & a, const value & b) {
		if (a.v_.index() != b.v_.index()) { return false; }
		if (a.is_number()) { return a.as_number() == b.as_number(); } // NaN != NaN
		if (a.is_array()) { return a.as_array() == b.as_array(); }
		if (a.is_object()) { return a.as_object() == b.as_object(); }
		if (a.is_function()) { return a.as_function() == b.as_function(); }
		return a.v_ == b.v_;
	}

	// ==: null == undefined; numeric coercion across number/string/bool;
	// reference identity for objects (object-to-primitive not modeled)
	friend constexpr bool loose_equals(const value & a, const value & b) {
		if (a.v_.index() == b.v_.index()) { return strict_equals(a, b); }
		if (a.is_nullish() && b.is_nullish()) { return true; }
		if (a.is_nullish() || b.is_nullish()) { return false; }
		const bool a_prim = a.is_number() || a.is_string() || a.is_bool();
		const bool b_prim = b.is_number() || b.is_string() || b.is_bool();
		if (a_prim && b_prim) {
			const double x = a.to_number();
			const double y = b.to_number();
			return x == y && !(x != x) && !(y != y);
		}
		if (a.is_array() && b_prim) { return loose_equals(value{a.to_string()}, b); }
		if (b.is_array() && a_prim) { return loose_equals(a, value{b.to_string()}); }
		return false;
	}

	// C++-side drilling: obj["key"], arr[2] - misses yield undefined
	constexpr value operator[](std::string_view key) const;
	constexpr value operator[](std::size_t i) const;

	// C++-side extraction: to<double>(), to<std::int32_t>(), to<bool>(), to<std::string>()
	template <typename T> constexpr T to() const {
		if constexpr (std::is_same_v<T, bool>) {
			return truthy();
		} else if constexpr (std::is_same_v<T, std::string>) {
			return to_string();
		} else {
			return static_cast<T>(to_number());
		}
	}

	constexpr const storage & raw() const { return v_; }

private:
	storage v_;
};

constexpr value value::operator[](std::string_view key) const {
	if (is_object()) {
		if (const value * v = as_object()->find(key)) { return *v; }
	}
	return value{};
}
constexpr value value::operator[](std::size_t i) const {
	if (is_array() && i < as_array()->size()) { return (*as_array())[i]; }
	return value{};
}

constexpr value * object_t::find(std::string_view key) {
	for (auto & [k, v] : props) {
		if (k == key) { return &v; }
	}
	return nullptr;
}
constexpr const value * object_t::find(std::string_view key) const {
	for (const auto & [k, v] : props) {
		if (k == key) { return &v; }
	}
	return nullptr;
}
constexpr void object_t::set(std::string_view key, value v) {
	if (value * slot = find(key)) {
		*slot = std::move(v);
	} else {
		props.emplace_back(std::string{key}, std::move(v));
	}
}

// --- exceptions: JS throw as a C++ exception carrying the value

struct js_throw {
	value thrown;
};

// helpers making the spec error shapes: { name, message } objects
constexpr value make_error(std::string_view name, std::string_view message) {
	object_t o;
	o.set("name", value{name});
	o.set("message", value{message});
	return value::object(std::move(o));
}
[[noreturn]] inline void throw_error(std::string_view name, std::string_view message) {
	throw js_throw{make_error(name, message)};
}

// the "TypeError: message" line for uncaught errors and console output
constexpr std::string error_to_string(const value & v) {
	if (v.is_object()) {
		const object_t & o = *v.as_object();
		if (const value * n = o.find("name")) {
			std::string out = n->to_string();
			if (const value * m = o.find("message"); m && !m->to_string().empty()) {
				out += ": ";
				out += m->to_string();
			}
			return out;
		}
	}
	return v.to_string();
}

// --- environments: lexical scopes as a shared chain (closures keep
// their defining chain alive). vars/consts/tdz are flat vectors (not
// std::map/std::set, which are not constexpr); scopes are small.

struct environment {
	rc<environment> parent;
	std::vector<std::pair<std::string, value>> vars;
	std::vector<std::string> consts;
	std::vector<std::string> tdz; // let/const hoisted but uninitialized
	bool function_scope = false;

	constexpr value * local(std::string_view name) {
		for (auto & [k, v] : vars) {
			if (k == name) { return &v; }
		}
		return nullptr;
	}
	constexpr bool has_tdz(std::string_view name) const {
		for (const std::string & s : tdz) {
			if (s == name) { return true; }
		}
		return false;
	}
	constexpr bool is_const(std::string_view name) const {
		for (const std::string & s : consts) {
			if (s == name) { return true; }
		}
		return false;
	}

	constexpr value * find(std::string_view name) {
		for (environment * e = this; e != nullptr; e = e->parent.get()) {
			if (value * v = e->local(name)) { return v; }
		}
		return nullptr;
	}
	// like find, but stops with tdz_hit when the nearest declaration of
	// the name is still in its temporal dead zone (shadowing respected)
	constexpr value * find_checked(std::string_view name, bool & tdz_hit) {
		for (environment * e = this; e != nullptr; e = e->parent.get()) {
			if (value * v = e->local(name)) { return v; }
			if (e->has_tdz(name)) {
				tdz_hit = true;
				return nullptr;
			}
		}
		return nullptr;
	}
	// the environment holding the nearest binding of name (nullptr = none)
	constexpr environment * owner(std::string_view name) {
		for (environment * e = this; e != nullptr; e = e->parent.get()) {
			if (e->local(name) != nullptr) { return e; }
		}
		return nullptr;
	}
	// nearest enclosing function/global scope: where `var` declarations land
	constexpr environment & hoist_target() {
		environment * e = this;
		while (!e->function_scope && e->parent != nullptr) { e = e->parent.get(); }
		return *e;
	}
	constexpr void declare(std::string_view name, value v) {
		for (std::size_t i = 0; i < tdz.size(); ++i) {
			if (tdz[i] == name) {
				tdz.erase(tdz.begin() + static_cast<std::ptrdiff_t>(i));
				break;
			}
		}
		if (value * slot = local(name)) {
			*slot = std::move(v);
		} else {
			vars.emplace_back(std::string{name}, std::move(v));
		}
	}
};
using env_ptr = rc<environment>;

// --- cycle-collector participation (gc.hpp). Each collectable heap type opts in
// (participates<T> = true) and provides gc_trace (enumerate outgoing rc edges)
// and gc_clear (null every rc edge, for the collector's free phase). Everything
// is RUNTIME ONLY - the collector never runs during constant evaluation. These
// visit ONLY real, visible rc references; a reference sealed inside a native
// cfunction's captures is simply not visited, which keeps the collector
// CONSERVATIVE (it may miss such a cycle - a leak - but never frees a live
// object). User closures ARE visible: make_fn mirrors the lambda's captured
// environment + lexical `this` onto function_t (env_hdr/this_hdr), so
// closure<->object cycles collect too.
// These live in namespace `ctjs` (NOT ctjs::gc) so the type-erased thunks in
// gc.hpp reach them by argument-dependent lookup on the argument type (object_t
// et al. are in ctjs).
// the collector header for whatever heap object a value holds (null for a
// primitive). Used both to trace a value edge and to mirror a lambda capture.
inline gc::header * value_header(const value & v) {
	if (v.is_object()) { return v.as_object().gc_header(); }
	if (v.is_array()) { return v.as_array().gc_header(); }
	if (v.is_function()) { return v.as_function().gc_header(); }
	return nullptr;
}

inline void gc_visit_value(const value & v, void (*visit)(gc::header *)) {
	if (gc::header * h = value_header(v)) { visit(h); }
}

inline void gc_trace(object_t & o, void (*v)(gc::header *)) {
	for (auto & [k, val] : o.props) { gc_visit_value(val, v); }
	if (gc::header * h = o.proto.gc_header()) { v(h); }
}
inline void gc_clear(object_t & o) {
	o.props.clear();
	o.proto = nullptr;
}

inline void gc_trace(array_t & a, void (*v)(gc::header *)) {
	for (value & val : a) { gc_visit_value(val, v); }
}
inline void gc_clear(array_t & a) { a.clear(); }

inline void gc_trace(function_t & f, void (*v)(gc::header *)) {
	if (gc::header * h = f.props.gc_header()) { v(h); } // statics / .prototype
	if (f.env_hdr != nullptr) { v(f.env_hdr); }        // captured closure environment
	if (f.this_hdr != nullptr) { v(f.this_hdr); }      // captured lexical `this`
}
inline void gc_clear(function_t & f) {
	f.props = nullptr;
	f.env_hdr = nullptr;
	f.this_hdr = nullptr;
	f.fn = native_fn{}; // releases the env + lexical this captured inside the cfunction
}

inline void gc_trace(environment & e, void (*v)(gc::header *)) {
	if (gc::header * h = e.parent.gc_header()) { v(h); }
	for (auto & [k, val] : e.vars) { gc_visit_value(val, v); }
}
inline void gc_clear(environment & e) {
	e.parent = nullptr;
	e.vars.clear();
}

// --- the execution context: console capture, recursion guard

struct context {
	std::string console;                    // captured console output
	cfunction<void(std::string_view)> sink; // optional live sink (empty by default)
	value last;                             // last expression-statement value
	value pending_this;
	value current_this;
	value current_super; // the parent constructor while a subclass ctor/method runs
	std::vector<value> * gen_sink = nullptr;
	std::string flow_label;
	std::vector<std::string> pending_labels;
	std::vector<std::string> stack; // live call stack (function names) for traces
	std::int32_t depth = 0;
	std::int32_t max_depth = 256;

	constexpr void write(std::string_view s) {
		console += s;
		if (sink) { sink(s); }
	}
};

} // namespace ctjs

#endif
