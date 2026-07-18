#ifndef CTJS__BUILTINS__HPP
#define CTJS__BUILTINS__HPP

#include "value.hpp"
#ifndef CTJS_IN_A_MODULE
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#endif

// The runtime library: property access on every value kind (array and
// string methods appear as native functions bound to their receiver,
// so `arr.push` is itself a first-class value), the global namespaces
// (console, Math, JSON), and the global functions (parseInt, String,
// Number, ...). The interpreter funnels ALL member reads through
// get_member and all calls through call_value, so there is no special
// casing anywhere else.

namespace ctjs {

inline value call_value(context & cx, const value & fn, std::vector<value> args) {
	if (!fn.is_function()) {
		throw_error("TypeError", fn.to_string() + " is not a function");
	}
	if (cx.depth >= cx.max_depth) {
		throw_error("RangeError", "Maximum call stack size exceeded");
	}
	++cx.depth;
	value out;
	try {
		out = fn.as_function()->fn(cx, args);
	} catch (...) {
		--cx.depth;
		throw;
	}
	--cx.depth;
	return out;
}

namespace detail {

inline value arg_or_undefined(const std::vector<value> & a, size_t i) {
	return i < a.size() ? a[i] : value{};
}

// JS ToIntegerOrInfinity + relative index clamping for slice()
inline size_t rel_index(double d, size_t len) {
	if (std::isnan(d)) { return 0; }
	if (d < 0) {
		d += static_cast<double>(len);
		if (d < 0) { return 0; }
	}
	if (d > static_cast<double>(len)) { return len; }
	return static_cast<size_t>(d);
}

// console.log renders arrays/objects like node does, not via ToString
inline std::string inspect(const value & v);

inline std::string inspect_parts(const value & v) {
	if (v.is_string()) { // quoted inside containers
		std::string out = "'";
		out += v.as_string();
		out += '\'';
		return out;
	}
	return inspect(v);
}

inline std::string inspect(const value & v) {
	if (v.is_array()) {
		std::string out = "[ ";
		bool first = true;
		for (const value & e : *v.as_array()) {
			if (!first) { out += ", "; }
			first = false;
			out += inspect_parts(e);
		}
		out += " ]";
		return v.as_array()->empty() ? "[]" : out;
	}
	if (v.is_object()) {
		const object_t & o = *v.as_object();
		if (o.props.empty()) { return "{}"; }
		std::string out = "{ ";
		bool first = true;
		for (const auto & [k, pv] : o.props) {
			if (!first) { out += ", "; }
			first = false;
			out += k;
			out += ": ";
			out += inspect_parts(pv);
		}
		out += " }";
		return out;
	}
	if (v.is_function()) {
		const std::string & n = v.as_function()->name;
		return n.empty() ? "[Function (anonymous)]" : "[Function: " + n + "]";
	}
	return v.to_string();
}

// JSON.stringify (no replacer/indent in v0.1); undefined/functions are
// omitted in objects and become null in arrays, like the spec
inline bool json_piece(const value & v, std::string & out) {
	if (v.is_undefined() || v.is_function()) { return false; }
	if (v.is_null()) { out += "null"; return true; }
	if (v.is_bool()) { out += v.as_bool() ? "true" : "false"; return true; }
	if (v.is_number()) {
		const double d = v.as_number();
		if (std::isnan(d) || std::isinf(d)) { out += "null"; } else { out += number_to_string(d); }
		return true;
	}
	if (v.is_string()) {
		out += '"';
		for (const char c : v.as_string()) {
			switch (c) {
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\t': out += "\\t"; break;
				case '\r': out += "\\r"; break;
				case '\b': out += "\\b"; break;
				case '\f': out += "\\f"; break;
				default:
					if (static_cast<unsigned char>(c) < 0x20) {
						char buf[8];
						std::snprintf(buf, sizeof(buf), "\\u%04x", c);
						out += buf;
					} else {
						out += c;
					}
			}
		}
		out += '"';
		return true;
	}
	if (v.is_array()) {
		out += '[';
		bool first = true;
		for (const value & e : *v.as_array()) {
			if (!first) { out += ','; }
			first = false;
			if (!json_piece(e, out)) { out += "null"; }
		}
		out += ']';
		return true;
	}
	out += '{';
	bool first = true;
	for (const auto & [k, pv] : v.as_object()->props) {
		std::string piece;
		if (!json_piece(pv, piece)) { continue; }
		if (!first) { out += ','; }
		first = false;
		std::string kv;
		json_piece(value{k}, kv);
		out += kv;
		out += ':';
		out += piece;
	}
	out += '}';
	return true;
}

} // namespace detail

// --- property access on every kind of receiver

inline value get_member(context & cx, const value & recv, std::string_view name);

namespace detail {

inline value bound(std::string name, native_fn fn) {
	return value::function(std::move(fn), std::move(name));
}

inline value array_member(const value & recv, std::string_view name) {
	const std::shared_ptr<array_t> arr = recv.as_array();
	if (name == "length") { return value{arr->size()}; }
	if (name == "push") {
		return bound("push", [arr](context &, const std::vector<value> & a) {
			for (const value & v : a) { arr->push_back(v); }
			return value{arr->size()};
		});
	}
	if (name == "pop") {
		return bound("pop", [arr](context &, const std::vector<value> &) {
			if (arr->empty()) { return value{}; }
			value v = arr->back();
			arr->pop_back();
			return v;
		});
	}
	if (name == "shift") {
		return bound("shift", [arr](context &, const std::vector<value> &) {
			if (arr->empty()) { return value{}; }
			value v = arr->front();
			arr->erase(arr->begin());
			return v;
		});
	}
	if (name == "unshift") {
		return bound("unshift", [arr](context &, const std::vector<value> & a) {
			arr->insert(arr->begin(), a.begin(), a.end());
			return value{arr->size()};
		});
	}
	if (name == "slice") {
		return bound("slice", [arr](context &, const std::vector<value> & a) {
			const size_t len = arr->size();
			const size_t from = a.empty() ? 0 : rel_index(a[0].to_number(), len);
			const size_t to = a.size() < 2 || a[1].is_undefined()
			                      ? len
			                      : rel_index(a[1].to_number(), len);
			array_t out;
			for (size_t i = from; i < to; ++i) { out.push_back((*arr)[i]); }
			return value::array(std::move(out));
		});
	}
	if (name == "indexOf") {
		return bound("indexOf", [arr](context &, const std::vector<value> & a) {
			const value target = arg_or_undefined(a, 0);
			for (size_t i = 0; i < arr->size(); ++i) {
				if (strict_equals((*arr)[i], target)) { return value{static_cast<double>(i)}; }
			}
			return value{-1.0};
		});
	}
	if (name == "includes") {
		return bound("includes", [arr](context &, const std::vector<value> & a) {
			const value target = arg_or_undefined(a, 0);
			for (const value & e : *arr) {
				if (strict_equals(e, target)) { return value{true}; }
			}
			return value{false};
		});
	}
	if (name == "join") {
		return bound("join", [arr](context &, const std::vector<value> & a) {
			const std::string sep =
			    a.empty() || a[0].is_undefined() ? "," : a[0].to_string();
			std::string out;
			bool first = true;
			for (const value & e : *arr) {
				if (!first) { out += sep; }
				first = false;
				if (!e.is_nullish()) { out += e.to_string(); }
			}
			return value{out};
		});
	}
	if (name == "reverse") {
		return bound("reverse", [arr, recv](context &, const std::vector<value> &) {
			std::reverse(arr->begin(), arr->end());
			return recv;
		});
	}
	if (name == "concat") {
		return bound("concat", [arr](context &, const std::vector<value> & a) {
			array_t out = *arr;
			for (const value & v : a) {
				if (v.is_array()) {
					out.insert(out.end(), v.as_array()->begin(), v.as_array()->end());
				} else {
					out.push_back(v);
				}
			}
			return value::array(std::move(out));
		});
	}
	if (name == "map") {
		return bound("map", [arr](context & c, const std::vector<value> & a) {
			array_t out;
			for (size_t i = 0; i < arr->size(); ++i) {
				out.push_back(call_value(c, arg_or_undefined(a, 0),
				                         {(*arr)[i], value{static_cast<double>(i)}}));
			}
			return value::array(std::move(out));
		});
	}
	if (name == "filter") {
		return bound("filter", [arr](context & c, const std::vector<value> & a) {
			array_t out;
			for (size_t i = 0; i < arr->size(); ++i) {
				if (call_value(c, arg_or_undefined(a, 0),
				               {(*arr)[i], value{static_cast<double>(i)}})
				        .truthy()) {
					out.push_back((*arr)[i]);
				}
			}
			return value::array(std::move(out));
		});
	}
	if (name == "forEach") {
		return bound("forEach", [arr](context & c, const std::vector<value> & a) {
			for (size_t i = 0; i < arr->size(); ++i) {
				call_value(c, arg_or_undefined(a, 0),
				           {(*arr)[i], value{static_cast<double>(i)}});
			}
			return value{};
		});
	}
	if (name == "reduce") {
		return bound("reduce", [arr](context & c, const std::vector<value> & a) {
			size_t i = 0;
			value acc;
			if (a.size() >= 2) {
				acc = a[1];
			} else {
				if (arr->empty()) {
					throw_error("TypeError", "Reduce of empty array with no initial value");
				}
				acc = (*arr)[0];
				i = 1;
			}
			for (; i < arr->size(); ++i) {
				acc = call_value(c, arg_or_undefined(a, 0),
				                 {acc, (*arr)[i], value{static_cast<double>(i)}});
			}
			return acc;
		});
	}
	return value{};
}

inline value string_member(const value & recv, std::string_view name) {
	const std::string s = recv.as_string();
	if (name == "length") { return value{s.size()}; }
	if (name == "slice") {
		return bound("slice", [s](context &, const std::vector<value> & a) {
			const size_t len = s.size();
			const size_t from = a.empty() ? 0 : rel_index(a[0].to_number(), len);
			const size_t to = a.size() < 2 || a[1].is_undefined()
			                      ? len
			                      : rel_index(a[1].to_number(), len);
			return value{from < to ? s.substr(from, to - from) : std::string{}};
		});
	}
	if (name == "indexOf") {
		return bound("indexOf", [s](context &, const std::vector<value> & a) {
			const auto pos = s.find(arg_or_undefined(a, 0).to_string());
			return value{pos == std::string::npos ? -1.0 : static_cast<double>(pos)};
		});
	}
	if (name == "includes") {
		return bound("includes", [s](context &, const std::vector<value> & a) {
			return value{s.find(arg_or_undefined(a, 0).to_string()) != std::string::npos};
		});
	}
	if (name == "startsWith") {
		return bound("startsWith", [s](context &, const std::vector<value> & a) {
			return value{s.rfind(arg_or_undefined(a, 0).to_string(), 0) == 0};
		});
	}
	if (name == "endsWith") {
		return bound("endsWith", [s](context &, const std::vector<value> & a) {
			const std::string t = arg_or_undefined(a, 0).to_string();
			return value{s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0};
		});
	}
	if (name == "toUpperCase") {
		return bound("toUpperCase", [s](context &, const std::vector<value> &) {
			std::string out = s;
			for (char & c : out) {
				if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 'a' + 'A'); }
			}
			return value{out};
		});
	}
	if (name == "toLowerCase") {
		return bound("toLowerCase", [s](context &, const std::vector<value> &) {
			std::string out = s;
			for (char & c : out) {
				if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
			}
			return value{out};
		});
	}
	if (name == "trim") {
		return bound("trim", [s](context &, const std::vector<value> &) {
			std::string_view v = s;
			const auto blank = [](char c) {
				return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
			};
			while (!v.empty() && blank(v.front())) { v.remove_prefix(1); }
			while (!v.empty() && blank(v.back())) { v.remove_suffix(1); }
			return value{v};
		});
	}
	if (name == "split") {
		return bound("split", [s](context &, const std::vector<value> & a) {
			array_t out;
			if (a.empty() || a[0].is_undefined()) {
				out.push_back(value{s});
				return value::array(std::move(out));
			}
			const std::string sep = a[0].to_string();
			if (sep.empty()) {
				for (const char c : s) { out.push_back(value{std::string(1, c)}); }
				return value::array(std::move(out));
			}
			size_t at = 0;
			while (true) {
				const size_t hit = s.find(sep, at);
				if (hit == std::string::npos) {
					out.push_back(value{s.substr(at)});
					break;
				}
				out.push_back(value{s.substr(at, hit - at)});
				at = hit + sep.size();
			}
			return value::array(std::move(out));
		});
	}
	if (name == "charAt") {
		return bound("charAt", [s](context &, const std::vector<value> & a) {
			const double d = a.empty() ? 0 : a[0].to_number();
			if (std::isnan(d) || d < 0 || d >= static_cast<double>(s.size())) {
				return value{std::string{}};
			}
			return value{std::string(1, s[static_cast<size_t>(d)])};
		});
	}
	if (name == "charCodeAt") {
		return bound("charCodeAt", [s](context &, const std::vector<value> & a) {
			const double d = a.empty() ? 0 : a[0].to_number();
			if (std::isnan(d) || d < 0 || d >= static_cast<double>(s.size())) {
				return value{std::nan("")};
			}
			return value{static_cast<double>(
			    static_cast<unsigned char>(s[static_cast<size_t>(d)]))};
		});
	}
	if (name == "repeat") {
		return bound("repeat", [s](context &, const std::vector<value> & a) {
			const double d = a.empty() ? 0 : a[0].to_number();
			if (d < 0 || std::isinf(d)) { throw_error("RangeError", "Invalid count value"); }
			std::string out;
			for (size_t i = 0; i < static_cast<size_t>(d); ++i) { out += s; }
			return value{out};
		});
	}
	if (name == "replace") { // first occurrence, string pattern only
		return bound("replace", [s](context &, const std::vector<value> & a) {
			const std::string from = arg_or_undefined(a, 0).to_string();
			const std::string to = arg_or_undefined(a, 1).to_string();
			const size_t hit = s.find(from);
			if (hit == std::string::npos || from.empty()) { return value{s}; }
			std::string out = s;
			out.replace(hit, from.size(), to);
			return value{out};
		});
	}
	if (name == "replaceAll") {
		return bound("replaceAll", [s](context &, const std::vector<value> & a) {
			const std::string from = arg_or_undefined(a, 0).to_string();
			const std::string to = arg_or_undefined(a, 1).to_string();
			if (from.empty()) { return value{s}; }
			std::string out;
			size_t at = 0;
			while (true) {
				const size_t hit = s.find(from, at);
				if (hit == std::string::npos) {
					out += s.substr(at);
					break;
				}
				out += s.substr(at, hit - at);
				out += to;
				at = hit + from.size();
			}
			return value{out};
		});
	}
	if (name == "padStart" || name == "padEnd") {
		const bool at_start = name == "padStart";
		return bound(std::string{name}, [s, at_start](context &, const std::vector<value> & a) {
			const double want_d = a.empty() ? 0 : a[0].to_number();
			const size_t want =
			    std::isnan(want_d) || want_d < 0 ? 0 : static_cast<size_t>(want_d);
			const std::string pad =
			    a.size() < 2 || a[1].is_undefined() ? " " : a[1].to_string();
			std::string out = s;
			if (pad.empty()) { return value{out}; }
			std::string fill;
			while (out.size() + fill.size() < want) {
				fill += pad[(fill.size()) % pad.size()];
			}
			return value{at_start ? fill + out : out + fill};
		});
	}
	if (name == "toString") {
		return bound("toString", [s](context &, const std::vector<value> &) { return value{s}; });
	}
	return value{};
}

