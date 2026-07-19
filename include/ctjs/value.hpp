#ifndef CTJS__VALUE__HPP
#define CTJS__VALUE__HPP

#ifndef CTJS_IN_A_MODULE
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#endif

// The RUNTIME value model. ctjs parses JavaScript at compile time (the
// AST is a type), but scripts execute at runtime, so values are
// ordinary dynamic data: numbers are IEEE-754 doubles, strings own
// their bytes, arrays/objects/functions have JS reference semantics
// via shared_ptr, and closures capture real environment chains.
//
// The JS semantics live here too: truthiness, ToNumber/ToString
// coercion, strict (===) and loose (==) equality, typeof, and the
// ECMA-262 Number::toString algorithm (shortest round-trip digits,
// fixed notation for exponents in (-7, 21), exponential outside).

namespace ctjs {

class value;
struct context;

using array_t = std::vector<value>;

// insertion-ordered property map, like a JS object
struct object_t {
	std::vector<std::pair<std::string, value>> props;

	value * find(std::string_view key);
	const value * find(std::string_view key) const;
	void set(std::string_view key, value v);
};

// one representation for JS functions and native (C++) functions: the
// closure environment, if any, is captured inside the std::function
using native_fn = std::function<value(context &, const std::vector<value> &)>;
struct function_t {
	native_fn fn;
	std::string name; // for typeof/printing; may be empty
};

struct undefined_t {
	bool operator==(const undefined_t &) const = default;
};
struct null_t {
	bool operator==(const null_t &) const = default;
};

// JS `throw` rides a C++ exception carrying the thrown value
struct js_throw;

namespace detail {

// ECMA-262 6.1.6.1.20 Number::toString(x, 10): shortest round-trip
// digits from to_chars, then fixed notation while the exponent is in
// (-7, 21), exponential ("1e+21", "1.5e-7") outside
inline std::string number_to_string(double x) {
	if (std::isnan(x)) { return "NaN"; }
	if (x == 0) { return "0"; } // -0 prints "0", like JS
	std::string out;
	if (x < 0) {
		out += '-';
		x = -x;
	}
	if (std::isinf(x)) {
		out += "Infinity";
		return out;
	}
	// shortest digits + decimal exponent via scientific to_chars
	char buf[40];
	const auto res = std::to_chars(buf, buf + sizeof(buf), x, std::chars_format::scientific);
	std::string_view sci{buf, static_cast<size_t>(res.ptr - buf)};
	// sci looks like "d.dddde±xx" or "de±xx"
	const size_t epos = sci.find('e');
	std::string digits;
	for (const char c : sci.substr(0, epos)) {
		if (c != '.') { digits += c; }
	}
	while (digits.size() > 1 && digits.back() == '0') { digits.pop_back(); }
	int exp10 = 0;
	// from_chars rejects the '+' to_chars writes into positive exponents
	const size_t esign = epos + 1 + (sci[epos + 1] == '+' ? 1 : 0);
	std::from_chars(sci.data() + esign, sci.data() + sci.size(), exp10);
	const int k = static_cast<int>(digits.size());
	const int n = exp10 + 1; // position of the decimal point
	if (k <= n && n <= 21) {
		out += digits;
		out.append(static_cast<size_t>(n - k), '0');
	} else if (0 < n && n <= 21) {
		out += digits.substr(0, static_cast<size_t>(n));
		out += '.';
		out += digits.substr(static_cast<size_t>(n));
	} else if (-6 < n && n <= 0) {
		out += "0.";
		out.append(static_cast<size_t>(-n), '0');
		out += digits;
	} else {
		out += digits.substr(0, 1);
		if (k > 1) {
			out += '.';
			out += digits.substr(1);
		}
		out += 'e';
		out += (n - 1 >= 0) ? '+' : '-';
		out += std::to_string(n - 1 >= 0 ? n - 1 : -(n - 1));
	}
	return out;
}

// JS ToNumber for strings: trimmed; "" -> 0; decimal/hex; else NaN
inline double string_to_number(std::string_view s) {
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
		if (s.empty()) { return std::nan(""); }
	}
	if (s == "Infinity") { return neg ? -INFINITY : INFINITY; }
	double d = 0;
	if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		unsigned long long u = 0;
		const auto r = std::from_chars(s.data() + 2, s.data() + s.size(), u, 16);
		if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) { return std::nan(""); }
		d = static_cast<double>(u);
	} else {
		const auto r = std::from_chars(s.data(), s.data() + s.size(), d);
		if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) { return std::nan(""); }
	}
	return neg ? -d : d;
}

} // namespace detail

// --- the value

class value {
public:
	using storage = std::variant<undefined_t, null_t, bool, double, std::string,
	                             std::shared_ptr<array_t>, std::shared_ptr<object_t>,
	                             std::shared_ptr<function_t>>;