inline value number_member(const value & recv, std::string_view name) {
	const double d = recv.as_number();
	if (name == "toString") {
		return bound("toString", [d](context &, const std::vector<value> &) {
			return value{number_to_string(d)};
		});
	}
	if (name == "toFixed") {
		return bound("toFixed", [d](context &, const std::vector<value> & a) {
			const int digits = a.empty() ? 0 : static_cast<int>(a[0].to_number());
			if (digits < 0 || digits > 100) {
				throw_error("RangeError", "toFixed() digits argument must be between 0 and 100");
			}
			char buf[400];
			std::snprintf(buf, sizeof(buf), "%.*f", digits, d);
			return value{std::string{buf}};
		});
	}
	return value{};
}

} // namespace detail

inline value get_member(context &, const value & recv, std::string_view name) {
	if (recv.is_undefined() || recv.is_null()) {
		throw_error("TypeError", "Cannot read properties of " +
		                             std::string{recv.is_null() ? "null" : "undefined"} +
		                             " (reading '" + std::string{name} + "')");
	}
	if (recv.is_object()) {
		if (const value * v = recv.as_object()->find(name)) { return *v; }
		return value{};
	}
	if (recv.is_array()) { return detail::array_member(recv, name); }
	if (recv.is_string()) { return detail::string_member(recv, name); }
	if (recv.is_number()) { return detail::number_member(recv, name); }
	if (recv.is_function() && name == "name") { return value{recv.as_function()->name}; }
	return value{};
}

// obj[i] / obj["key"] - arrays take numeric indexes, strings index to
// one-char strings, objects key by ToString
inline value get_index(context & cx, const value & recv, const value & key) {
	if (recv.is_array() && key.is_number()) {
		const double d = key.as_number();
		const auto & arr = *recv.as_array();
		if (d < 0 || d >= static_cast<double>(arr.size()) ||
		    d != static_cast<double>(static_cast<size_t>(d))) {
			return value{};
		}
		return arr[static_cast<size_t>(d)];
	}
	if (recv.is_string() && key.is_number()) {
		const double d = key.as_number();
		const std::string & s = recv.as_string();
		if (d < 0 || d >= static_cast<double>(s.size()) ||
		    d != static_cast<double>(static_cast<size_t>(d))) {
			return value{};
		}
		return value{std::string(1, s[static_cast<size_t>(d)])};
	}
	return get_member(cx, recv, key.to_string());
}

inline void set_member(const value & recv, std::string_view name, value v) {
	if (recv.is_undefined() || recv.is_null()) {
		throw_error("TypeError", "Cannot set properties of " +
		                             std::string{recv.is_null() ? "null" : "undefined"} +
		                             " (setting '" + std::string{name} + "')");
	}
	if (recv.is_object()) {
		recv.as_object()->set(name, std::move(v));
		return;
	}
	if (recv.is_array() && name == "length") {
		recv.as_array()->resize(static_cast<size_t>(v.to_number()));
		return;
	}
	// numbers/strings: silent no-op, like non-strict JS
}