	constexpr value() noexcept = default; // undefined
	value(null_t) : v_(null_t{}) { }
	value(bool b) : v_(b) { }
	value(double d) : v_(d) { }
	value(int i) : v_(static_cast<double>(i)) { }
	value(long long i) : v_(static_cast<double>(i)) { }
	value(size_t i) : v_(static_cast<double>(i)) { }
	value(const char * s) : v_(std::string{s}) { }
	value(std::string s) : v_(std::move(s)) { }
	value(std::string_view s) : v_(std::string{s}) { }
	value(std::shared_ptr<array_t> a) : v_(std::move(a)) { }
	value(std::shared_ptr<object_t> o) : v_(std::move(o)) { }
	value(std::shared_ptr<function_t> f) : v_(std::move(f)) { }

	static value undefined() { return value{}; }
	static value null() { return value{null_t{}}; }
	static value array(array_t init = {}) {
		return value{std::make_shared<array_t>(std::move(init))};
	}
	static value object(object_t init = {}) {
		return value{std::make_shared<object_t>(std::move(init))};
	}
	static value function(native_fn fn, std::string name = {}) {
		return value{std::make_shared<function_t>(function_t{std::move(fn), std::move(name)})};
	}

	bool is_undefined() const { return std::holds_alternative<undefined_t>(v_); }
	bool is_null() const { return std::holds_alternative<null_t>(v_); }
	bool is_nullish() const { return is_undefined() || is_null(); }
	bool is_bool() const { return std::holds_alternative<bool>(v_); }
	bool is_number() const { return std::holds_alternative<double>(v_); }
	bool is_string() const { return std::holds_alternative<std::string>(v_); }
	bool is_array() const { return std::holds_alternative<std::shared_ptr<array_t>>(v_); }
	bool is_object() const { return std::holds_alternative<std::shared_ptr<object_t>>(v_); }
	bool is_function() const { return std::holds_alternative<std::shared_ptr<function_t>>(v_); }

	bool as_bool() const { return std::get<bool>(v_); }
	double as_number() const { return std::get<double>(v_); }
	const std::string & as_string() const { return std::get<std::string>(v_); }
	std::string & as_string() { return std::get<std::string>(v_); }
	const std::shared_ptr<array_t> & as_array() const {
		return std::get<std::shared_ptr<array_t>>(v_);
	}
	const std::shared_ptr<object_t> & as_object() const {
		return std::get<std::shared_ptr<object_t>>(v_);
	}
	const std::shared_ptr<function_t> & as_function() const {
		return std::get<std::shared_ptr<function_t>>(v_);
	}

	// --- JS coercions

	bool truthy() const {
		if (is_undefined() || is_null()) { return false; }
		if (is_bool()) { return as_bool(); }
		if (is_number()) { return as_number() != 0 && !std::isnan(as_number()); }
		if (is_string()) { return !as_string().empty(); }
		return true; // arrays, objects, functions
	}

	double to_number() const {
		if (is_number()) { return as_number(); }
		if (is_bool()) { return as_bool() ? 1 : 0; }
		if (is_null()) { return 0; }
		if (is_undefined()) { return std::nan(""); }
		if (is_string()) { return detail::string_to_number(as_string()); }
		if (is_array()) { return detail::string_to_number(to_string()); } // via ToPrimitive
		return std::nan(""); // objects, functions
	}