inline void set_index(const value & recv, const value & key, value v) {
	if (recv.is_array() && key.is_number()) {
		const double d = key.as_number();
		if (d >= 0 && d == static_cast<double>(static_cast<size_t>(d))) {
			auto & arr = *recv.as_array();
			const size_t i = static_cast<size_t>(d);
			if (i >= arr.size()) { arr.resize(i + 1); }
			arr[i] = std::move(v);
			return;
		}
	}
	set_member(recv, key.to_string(), std::move(v));
}

// --- the default global environment

namespace detail {

inline value make_console() {
	object_t console;
	console.set("log", value::function(
	                       [](context & cx, const std::vector<value> & a) {
		                       std::string line;
		                       bool first = true;
		                       for (const value & v : a) {
			                       if (!first) { line += ' '; }
			                       first = false;
			                       line += v.is_string() ? v.as_string() : inspect(v);
		                       }
		                       line += '\n';
		                       cx.write(line);
		                       return value{};
	                       },
	                       "log"));
	return value::object(std::move(console));
}

inline value math_fn(std::string name, double (*f)(double)) {
	return value::function(
	    [f](context &, const std::vector<value> & a) {
		    return value{f(arg_or_undefined(a, 0).to_number())};
	    },
	    std::move(name));
}

inline value make_math() {
	object_t math;
	math.set("floor", math_fn("floor", [](double d) { return std::floor(d); }));
	math.set("ceil", math_fn("ceil", [](double d) { return std::ceil(d); }));
	math.set("round", math_fn("round", [](double d) { return std::floor(d + 0.5); }));
	math.set("trunc", math_fn("trunc", [](double d) { return std::trunc(d); }));
	math.set("abs", math_fn("abs", [](double d) { return std::fabs(d); }));
	math.set("sqrt", math_fn("sqrt", [](double d) { return std::sqrt(d); }));
	math.set("sign", math_fn("sign", [](double d) {
		         return d > 0 ? 1.0 : d < 0 ? -1.0 : d;
	         }));
	// the trigonometry and friends a game loop lives on
	math.set("sin", math_fn("sin", [](double d) { return std::sin(d); }));
	math.set("cos", math_fn("cos", [](double d) { return std::cos(d); }));
	math.set("tan", math_fn("tan", [](double d) { return std::tan(d); }));
	math.set("asin", math_fn("asin", [](double d) { return std::asin(d); }));
	math.set("acos", math_fn("acos", [](double d) { return std::acos(d); }));
	math.set("atan", math_fn("atan", [](double d) { return std::atan(d); }));
	math.set("exp", math_fn("exp", [](double d) { return std::exp(d); }));
	math.set("log", math_fn("log", [](double d) { return std::log(d); }));
	math.set("log2", math_fn("log2", [](double d) { return std::log2(d); }));
	math.set("log10", math_fn("log10", [](double d) { return std::log10(d); }));
	math.set("cbrt", math_fn("cbrt", [](double d) { return std::cbrt(d); }));
	math.set("atan2", value::function(
	                      [](context &, const std::vector<value> & a) {
		                      return value{std::atan2(arg_or_undefined(a, 0).to_number(),
		                                              arg_or_undefined(a, 1).to_number())};
	                      },
	                      "atan2"));
	math.set("hypot", value::function(
	                      [](context &, const std::vector<value> & a) {
		                      double sum = 0;
		                      for (const value & v : a) {
			                      const double d = v.to_number();
			                      sum += d * d;
		                      }
		                      return value{std::sqrt(sum)};
	                      },
	                      "hypot"));
	math.set("pow", value::function(
	                    [](context &, const std::vector<value> & a) {
		                    return value{std::pow(arg_or_undefined(a, 0).to_number(),
		                                          arg_or_undefined(a, 1).to_number())};
	                    },
	                    "pow"));
	math.set("min", value::function(
	                    [](context &, const std::vector<value> & a) {
		                    double best = INFINITY;
		                    for (const value & v : a) {
			                    const double d = v.to_number();
			                    if (std::isnan(d)) { return value{d}; }
			                    best = std::min(best, d);
		                    }
		                    return value{best};
	                    },
	                    "min"));
	math.set("max", value::function(
	                    [](context &, const std::vector<value> & a) {
		                    double best = -INFINITY;
		                    for (const value & v : a) {
			                    const double d = v.to_number();
			                    if (std::isnan(d)) { return value{d}; }
			                    best = std::max(best, d);
		                    }
		                    return value{best};
	                    },
	                    "max"));
	math.set("random", value::function(
	                       [](context &, const std::vector<value> &) {
		                       static std::mt19937_64 rng{0xC7195u}; // deterministic seed
		                       return value{std::uniform_real_distribution<double>{0, 1}(rng)};
	                       },
	                       "random"));
	math.set("PI", value{3.141592653589793});
	math.set("E", value{2.718281828459045});
	return value::object(std::move(math));
}

inline value make_json() {
	object_t json;
	json.set("stringify", value::function(
	                          [](context &, const std::vector<value> & a) {
		                          std::string out;
		                          if (!json_piece(arg_or_undefined(a, 0), out)) {
			                          return value{};
		                          }
		                          return value{out};
	                          },
	                          "stringify"));
	return value::object(std::move(json));
}

} // namespace detail