	// JS ToString (what `String(v)` and `+ ""` produce)
	std::string to_string() const {
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

	std::string_view typeof_string() const {
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
	friend bool strict_equals(const value & a, const value & b) {
		if (a.v_.index() != b.v_.index()) { return false; }
		if (a.is_number()) { return a.as_number() == b.as_number(); } // NaN != NaN
		if (a.is_array()) { return a.as_array() == b.as_array(); }
		if (a.is_object()) { return a.as_object() == b.as_object(); }
		if (a.is_function()) { return a.as_function() == b.as_function(); }
		return a.v_ == b.v_;
	}

	// ==: null == undefined; numeric coercion across number/string/bool;
	// reference identity for objects (object-to-primitive not modeled)
	friend bool loose_equals(const value & a, const value & b) {
		if (a.v_.index() == b.v_.index()) { return strict_equals(a, b); }
		if (a.is_nullish() && b.is_nullish()) { return true; }
		if (a.is_nullish() || b.is_nullish()) { return false; }
		const bool a_prim = a.is_number() || a.is_string() || a.is_bool();
		const bool b_prim = b.is_number() || b.is_string() || b.is_bool();
		if (a_prim && b_prim) {
			const double x = a.to_number();
			const double y = b.to_number();
			return x == y && !std::isnan(x) && !std::isnan(y);
		}
		// one side is a reference type: compare via its array-join string
		// only for arrays (JS ToPrimitive); plain objects never equal
		if (a.is_array() && b_prim) { return loose_equals(value{a.to_string()}, b); }
		if (b.is_array() && a_prim) { return loose_equals(a, value{b.to_string()}); }
		return false;
	}

	// C++-side drilling: obj["key"], arr[2] - misses yield undefined and
	// chain harmlessly (null-object style, like the family's views)
	value operator[](std::string_view key) const;
	value operator[](size_t i) const;

	// C++-side extraction: to<double>(), to<int>(), to<bool>(),
	// to<std::string>() - JS coercion rules apply
	template <typename T> T to() const {
		if constexpr (std::is_same_v<T, bool>) {
			return truthy();
		} else if constexpr (std::is_same_v<T, std::string>) {
			return to_string();
		} else {
			return static_cast<T>(to_number());
		}
	}

	const storage & raw() const { return v_; }

private:
	storage v_;
};

inline value value::operator[](std::string_view key) const {
	if (is_object()) {
		if (const value * v = as_object()->find(key)) { return *v; }
	}
	return value{};
}
inline value value::operator[](size_t i) const {
	if (is_array() && i < as_array()->size()) { return (*as_array())[i]; }
	return value{};
}

inline value * object_t::find(std::string_view key) {
	for (auto & [k, v] : props) {
		if (k == key) { return &v; }
	}
	return nullptr;
}
inline const value * object_t::find(std::string_view key) const {
	for (const auto & [k, v] : props) {
		if (k == key) { return &v; }
	}
	return nullptr;
}
inline void object_t::set(std::string_view key, value v) {
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

// helpers making the spec error shapes: { name, message } objects with
// the usual "TypeError: ..." rendering
inline value make_error(std::string_view name, std::string_view message) {
	object_t o;
	o.set("name", value{name});
	o.set("message", value{message});
	return value::object(std::move(o));
}
[[noreturn]] inline void throw_error(std::string_view name, std::string_view message) {
	throw js_throw{make_error(name, message)};
}

// the "TypeError: message" line for uncaught errors and console output
inline std::string error_to_string(const value & v) {
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
// their defining chain alive)

struct environment {
	std::shared_ptr<environment> parent;
	std::map<std::string, value, std::less<>> vars;
	// V8-faithful binding metadata: which names are const, which are in
	// their temporal dead zone (let/const hoisted but uninitialized),
	// and whether this scope is a var-hoisting boundary (function/global)
	std::set<std::string, std::less<>> consts;
	std::set<std::string, std::less<>> tdz;
	bool function_scope = false;

	value * find(std::string_view name) {
		for (environment * e = this; e != nullptr; e = e->parent.get()) {
			if (const auto it = e->vars.find(name); it != e->vars.end()) {
				return &it->second;
			}
		}
		return nullptr;
	}
	// like find, but stops with tdz_hit when the nearest declaration of
	// the name is still in its temporal dead zone (shadowing respected)
	value * find_checked(std::string_view name, bool & tdz_hit) {
		for (environment * e = this; e != nullptr; e = e->parent.get()) {
			if (const auto it = e->vars.find(name); it != e->vars.end()) {
				return &it->second;
			}
			if (e->tdz.contains(name)) {
				tdz_hit = true;
				return nullptr;
			}
		}
		return nullptr;
	}
	// the environment holding the nearest binding of name (nullptr = none)
	environment * owner(std::string_view name) {
		for (environment * e = this; e != nullptr; e = e->parent.get()) {
			if (e->vars.find(name) != e->vars.end()) { return e; }
		}
		return nullptr;
	}
	// nearest enclosing function/global scope: where `var` declarations land
	environment & hoist_target() {
		environment * e = this;
		while (!e->function_scope && e->parent != nullptr) { e = e->parent.get(); }
		return *e;
	}
	void declare(std::string_view name, value v) {
		tdz.erase(std::string{name}); // initialization ends the dead zone
		vars.insert_or_assign(std::string{name}, std::move(v));
	}
};
using env_ptr = std::shared_ptr<environment>;

// --- the execution context: console capture, recursion guard

struct context {
	std::string console;                          // captured console output
	std::function<void(std::string_view)> sink{}; // optional live sink
	value last;                                   // last expression-statement value
	// `this` plumbing: a method call parks the receiver in pending_this
	// just before call_value; call_value moves it into current_this for
	// exactly that call (fn_maker binds it as `this` in the callee env).
	// Plain calls see undefined - module/strict semantics, documented.
	value pending_this;
	value current_this;
	int depth = 0;
	int max_depth = 256;

	void write(std::string_view s) {
		console += s;
		if (sink) { sink(s); }
	}
};

} // namespace ctjs

#endif