inline env_ptr make_globals() {
	auto g = std::make_shared<environment>();
	g->declare("undefined", value{});
	g->declare("NaN", value{std::nan("")});
	g->declare("Infinity", value{INFINITY});
	g->declare("console", detail::make_console());
	g->declare("Math", detail::make_math());
	g->declare("JSON", detail::make_json());
	g->declare("parseInt", value::function(
	                           [](context &, const std::vector<value> & a) {
		                           std::string s = detail::arg_or_undefined(a, 0).to_string();
		                           const int radix =
		                               a.size() > 1 && !a[1].is_undefined()
		                                   ? static_cast<int>(a[1].to_number())
		                                   : 10;
		                           size_t at = 0;
		                           while (at < s.size() &&
		                                  (s[at] == ' ' || s[at] == '\t' || s[at] == '\n')) {
			                           ++at;
		                           }
		                           bool neg = false;
		                           if (at < s.size() && (s[at] == '+' || s[at] == '-')) {
			                           neg = s[at] == '-';
			                           ++at;
		                           }
		                           long long out = 0;
		                           size_t digits = 0;
		                           for (; at < s.size(); ++at) {
			                           int d = -1;
			                           const char c = s[at];
			                           if (c >= '0' && c <= '9') { d = c - '0'; }
			                           else if (c >= 'a' && c <= 'z') { d = c - 'a' + 10; }
			                           else if (c >= 'A' && c <= 'Z') { d = c - 'A' + 10; }
			                           if (d < 0 || d >= radix) { break; }
			                           out = out * radix + d;
			                           ++digits;
		                           }
		                           if (digits == 0) { return value{std::nan("")}; }
		                           return value{static_cast<double>(neg ? -out : out)};
	                           },
	                           "parseInt"));
	g->declare("parseFloat", value::function(
	                             [](context &, const std::vector<value> & a) {
		                             const std::string s =
		                                 detail::arg_or_undefined(a, 0).to_string();
		                             double d = 0;
		                             const auto r =
		                                 std::from_chars(s.data(), s.data() + s.size(), d);
		                             if (r.ptr == s.data()) { return value{std::nan("")}; }
		                             return value{d};
	                             },
	                             "parseFloat"));
	g->declare("isNaN", value::function(
	                        [](context &, const std::vector<value> & a) {
		                        return value{std::isnan(detail::arg_or_undefined(a, 0).to_number())};
	                        },
	                        "isNaN"));
	g->declare("isFinite", value::function(
	                           [](context &, const std::vector<value> & a) {
		                           return value{std::isfinite(
		                               detail::arg_or_undefined(a, 0).to_number())};
	                           },
	                           "isFinite"));
	g->declare("String", value::function(
	                         [](context &, const std::vector<value> & a) {
		                         return value{a.empty() ? std::string{} : a[0].to_string()};
	                         },
	                         "String"));
	g->declare("Number", value::function(
	                         [](context &, const std::vector<value> & a) {
		                         return value{a.empty() ? 0.0 : a[0].to_number()};
	                         },
	                         "Number"));
	g->declare("Boolean", value::function(
	                          [](context &, const std::vector<value> & a) {
		                          return value{!a.empty() && a[0].truthy()};
	                          },
	                          "Boolean"));
	object_t array_ns;
	array_ns.set("isArray", value::function(
	                            [](context &, const std::vector<value> & a) {
		                            return value{!a.empty() && a[0].is_array()};
	                            },
	                            "isArray"));
	g->declare("Array", value::object(std::move(array_ns)));
	return g;
}

} // namespace ctjs

#endif
