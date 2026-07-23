#ifndef CTJS__BUILTINS__HPP
#define CTJS__BUILTINS__HPP

#include "value.hpp"
#ifndef CTJS_IN_A_MODULE
#include <cstddef>
#include <cstdint>
#include <memory>
#include <functional>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <unordered_map>
#endif

// The runtime library: property access on every value kind (array and
// string methods appear as native functions bound to their receiver,
// so `arr.push` is itself a first-class value), the global namespaces
// (console, Math, JSON), and the global functions (parseInt, String,
// Number, ...). The interpreter funnels ALL member reads through
// get_member and all calls through call_value, so there is no special
// casing anywhere else.

namespace ctjs {

inline constexpr value call_value(context & cx, const value & fn, std::vector<value> args) {
	if (!fn.is_function()) {
		throw_error("TypeError", fn.to_string() + " is not a function");
	}
	if (cx.depth >= cx.max_depth) {
		throw_error("RangeError", "Maximum call stack size exceeded");
	}
	// route the (possibly absent) method-call receiver into current_this
	// for exactly this call; restore the caller's `this` afterwards
	value saved_this = std::move(cx.current_this);
	cx.current_this = std::exchange(cx.pending_this, value{});
	++cx.depth;
	const std::string & fname = fn.as_function()->name;
	cx.stack.push_back(fname.empty() ? std::string{"<anonymous>"} : fname);
	value out;
	try {
		out = fn.as_function()->fn(cx, args);
	} catch (js_throw & t) {
		// attach a call-stack trace once, at the deepest frame that sees the
		// throw, so an uncaught error shows WHERE it came from
		if (t.thrown.is_object() && t.thrown.as_object()->find("__traced") == nullptr) {
			std::string tr = error_to_string(t.thrown);
			for (auto it = cx.stack.rbegin(); it != cx.stack.rend(); ++it) { tr += "\n    at " + *it; }
			t.thrown.as_object()->set("stack", value{tr});
			t.thrown.as_object()->set("__traced", value{true});
		}
		cx.stack.pop_back();
		--cx.depth;
		cx.current_this = std::move(saved_this);
		throw;
	} catch (...) {
		cx.stack.pop_back();
		--cx.depth;
		cx.current_this = std::move(saved_this);
		throw;
	}
	cx.stack.pop_back();
	cx.current_this = std::move(saved_this);
	--cx.depth;
	return out;
}

// --- promises, the SETTLED subset. The engine is synchronous and
// single-threaded: nothing can be pending, so a promise is an object
// that is already fulfilled or rejected at creation. `await` and
// then/catch/finally run handlers immediately instead of queueing
// microtasks - observable ordering differs from V8 only where code
// relies on deferral, which nothing synchronous can. Async hosts
// (compile-time-browser's fetch) resolve their work before the script
// ever sees the promise, so the subset is exact for them.

inline constexpr value make_promise(value settled, bool rejected);

inline constexpr bool is_promise(const value & v) {
	return v.is_object() && v.as_object()->find("__ctjs_promise") != nullptr;
}

namespace detail {

// handler result chaining: a returned promise adopts, a throw rejects
inline constexpr value promise_handler_result(context & cx, const value & handler, const value & input) {
	try {
		value r = call_value(cx, handler, {input});
		return is_promise(r) ? r : make_promise(std::move(r), false);
	} catch (const js_throw & ex) {
		return make_promise(ex.thrown, true);
	}
}

} // namespace detail

inline constexpr value make_promise(value settled, bool rejected) {
	auto o = rc<object_t>::make();
	o->set("__ctjs_promise", value{true});
	o->set("__state", value{rejected ? "rejected" : "fulfilled"});
	o->set("__value", settled);
	// a settled promise is immutable, so the methods capture the state
	// BY VALUE - no cycle through the object, nothing to invalidate
	const value stored = std::move(settled);
	o->set("then",
	       value::function(
	           [stored, rejected](context & cx, const std::vector<value> & a) -> value {
		           const value on_ok = a.size() > 0 ? a[0] : value{};
		           const value on_err = a.size() > 1 ? a[1] : value{};
		           if (!rejected) {
			           return on_ok.is_function()
			                      ? detail::promise_handler_result(cx, on_ok, stored)
			                      : make_promise(stored, false);
		           }
		           return on_err.is_function()
		                      ? detail::promise_handler_result(cx, on_err, stored)
		                      : make_promise(stored, true);
	           },
	           "then"));
	o->set("catch",
	       value::function(
	           [stored, rejected](context & cx, const std::vector<value> & a) -> value {
		           const value on_err = a.empty() ? value{} : a[0];
		           if (rejected && on_err.is_function()) {
			           return detail::promise_handler_result(cx, on_err, stored);
		           }
		           return make_promise(stored, rejected);
	           },
	           "catch"));
	o->set("finally",
	       value::function(
	           [stored, rejected](context & cx, const std::vector<value> & a) -> value {
		           const value fn = a.empty() ? value{} : a[0];
		           if (fn.is_function()) {
			           try {
				           (void)call_value(cx, fn, {});
			           } catch (const js_throw & ex) {
				           return make_promise(ex.thrown, true);
			           }
		           }
		           return make_promise(stored, rejected);
	           },
	           "finally"));
	return value{std::move(o)};
}

// `await v`: unwrap settled promises (rethrowing rejections as JS
// throws); every other value passes through, exactly like V8 awaiting
// a non-thenable
inline constexpr value await_value(value v) {
	while (is_promise(v)) {
		const rc<object_t> o = v.as_object();
		const value * state = o->find("__state");
		const value * stored = o->find("__value");
		value inner = stored != nullptr ? *stored : value{};
		if (state != nullptr && state->is_string() && state->as_string() == "rejected") {
			throw js_throw{std::move(inner)};
		}
		v = std::move(inner);
	}
	return v;
}

// --- accessors: { get x() {}, set x(v) {} } (object literals and
// classes) store a hidden marker object per property; get_member calls
// the getter with the receiver as `this`, set_member the setter.

inline constexpr bool is_accessor(const value & v) {
	return v.is_object() && v.as_object()->find("__accessor") != nullptr;
}

inline constexpr void attach_accessor(object_t & o, std::string_view name, char kind, value fn) {
	value * slot = o.find(name);
	if (slot == nullptr || !is_accessor(*slot)) {
		object_t acc;
		acc.set("__accessor", value{true});
		o.set(name, value::object(std::move(acc)));
		slot = o.find(name);
	}
	slot->as_object()->set(kind == 'g' ? "get" : "set", std::move(fn));
}

// --- regular expressions: a small backtracking engine behind regex
// literals (/ab+c/i), RegExp-object methods (test/exec) and the
// regex-aware string methods (match/replace/split). Features: literals,
// ., [] classes (ranges, negation, \d\w\s inside), \d\w\s\D\W\S, \b\B,
// ^ $ (m-aware), groups (capturing and (?:)), alternation, quantifiers
// * + ? {n} {n,} {n,m} with lazy '?', flags i g m. No lookaround, no
// backreferences, no named groups (documented).

namespace rxd {

struct rx_class {
	bool neg = false;
	std::vector<std::pair<unsigned char, unsigned char>> ranges;
};
struct rx_alt;
struct rx_piece {
	enum kind_t { lit, any, cls, grp, bol, eol, wordb, nwordb } kind = lit;
	char c = 0;
	rx_class cc;
	std::shared_ptr<rx_alt> sub;
	std::int32_t cap = -1; // capture slot, -1 = (?:)
	std::int32_t min = 1;
	std::int32_t max = 1; // -1 = unbounded
	bool greedy = true;
};
using rx_seq = std::vector<rx_piece>;
struct rx_alt {
	std::vector<rx_seq> alts;
};
struct rx_prog {
	std::shared_ptr<rx_alt> root;
	std::int32_t ngroups = 0;
	bool icase = false, global = false, multi = false;
};

[[noreturn]] inline void rx_fail(std::string_view src) {
	throw_error("SyntaxError", "Invalid regular expression: /" + std::string{src} + "/");
}

inline constexpr void rx_class_escape(rx_class & out, char e) {
	switch (e) {
	case 'd': out.ranges.push_back({'0', '9'}); break;
	case 'w':
		out.ranges.push_back({'a', 'z'});
		out.ranges.push_back({'A', 'Z'});
		out.ranges.push_back({'0', '9'});
		out.ranges.push_back({'_', '_'});
		break;
	case 's':
		out.ranges.push_back({' ', ' '});
		out.ranges.push_back({'\t', '\t'});
		out.ranges.push_back({'\n', '\n'});
		out.ranges.push_back({'\r', '\r'});
		out.ranges.push_back({'\f', '\f'});
		out.ranges.push_back({'\v', '\v'});
		break;
	default: break;
	}
}

inline char rx_escape_char(char e) {
	switch (e) {
	case 'n': return '\n';
	case 't': return '\t';
	case 'r': return '\r';
	case 'f': return '\f';
	case 'v': return '\v';
	case '0': return '\0';
	default: return e; // \. \/ \[ \\ etc: the char itself
	}
}

inline std::shared_ptr<rx_alt> rx_parse_alt(std::string_view src, std::size_t & i, rx_prog & p,
                                            bool top);

inline rx_piece rx_parse_atom(std::string_view src, std::size_t & i, rx_prog & p) {
	rx_piece pc;
	const char c = src[i];
	if (c == '(') {
		++i;
		pc.kind = rx_piece::grp;
		if (i + 1 < src.size() && src[i] == '?' && src[i + 1] == ':') {
			i += 2;
		} else {
			pc.cap = p.ngroups++;
		}
		pc.sub = rx_parse_alt(src, i, p, false);
		if (i >= src.size() || src[i] != ')') { rx_fail(src); }
		++i;
		return pc;
	}
	if (c == '[') {
		++i;
		pc.kind = rx_piece::cls;
		if (i < src.size() && src[i] == '^') {
			pc.cc.neg = true;
			++i;
		}
		bool first = true;
		while (i < src.size() && (src[i] != ']' || first)) {
			first = false;
			unsigned char lo;
			if (src[i] == '\\' && i + 1 < src.size()) {
				const char e = src[i + 1];
				i += 2;
				if (e == 'd' || e == 'w' || e == 's') {
					rx_class_escape(pc.cc, e);
					continue;
				}
				lo = static_cast<unsigned char>(rx_escape_char(e));
			} else {
				lo = static_cast<unsigned char>(src[i++]);
			}
			unsigned char hi = lo;
			if (i + 1 < src.size() && src[i] == '-' && src[i + 1] != ']') {
				++i;
				if (src[i] == '\\' && i + 1 < src.size()) {
					hi = static_cast<unsigned char>(rx_escape_char(src[i + 1]));
					i += 2;
				} else {
					hi = static_cast<unsigned char>(src[i++]);
				}
			}
			pc.cc.ranges.push_back({lo, hi});
		}
		if (i >= src.size()) { rx_fail(src); }
		++i; // ']'
		return pc;
	}
	if (c == '.') {
		++i;
		pc.kind = rx_piece::any;
		return pc;
	}
	if (c == '^') {
		++i;
		pc.kind = rx_piece::bol;
		return pc;
	}
	if (c == '$') {
		++i;
		pc.kind = rx_piece::eol;
		return pc;
	}
	if (c == '\\' && i + 1 < src.size()) {
		const char e = src[i + 1];
		i += 2;
		if (e == 'b') { pc.kind = rx_piece::wordb; return pc; }
		if (e == 'B') { pc.kind = rx_piece::nwordb; return pc; }
		if (e == 'd' || e == 'w' || e == 's') {
			pc.kind = rx_piece::cls;
			rx_class_escape(pc.cc, e);
			return pc;
		}
		if (e == 'D' || e == 'W' || e == 'S') {
			pc.kind = rx_piece::cls;
			pc.cc.neg = true;
			rx_class_escape(pc.cc, static_cast<char>(e + ('a' - 'A')));
			return pc;
		}
		pc.kind = rx_piece::lit;
		pc.c = rx_escape_char(e);
		return pc;
	}
	pc.kind = rx_piece::lit;
	pc.c = c;
	++i;
	return pc;
}

inline constexpr void rx_parse_quant(std::string_view src, std::size_t & i, rx_piece & pc) {
	if (i >= src.size()) { return; }
	const char c = src[i];
	if (c == '*') { pc.min = 0; pc.max = -1; ++i; }
	else if (c == '+') { pc.min = 1; pc.max = -1; ++i; }
	else if (c == '?') { pc.min = 0; pc.max = 1; ++i; }
	else if (c == '{') {
		std::size_t j = i + 1;
		std::int32_t lo = 0;
		bool has = false;
		while (j < src.size() && src[j] >= '0' && src[j] <= '9') {
			lo = lo * 10 + (src[j++] - '0');
			has = true;
		}
		if (!has) { return; } // literal '{'
		std::int32_t hi = lo;
		if (j < src.size() && src[j] == ',') {
			++j;
			if (j < src.size() && src[j] == '}') { hi = -1; }
			else {
				hi = 0;
				while (j < src.size() && src[j] >= '0' && src[j] <= '9') {
					hi = hi * 10 + (src[j++] - '0');
				}
			}
		}
		if (j >= src.size() || src[j] != '}') { return; }
		pc.min = lo;
		pc.max = hi;
		i = j + 1;
	} else {
		return;
	}
	if (i < src.size() && src[i] == '?') {
		pc.greedy = false;
		++i;
	}
}

inline std::shared_ptr<rx_alt> rx_parse_alt(std::string_view src, std::size_t & i, rx_prog & p,
                                            bool top) {
	auto out = std::make_shared<rx_alt>();
	out->alts.emplace_back();
	while (i < src.size()) {
		const char c = src[i];
		if (c == ')') {
			if (top) { rx_fail(src); }
			break;
		}
		if (c == '|') {
			++i;
			out->alts.emplace_back();
			continue;
		}
		rx_piece pc = rx_parse_atom(src, i, p);
		rx_parse_quant(src, i, pc);
		out->alts.back().push_back(std::move(pc));
	}
	return out;
}

inline rx_prog rx_compile(std::string_view source, std::string_view flags) {
	rx_prog p;
	for (const char f : flags) {
		if (f == 'i') { p.icase = true; }
		else if (f == 'g') { p.global = true; }
		else if (f == 'm') { p.multi = true; }
		else { throw_error("SyntaxError", "Invalid regular expression flags"); }
	}
	std::size_t i = 0;
	p.root = rx_parse_alt(source, i, p, true);
	if (i != source.size()) { rx_fail(source); }
	return p;
}

struct rx_state {
	const std::string * s = nullptr;
	const rx_prog * p = nullptr;
	std::vector<std::pair<std::ptrdiff_t, std::ptrdiff_t>> caps; // -1,-1 = unmatched
};

inline char rx_fold(char c, bool icase) {
	return icase && c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}
inline constexpr bool rx_is_word(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
	       c == '_';
}
inline constexpr bool rx_class_hit(const rx_class & cc, char ch, bool icase) {
	const auto in = [&](char probe) {
		for (const auto & [lo, hi] : cc.ranges) {
			if (static_cast<unsigned char>(probe) >= lo &&
			    static_cast<unsigned char>(probe) <= hi) {
				return true;
			}
		}
		return false;
	};
	bool hit = in(ch);
	if (!hit && icase) {
		const char other = (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - ('a' - 'A'))
		                   : (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + ('a' - 'A'))
		                                              : ch;
		hit = other != ch && in(other);
	}
	return cc.neg ? !hit : hit;
}

using rx_cont = std::function<bool(std::size_t)>;

inline constexpr bool rx_match_alt(const rx_alt & alt, rx_state & st, std::size_t pos, const rx_cont & k);

inline constexpr bool rx_match_once(const rx_piece & pc, rx_state & st, std::size_t pos, const rx_cont & k) {
	const std::string & s = *st.s;
	switch (pc.kind) {
	case rx_piece::lit:
		return pos < s.size() && rx_fold(s[pos], st.p->icase) == rx_fold(pc.c, st.p->icase) &&
		       k(pos + 1);
	case rx_piece::any:
		return pos < s.size() && s[pos] != '\n' && k(pos + 1);
	case rx_piece::cls:
		return pos < s.size() && rx_class_hit(pc.cc, s[pos], st.p->icase) && k(pos + 1);
	case rx_piece::bol:
		return (pos == 0 || (st.p->multi && s[pos - 1] == '\n')) && k(pos);
	case rx_piece::eol:
		return (pos == s.size() || (st.p->multi && s[pos] == '\n')) && k(pos);
	case rx_piece::wordb:
	case rx_piece::nwordb: {
		const bool before = pos > 0 && rx_is_word(s[pos - 1]);
		const bool after = pos < s.size() && rx_is_word(s[pos]);
		const bool boundary = before != after;
		return boundary == (pc.kind == rx_piece::wordb) && k(pos);
	}
	case rx_piece::grp: {
		const std::int32_t cap = pc.cap;
		const auto saved = cap >= 0 ? st.caps[static_cast<std::size_t>(cap)]
		                            : std::pair<std::ptrdiff_t, std::ptrdiff_t>{-1, -1};
		const bool ok = rx_match_alt(*pc.sub, st, pos, [&](std::size_t end) {
			if (cap >= 0) {
				st.caps[static_cast<std::size_t>(cap)] = {static_cast<std::ptrdiff_t>(pos),
				                                     static_cast<std::ptrdiff_t>(end)};
			}
			return k(end);
		});
		if (!ok && cap >= 0) { st.caps[static_cast<std::size_t>(cap)] = saved; }
		return ok;
	}
	}
	return false;
}

inline constexpr bool rx_match_piece(const rx_piece & pc, rx_state & st, std::size_t pos, const rx_cont & k) {
	// quantified matching; a zero-width repetition stops the loop
	std::function<bool(std::size_t, std::int32_t)> rec = [&](std::size_t at, std::int32_t n) -> bool {
		const bool may_more = pc.max < 0 || n < pc.max;
		const bool may_stop = n >= pc.min;
		const auto more = [&]() {
			return may_more && rx_match_once(pc, st, at, [&](std::size_t np) {
				       return np == at ? (n + 1 >= pc.min && k(np)) : rec(np, n + 1);
			       });
		};
		if (pc.greedy) { return more() || (may_stop && k(at)); }
		return (may_stop && k(at)) || more();
	};
	return rec(pos, 0);
}

inline constexpr bool rx_match_seq(const rx_seq & sq, std::size_t idx, rx_state & st, std::size_t pos,
                         const rx_cont & k) {
	if (idx == sq.size()) { return k(pos); }
	return rx_match_piece(sq[idx], st, pos, [&](std::size_t np) {
		return rx_match_seq(sq, idx + 1, st, np, k);
	});
}

inline constexpr bool rx_match_alt(const rx_alt & alt, rx_state & st, std::size_t pos, const rx_cont & k) {
	for (const rx_seq & sq : alt.alts) {
		if (rx_match_seq(sq, 0, st, pos, k)) { return true; }
	}
	return false;
}

struct rx_match {
	std::size_t begin = 0, end = 0;
	std::vector<std::pair<std::ptrdiff_t, std::ptrdiff_t>> caps;
};

inline constexpr bool rx_search(const rx_prog & p, const std::string & s, std::size_t from, rx_match & out) {
	for (std::size_t start = from; start <= s.size(); ++start) {
		rx_state st;
		st.s = &s;
		st.p = &p;
		st.caps.assign(static_cast<std::size_t>(p.ngroups), {-1, -1});
		std::size_t got_end = 0;
		if (rx_match_alt(*p.root, st, start, [&](std::size_t end) {
			    got_end = end;
			    return true;
		    })) {
			out.begin = start;
			out.end = got_end;
			out.caps = std::move(st.caps);
			return true;
		}
	}
	return false;
}

} // namespace rxd

inline constexpr bool is_regex(const value & v) {
	return v.is_object() && v.as_object()->find("__regex") != nullptr;
}

// exec-shaped result: [full, group1, ...] (no .index/.input in v0.1)
inline constexpr value rx_exec_array(const std::string & s, const rxd::rx_match & m) {
	array_t out;
	out.push_back(value{s.substr(m.begin, m.end - m.begin)});
	for (const auto & [b, e] : m.caps) {
		if (b < 0) { out.push_back(value{}); }
		else {
			out.push_back(value{s.substr(static_cast<std::size_t>(b),
			                             static_cast<std::size_t>(e - b))});
		}
	}
	return value::array(std::move(out));
}

inline constexpr value make_regex(std::string source, std::string flags) {
	const auto prog = std::make_shared<rxd::rx_prog>(rxd::rx_compile(source, flags));
	const auto last_index = std::make_shared<std::size_t>(0); // g-mode exec cursor
	auto o = rc<object_t>::make();
	o->set("__regex", value{true});
	o->set("source", value{source});
	o->set("flags", value{flags});
	o->set("global", value{prog->global});
	o->set("ignoreCase", value{prog->icase});
	o->set("multiline", value{prog->multi});
	o->set("test", value::function(
	                   [prog](context &, const std::vector<value> & a) {
		                   const std::string s = a.empty() ? "" : a[0].to_string();
		                   rxd::rx_match m;
		                   return value{rxd::rx_search(*prog, s, 0, m)};
	                   },
	                   "test"));
	o->set("exec", value::function(
	                   [prog, last_index](context &, const std::vector<value> & a) -> value {
		                   const std::string s = a.empty() ? "" : a[0].to_string();
		                   const std::size_t from = prog->global ? *last_index : 0;
		                   rxd::rx_match m;
		                   if (from > s.size() || !rxd::rx_search(*prog, s, from, m)) {
			                   *last_index = 0;
			                   return value::null();
		                   }
		                   if (prog->global) {
			                   *last_index = m.end > m.begin ? m.end : m.end + 1;
		                   }
		                   return rx_exec_array(s, m);
	                   },
	                   "exec"));
	return value{std::move(o)};
}

// shared by String.prototype.{match,replace,split}: compile the regex
// object's source/flags (tiny patterns; compilation is cheap)
inline rxd::rx_prog rx_of(const value & re) {
	const value * src = re.as_object()->find("source");
	const value * flg = re.as_object()->find("flags");
	return rxd::rx_compile(src != nullptr ? src->to_string() : "",
	                       flg != nullptr ? flg->to_string() : "");
}

namespace detail {

inline constexpr value arg_or_undefined(const std::vector<value> & a, std::size_t i) {
	return i < a.size() ? a[i] : value{};
}

// JS ToIntegerOrInfinity + relative index clamping for slice()
inline constexpr std::size_t rel_index(double d, std::size_t len) {
	if (std::isnan(d)) { return 0; }
	if (d < 0) {
		d += static_cast<double>(len);
		if (d < 0) { return 0; }
	}
	if (d > static_cast<double>(len)) { return len; }
	return static_cast<std::size_t>(d);
}

// console.log renders arrays/objects like node does, not via ToString
inline constexpr std::string inspect(const value & v);

inline constexpr std::string inspect_parts(const value & v) {
	if (v.is_string()) { // quoted inside containers
		std::string out = "'";
		out += v.as_string();
		out += '\'';
		return out;
	}
	return inspect(v);
}

inline constexpr std::string inspect(const value & v) {
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
		std::string out = "{ ";
		bool first = true;
		for (const auto & [k, pv] : o.props) {
			if (k == "__ctor" || is_accessor(pv)) { continue; } // engine-internal
			if (!first) { out += ", "; }
			first = false;
			out += k;
			out += ": ";
			out += inspect_parts(pv);
		}
		out += " }";
		return first ? std::string{"{}"} : out;
	}
	if (v.is_function()) {
		const std::string & n = v.as_function()->name;
		return n.empty() ? "[Function (anonymous)]" : "[Function: " + n + "]";
	}
	// node's inspect distinguishes negative zero; String(-0) stays "0"
	if (v.is_number() && v.as_number() == 0 && std::signbit(v.as_number())) { return "-0"; }
	return v.to_string();
}

// JSON.stringify (no replacer/indent in v0.1); undefined/functions are
// omitted in objects and become null in arrays, like the spec
inline constexpr bool json_piece(const value & v, std::string & out) {
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
		if (k == "__ctor" || is_accessor(pv)) { continue; } // engine-internal
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

inline constexpr value get_member(context & cx, const value & recv, std::string_view name);

namespace detail {

inline constexpr value bound(std::string name, native_fn fn) {
	return value::function(std::move(fn), std::move(name));
}

inline constexpr value array_member(const value & recv, std::string_view name) {
	const rc<array_t> arr = recv.as_array();
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
			const std::size_t len = arr->size();
			const std::size_t from = a.empty() ? 0 : rel_index(a[0].to_number(), len);
			const std::size_t to = a.size() < 2 || a[1].is_undefined()
			                      ? len
			                      : rel_index(a[1].to_number(), len);
			array_t out;
			for (std::size_t i = from; i < to; ++i) { out.push_back((*arr)[i]); }
			return value::array(std::move(out));
		});
	}
	if (name == "splice") {
		return bound("splice", [arr](context &, const std::vector<value> & a) {
			const std::size_t len = arr->size();
			const std::size_t start = a.empty() ? 0 : rel_index(a[0].to_number(), len);
			std::size_t del;
			if (a.size() < 2) {
				del = len - start; // no deleteCount -> remove through the end
			} else {
				const double dc = a[1].to_number();
				del = (std::isnan(dc) || dc < 0) ? 0 : static_cast<std::size_t>(dc);
				if (del > len - start) { del = len - start; }
			}
			array_t removed;
			for (std::size_t i = 0; i < del; ++i) { removed.push_back((*arr)[start + i]); }
			arr->erase(arr->begin() + static_cast<std::ptrdiff_t>(start),
			           arr->begin() + static_cast<std::ptrdiff_t>(start + del));
			if (a.size() > 2) {
				arr->insert(arr->begin() + static_cast<std::ptrdiff_t>(start), a.begin() + 2, a.end());
			}
			return value::array(std::move(removed));
		});
	}
	if (name == "find" || name == "findIndex") {
		const bool want_index = (name == "findIndex");
		return bound(want_index ? "findIndex" : "find", [arr, recv, want_index](context & cx, const std::vector<value> & a) -> value {
			if (a.empty() || !a[0].is_function()) { return want_index ? value{-1.0} : value{}; }
			for (std::size_t i = 0; i < arr->size(); ++i) {
				if (call_value(cx, a[0], {(*arr)[i], value{static_cast<double>(i)}, recv}).truthy()) {
					return want_index ? value{static_cast<double>(i)} : (*arr)[i];
				}
			}
			return want_index ? value{-1.0} : value{};
		});
	}
	if (name == "some" || name == "every") {
		const bool is_every = (name == "every");
		return bound(is_every ? "every" : "some", [arr, recv, is_every](context & cx, const std::vector<value> & a) -> value {
			if (a.empty() || !a[0].is_function()) { return value{is_every}; }
			for (std::size_t i = 0; i < arr->size(); ++i) {
				const bool t = call_value(cx, a[0], {(*arr)[i], value{static_cast<double>(i)}, recv}).truthy();
				if (is_every && !t) { return value{false}; }
				if (!is_every && t) { return value{true}; }
			}
			return value{is_every};
		});
	}
	if (name == "fill") {
		return bound("fill", [arr](context &, const std::vector<value> & a) {
			const std::size_t len = arr->size();
			const value fv = arg_or_undefined(a, 0);
			const std::size_t from = a.size() < 2 ? 0 : rel_index(a[1].to_number(), len);
			const std::size_t to = a.size() < 3 || a[2].is_undefined() ? len : rel_index(a[2].to_number(), len);
			for (std::size_t i = from; i < to; ++i) { (*arr)[i] = fv; }
			return value{arr};
		});
	}
	if (name == "indexOf") {
		return bound("indexOf", [arr](context &, const std::vector<value> & a) {
			const value target = arg_or_undefined(a, 0);
			for (std::size_t i = 0; i < arr->size(); ++i) {
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
	if (name == "sort") {
		// default comparator is LEXICOGRAPHIC (ToString order) like the
		// spec - [10,1,2].sort() is [1,10,2]; undefined sorts last
		return bound("sort", [arr, recv](context & cx, const std::vector<value> & a) {
			const value cmp = arg_or_undefined(a, 0);
			std::stable_sort(arr->begin(), arr->end(),
			                 [&](const value & x, const value & y) {
				                 if (x.is_undefined()) { return false; }
				                 if (y.is_undefined()) { return true; }
				                 if (cmp.is_function()) {
					                 return call_value(cx, cmp, {x, y}).to_number() < 0;
				                 }
				                 return x.to_string() < y.to_string();
			                 });
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
			for (std::size_t i = 0; i < arr->size(); ++i) {
				out.push_back(call_value(c, arg_or_undefined(a, 0),
				                         {(*arr)[i], value{static_cast<double>(i)}}));
			}
			return value::array(std::move(out));
		});
	}
	if (name == "filter") {
		return bound("filter", [arr](context & c, const std::vector<value> & a) {
			array_t out;
			for (std::size_t i = 0; i < arr->size(); ++i) {
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
			for (std::size_t i = 0; i < arr->size(); ++i) {
				call_value(c, arg_or_undefined(a, 0),
				           {(*arr)[i], value{static_cast<double>(i)}});
			}
			return value{};
		});
	}
	if (name == "reduce") {
		return bound("reduce", [arr](context & c, const std::vector<value> & a) {
			std::size_t i = 0;
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

inline constexpr value string_member(const value & recv, std::string_view name) {
	const std::string s = recv.as_string();
	if (name == "length") { return value{s.size()}; }
	if (name == "slice") {
		return bound("slice", [s](context &, const std::vector<value> & a) {
			const std::size_t len = s.size();
			const std::size_t from = a.empty() ? 0 : rel_index(a[0].to_number(), len);
			const std::size_t to = a.size() < 2 || a[1].is_undefined()
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
			if (is_regex(a[0])) {
				const rxd::rx_prog prog = rx_of(a[0]);
				std::size_t at = 0;
				rxd::rx_match m;
				while (at <= s.size() && rxd::rx_search(prog, s, at, m)) {
					if (m.end == m.begin) { // zero-width: avoid stalling
						if (m.begin >= s.size()) { break; }
						m.end = m.begin + 1;
						out.push_back(value{s.substr(at, m.end - at)});
					} else {
						out.push_back(value{s.substr(at, m.begin - at)});
					}
					at = m.end;
				}
				out.push_back(value{s.substr(at)});
				return value::array(std::move(out));
			}
			const std::string sep = a[0].to_string();
			if (sep.empty()) {
				for (const char c : s) { out.push_back(value{std::string(1, c)}); }
				return value::array(std::move(out));
			}
			std::size_t at = 0;
			while (true) {
				const std::size_t hit = s.find(sep, at);
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
			return value{std::string(1, s[static_cast<std::size_t>(d)])};
		});
	}
	if (name == "charCodeAt") {
		return bound("charCodeAt", [s](context &, const std::vector<value> & a) {
			const double d = a.empty() ? 0 : a[0].to_number();
			if (std::isnan(d) || d < 0 || d >= static_cast<double>(s.size())) {
				return value{std::nan("")};
			}
			return value{static_cast<double>(
			    static_cast<unsigned char>(s[static_cast<std::size_t>(d)]))};
		});
	}
	if (name == "repeat") {
		return bound("repeat", [s](context &, const std::vector<value> & a) {
			const double d = a.empty() ? 0 : a[0].to_number();
			if (d < 0 || std::isinf(d)) { throw_error("RangeError", "Invalid count value"); }
			std::string out;
			for (std::size_t i = 0; i < static_cast<std::size_t>(d); ++i) { out += s; }
			return value{out};
		});
	}
	if (name == "replace") { // string pattern: first occurrence; regex: honors g
		return bound("replace", [s](context &, const std::vector<value> & a) {
			if (!a.empty() && is_regex(a[0])) {
				const rxd::rx_prog prog = rx_of(a[0]);
				const std::string tpl = arg_or_undefined(a, 1).to_string();
				std::string out;
				std::size_t at = 0;
				rxd::rx_match m;
				while (at <= s.size() && rxd::rx_search(prog, s, at, m)) {
					out += s.substr(at, m.begin - at);
					// substitute $&, $1..$9, $$ into the replacement
					for (std::size_t i = 0; i < tpl.size(); ++i) {
						if (tpl[i] != '$' || i + 1 >= tpl.size()) {
							out += tpl[i];
							continue;
						}
						const char d = tpl[++i];
						if (d == '$') { out += '$'; }
						else if (d == '&') { out += s.substr(m.begin, m.end - m.begin); }
						else if (d >= '1' && d <= '9' &&
						         static_cast<std::size_t>(d - '1') < m.caps.size()) {
							const auto & [b, e] = m.caps[static_cast<std::size_t>(d - '1')];
							if (b >= 0) {
								out += s.substr(static_cast<std::size_t>(b),
								                static_cast<std::size_t>(e - b));
							}
						} else {
							out += '$';
							out += d;
						}
					}
					at = m.end > m.begin ? m.end : m.end + 1;
					if (at > m.end && m.begin < s.size()) { out += s[m.begin]; }
					if (!prog.global) { break; }
				}
				out += at <= s.size() ? s.substr(at) : "";
				return value{out};
			}
			const std::string from = arg_or_undefined(a, 0).to_string();
			const std::string to = arg_or_undefined(a, 1).to_string();
			const std::size_t hit = s.find(from);
			if (hit == std::string::npos || from.empty()) { return value{s}; }
			std::string out = s;
			out.replace(hit, from.size(), to);
			return value{out};
		});
	}
	if (name == "match") { // regex only: g -> all full matches, else exec shape
		return bound("match", [s](context &, const std::vector<value> & a) -> value {
			if (a.empty() || !is_regex(a[0])) {
				throw_error("TypeError", "match() needs a regular expression");
			}
			const rxd::rx_prog prog = rx_of(a[0]);
			rxd::rx_match m;
			if (!prog.global) {
				if (!rxd::rx_search(prog, s, 0, m)) { return value::null(); }
				return rx_exec_array(s, m);
			}
			array_t out;
			std::size_t at = 0;
			while (at <= s.size() && rxd::rx_search(prog, s, at, m)) {
				out.push_back(value{s.substr(m.begin, m.end - m.begin)});
				at = m.end > m.begin ? m.end : m.end + 1;
			}
			if (out.empty()) { return value::null(); }
			return value::array(std::move(out));
		});
	}
	if (name == "replaceAll") {
		return bound("replaceAll", [s](context &, const std::vector<value> & a) {
			const std::string from = arg_or_undefined(a, 0).to_string();
			const std::string to = arg_or_undefined(a, 1).to_string();
			if (from.empty()) { return value{s}; }
			std::string out;
			std::size_t at = 0;
			while (true) {
				const std::size_t hit = s.find(from, at);
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
			const std::size_t want =
			    std::isnan(want_d) || want_d < 0 ? 0 : static_cast<std::size_t>(want_d);
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

inline constexpr value number_member(const value & recv, std::string_view name) {
	const double d = recv.as_number();
	if (name == "toString") {
		return bound("toString", [d](context &, const std::vector<value> &) {
			return value{number_to_string(d)};
		});
	}
	if (name == "toFixed") {
		return bound("toFixed", [d](context &, const std::vector<value> & a) {
			const std::int32_t digits = a.empty() ? 0 : static_cast<std::int32_t>(a[0].to_number());
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

inline constexpr value get_member(context & cx, const value & recv, std::string_view name) {
	if (recv.is_undefined() || recv.is_null()) {
		throw_error("TypeError", "Cannot read properties of " +
		                             std::string{recv.is_null() ? "null" : "undefined"} +
		                             " (reading '" + std::string{name} + "')");
	}
	if (recv.is_object()) {
		if (name == "__proto__") {
			const auto & pr = recv.as_object()->proto;
			return pr ? value{pr} : value::null();
		}
		// walk the [[Prototype]] chain: own props, then inherited methods
		for (object_t * o = recv.as_object().get(); o != nullptr; o = o->proto.get()) {
			if (const value * v = o->find(name)) {
				if (is_accessor(*v)) {
					const value * g = v->as_object()->find("get");
					if (g != nullptr && g->is_function()) {
						cx.pending_this = recv; // getter sees the ORIGINAL receiver
						return call_value(cx, *g, {});
					}
					return value{}; // setter-only property reads undefined
				}
				return *v;
			}
		}
		return value{};
	}
	if (recv.is_array()) { return detail::array_member(recv, name); }
	if (recv.is_string()) { return detail::string_member(recv, name); }
	if (recv.is_number()) { return detail::number_member(recv, name); }
	if (recv.is_function()) {
		const rc<function_t> f = recv.as_function();
		if (name == "prototype") {
			// every function has a .prototype; make one on demand so plain
			// `function F(){}; F.prototype.m = ...; new F()` works
			if (!f->props) { f->props = rc<object_t>::make(); }
			if (const value * pt = f->props->find("prototype")) { return *pt; }
			value proto = value::object();
			f->props->set("prototype", proto);
			return proto;
		}
		// statics ride on props; walk its chain for `class extends` statics
		for (object_t * o = f->props ? f->props.get() : nullptr; o != nullptr;
		     o = o->proto.get()) {
			if (const value * v = o->find(name)) { return *v; }
		}
		if (name == "name") { return value{f->name}; }
	}
	return value{};
}

// obj[i] / obj["key"] - arrays take numeric indexes, strings index to
// one-char strings, objects key by ToString
inline constexpr value get_index(context & cx, const value & recv, const value & key) {
	if (recv.is_array() && key.is_number()) {
		const double d = key.as_number();
		const auto & arr = *recv.as_array();
		if (d < 0 || d >= static_cast<double>(arr.size()) ||
		    d != static_cast<double>(static_cast<std::size_t>(d))) {
			return value{};
		}
		return arr[static_cast<std::size_t>(d)];
	}
	if (recv.is_string() && key.is_number()) {
		const double d = key.as_number();
		const std::string & s = recv.as_string();
		if (d < 0 || d >= static_cast<double>(s.size()) ||
		    d != static_cast<double>(static_cast<std::size_t>(d))) {
			return value{};
		}
		return value{std::string(1, s[static_cast<std::size_t>(d)])};
	}
	return get_member(cx, recv, key.to_string());
}

inline constexpr void set_member(context & cx, const value & recv, std::string_view name, value v) {
	if (recv.is_undefined() || recv.is_null()) {
		throw_error("TypeError", "Cannot set properties of " +
		                             std::string{recv.is_null() ? "null" : "undefined"} +
		                             " (setting '" + std::string{name} + "')");
	}
	if (recv.is_object()) {
		if (name == "__proto__") {
			if (v.is_object()) { recv.as_object()->proto = v.as_object(); }
			else if (v.is_null()) { recv.as_object()->proto = nullptr; }
			return;
		}
		// an accessor anywhere on the chain routes the write to its setter;
		// a data property (own or inherited) is shadowed by an own prop
		for (object_t * o = recv.as_object().get(); o != nullptr; o = o->proto.get()) {
			if (value * cur = o->find(name)) {
				if (is_accessor(*cur)) {
					const value * st = cur->as_object()->find("set");
					if (st != nullptr && st->is_function()) {
						cx.pending_this = recv;
						(void)call_value(cx, *st, {std::move(v)});
					}
					return; // getter-only property writes are silent no-ops
				}
				break;
			}
		}
		recv.as_object()->set(name, std::move(v)); // own data property
		return;
	}
	if (recv.is_array() && name == "length") {
		recv.as_array()->resize(static_cast<std::size_t>(v.to_number()));
		return;
	}
	if (recv.is_function()) { // statics: C.x = ... / F.prototype = ...
		const rc<function_t> f = recv.as_function();
		if (!f->props) { f->props = rc<object_t>::make(); }
		f->props->set(name, std::move(v));
		return;
	}
	// numbers/strings: silent no-op, like non-strict JS
}

inline constexpr void set_index(context & cx, const value & recv, const value & key, value v) {
	if (recv.is_array() && key.is_number()) {
		const double d = key.as_number();
		if (d >= 0 && d == static_cast<double>(static_cast<std::size_t>(d))) {
			auto & arr = *recv.as_array();
			const std::size_t i = static_cast<std::size_t>(d);
			if (i >= arr.size()) { arr.resize(i + 1); }
			arr[i] = std::move(v);
			return;
		}
	}
	set_member(cx, recv, key.to_string(), std::move(v));
}

// --- the default global environment

namespace detail {

inline constexpr value make_console() {
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

inline constexpr value math_fn(std::string name, double (*f)(double)) {
	return value::function(
	    [f](context &, const std::vector<value> & a) {
		    return value{f(arg_or_undefined(a, 0).to_number())};
	    },
	    std::move(name));
}

inline constexpr value make_math() {
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

// JSON.parse: strict recursive descent -> value tree; SyntaxError on
// anything malformed (no reviver in v0.1)
inline constexpr void json_ws(std::string_view s, std::size_t & i) {
	while (i < s.size() &&
	       (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
		++i;
	}
}

[[noreturn]] inline void json_fail(std::size_t at) {
	throw_error("SyntaxError", "Unexpected token in JSON at position " + std::to_string(at));
}

inline constexpr void json_utf8(std::string & out, std::uint32_t cp) {
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

inline std::uint32_t json_hex4(std::string_view s, std::size_t & i) {
	if (i + 4 > s.size()) { json_fail(i); }
	std::uint32_t cp = 0;
	for (std::size_t k = 0; k < 4; ++k) {
		const char c = s[i + k];
		cp <<= 4;
		if (c >= '0' && c <= '9') { cp |= static_cast<std::uint32_t>(c - '0'); }
		else if (c >= 'a' && c <= 'f') { cp |= static_cast<std::uint32_t>(c - 'a' + 10); }
		else if (c >= 'A' && c <= 'F') { cp |= static_cast<std::uint32_t>(c - 'A' + 10); }
		else { json_fail(i + k); }
	}
	i += 4;
	return cp;
}

inline constexpr std::string json_string(std::string_view s, std::size_t & i) {
	// s[i] == '"' on entry
	++i;
	std::string out;
	while (true) {
		if (i >= s.size()) { json_fail(i); }
		const char c = s[i];
		if (c == '"') { ++i; return out; }
		if (static_cast<unsigned char>(c) < 0x20) { json_fail(i); }
		if (c != '\\') {
			out += c;
			++i;
			continue;
		}
		if (++i >= s.size()) { json_fail(i); }
		switch (s[i]) {
		case '"': out += '"'; ++i; break;
		case '\\': out += '\\'; ++i; break;
		case '/': out += '/'; ++i; break;
		case 'b': out += '\b'; ++i; break;
		case 'f': out += '\f'; ++i; break;
		case 'n': out += '\n'; ++i; break;
		case 'r': out += '\r'; ++i; break;
		case 't': out += '\t'; ++i; break;
		case 'u': {
			++i;
			std::uint32_t cp = json_hex4(s, i);
			if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i] == '\\' &&
			    s[i + 1] == 'u') { // surrogate pair
				std::size_t j = i + 2;
				const std::uint32_t lo = json_hex4(s, j);
				if (lo >= 0xDC00 && lo <= 0xDFFF) {
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
					i = j;
				}
			}
			json_utf8(out, cp);
			break;
		}
		default: json_fail(i);
		}
	}
}

inline constexpr value json_value(std::string_view s, std::size_t & i, std::int32_t depth) {
	if (depth > 128) { json_fail(i); } // nesting bound, plain-stack safety
	json_ws(s, i);
	if (i >= s.size()) { json_fail(i); }
	const char c = s[i];
	if (c == '"') { return value{json_string(s, i)}; }
	if (c == '{') {
		++i;
		object_t obj;
		json_ws(s, i);
		if (i < s.size() && s[i] == '}') { ++i; return value::object(std::move(obj)); }
		while (true) {
			json_ws(s, i);
			if (i >= s.size() || s[i] != '"') { json_fail(i); }
			std::string key = json_string(s, i);
			json_ws(s, i);
			if (i >= s.size() || s[i] != ':') { json_fail(i); }
			++i;
			obj.set(key, json_value(s, i, depth + 1));
			json_ws(s, i);
			if (i < s.size() && s[i] == ',') { ++i; continue; }
			if (i < s.size() && s[i] == '}') { ++i; return value::object(std::move(obj)); }
			json_fail(i);
		}
	}
	if (c == '[') {
		++i;
		array_t arr;
		json_ws(s, i);
		if (i < s.size() && s[i] == ']') { ++i; return value::array(std::move(arr)); }
		while (true) {
			arr.push_back(json_value(s, i, depth + 1));
			json_ws(s, i);
			if (i < s.size() && s[i] == ',') { ++i; continue; }
			if (i < s.size() && s[i] == ']') { ++i; return value::array(std::move(arr)); }
			json_fail(i);
		}
	}
	if (s.compare(i, 4, "true") == 0) { i += 4; return value{true}; }
	if (s.compare(i, 5, "false") == 0) { i += 5; return value{false}; }
	if (s.compare(i, 4, "null") == 0) { i += 4; return value::null(); }
	if (c == '-' || (c >= '0' && c <= '9')) {
		const std::size_t start = i;
		if (s[i] == '-') { ++i; }
		if (i >= s.size() || s[i] < '0' || s[i] > '9') { json_fail(i); }
		if (s[i] == '0') { ++i; } // no leading zeros
		else { while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; } }
		if (i < s.size() && s[i] == '.') {
			++i;
			if (i >= s.size() || s[i] < '0' || s[i] > '9') { json_fail(i); }
			while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; }
		}
		if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
			++i;
			if (i < s.size() && (s[i] == '+' || s[i] == '-')) { ++i; }
			if (i >= s.size() || s[i] < '0' || s[i] > '9') { json_fail(i); }
			while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; }
		}
		const std::string num{s.substr(start, i - start)};
		return value{std::strtod(num.c_str(), nullptr)};
	}
	json_fail(i);
}

inline constexpr value make_json() {
	object_t json;
	json.set("parse", value::function(
	                      [](context &, const std::vector<value> & a) {
		                      const std::string text = arg_or_undefined(a, 0).to_string();
		                      std::size_t i = 0;
		                      value out = json_value(text, i, 0);
		                      json_ws(text, i);
		                      if (i != text.size()) { json_fail(i); }
		                      return out;
	                      },
	                      "parse"));
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

// days -> y/m/d (proleptic Gregorian, UTC); Hinnant's civil_from_days
inline constexpr void civil_from_days(std::int64_t z, std::int64_t & y, std::uint32_t & m, std::uint32_t & d) {
	z += 719468;
	const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	const std::uint64_t doe = static_cast<std::uint64_t>(z - era * 146097);
	const std::uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y = static_cast<std::int64_t>(yoe) + era * 400;
	const std::uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const std::uint64_t mp = (5 * doy + 2) / 153;
	d = static_cast<std::uint32_t>(doy - (153 * mp + 2) / 5 + 1);
	m = static_cast<std::uint32_t>(mp < 10 ? mp + 3 : mp - 9);
	if (m <= 2) { ++y; }
}

// y/m/d -> days since the epoch (inverse of the above); Hinnant
inline std::int64_t days_from_civil(std::int64_t y, std::uint32_t m, std::uint32_t d) {
	y -= m <= 2;
	const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
	const std::uint32_t yoe = static_cast<std::uint32_t>(y - era * 400);
	const std::uint32_t doy =
	    (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
	const std::uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
	return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// parse the ISO-8601 subset toISOString produces (plus date-only and
// no-Z forms), always as UTC; NaN on anything malformed
inline constexpr double parse_date_ms(std::string_view s) {
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) { s.remove_prefix(1); }
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) { s.remove_suffix(1); }
	const auto num = [&](std::size_t pos, std::size_t len) -> std::int32_t {
		if (pos + len > s.size()) { return -1; }
		std::int32_t v = 0;
		for (std::size_t i = 0; i < len; ++i) {
			const char c = s[pos + i];
			if (c < '0' || c > '9') { return -1; }
			v = v * 10 + (c - '0');
		}
		return v;
	};
	if (s.size() < 10 || s[4] != '-' || s[7] != '-') { return std::nan(""); }
	const std::int32_t Y = num(0, 4), Mo = num(5, 2), D = num(8, 2);
	if (Y < 0 || Mo < 1 || Mo > 12 || D < 1 || D > 31) { return std::nan(""); }
	std::int32_t hh = 0, mm = 0, ss = 0, ms = 0;
	std::size_t i = 10;
	if (s.size() > 10 && (s[10] == 'T' || s[10] == ' ')) {
		if (s.size() < 16 || s[13] != ':') { return std::nan(""); }
		hh = num(11, 2);
		mm = num(14, 2);
		if (hh < 0 || mm < 0) { return std::nan(""); }
		i = 16;
		if (s.size() > i && s[i] == ':') {
			ss = num(i + 1, 2);
			if (ss < 0) { return std::nan(""); }
			i += 3;
		}
		if (s.size() > i && s[i] == '.') {
			std::size_t j = i + 1, k = 0;
			std::int32_t f = 0;
			while (j < s.size() && k < 3 && s[j] >= '0' && s[j] <= '9') {
				f = f * 10 + (s[j] - '0');
				++j;
				++k;
			}
			while (k < 3) { f *= 10; ++k; }
			ms = f;
			while (j < s.size() && s[j] >= '0' && s[j] <= '9') { ++j; }
			i = j;
		}
	}
	if (i < s.size() && s[i] != 'Z') { return std::nan(""); } // only UTC ('Z' or bare)
	const std::int64_t days = days_from_civil(Y, static_cast<std::uint32_t>(Mo), static_cast<std::uint32_t>(D));
	return static_cast<double>(days) * 86400000.0 + static_cast<double>(hh) * 3600000.0 +
	       static_cast<double>(mm) * 60000.0 + static_cast<double>(ss) * 1000.0 +
	       static_cast<double>(ms);
}

inline constexpr value make_date_object(double ms) {
	auto o = rc<object_t>::make();
	o->set("__date_ms", value{ms});
	const auto part = [ms](std::int32_t which) -> double {
		const std::int64_t total = static_cast<std::int64_t>(ms);
		std::int64_t days = total / 86400000;
		std::int64_t rem = total % 86400000;
		if (rem < 0) { rem += 86400000; --days; }
		std::int64_t y;
		std::uint32_t mo, dy;
		civil_from_days(days, y, mo, dy);
		switch (which) {
		case 0: return static_cast<double>(y);
		case 1: return static_cast<double>(mo - 1); // JS months are 0-based
		case 2: return static_cast<double>(dy);
		case 3: return static_cast<double>(rem / 3600000);
		case 4: return static_cast<double>((rem / 60000) % 60);
		case 5: return static_cast<double>((rem / 1000) % 60);
		case 6: return static_cast<double>(rem % 1000);
		default: return static_cast<double>((days % 7 + 11) % 7); // 1970-01-01 = Thursday
		}
	};
	const auto getter = [part](const char * name, std::int32_t which) {
		return value::function(
		    [part, which](context &, const std::vector<value> &) { return value{part(which)}; },
		    name);
	};
	o->set("getTime", value::function(
	                      [ms](context &, const std::vector<value> &) { return value{ms}; },
	                      "getTime"));
	o->set("getUTCFullYear", getter("getUTCFullYear", 0));
	o->set("getFullYear", getter("getFullYear", 0)); // engine is UTC-only
	o->set("getMonth", getter("getMonth", 1));
	o->set("getDate", getter("getDate", 2));
	o->set("getHours", getter("getHours", 3));
	o->set("getMinutes", getter("getMinutes", 4));
	o->set("getSeconds", getter("getSeconds", 5));
	o->set("getMilliseconds", getter("getMilliseconds", 6));
	o->set("getDay", getter("getDay", 7));
	o->set("toISOString", value::function(
	                          [part](context &, const std::vector<value> &) {
		                          char buf[40];
		                          std::snprintf(buf, sizeof(buf),
		                                        "%04lld-%02u-%02uT%02u:%02u:%02u.%03uZ",
		                                        static_cast<std::int64_t>(part(0)),
		                                        static_cast<std::uint32_t>(part(1)) + 1,
		                                        static_cast<std::uint32_t>(part(2)),
		                                        static_cast<std::uint32_t>(part(3)),
		                                        static_cast<std::uint32_t>(part(4)),
		                                        static_cast<std::uint32_t>(part(5)),
		                                        static_cast<std::uint32_t>(part(6)));
		                          return value{std::string{buf}};
	                          },
	                          "toISOString"));
	return value{std::move(o)};
}

inline constexpr env_ptr make_globals() {
	auto g = rc<environment>::make();
	g->function_scope = true; // the global scope is where top-level var lands
	g->declare("undefined", value{});
	g->declare("NaN", value{std::nan("")});
	g->declare("Infinity", value{INFINITY});
	g->declare("console", detail::make_console());
	g->declare("Math", detail::make_math());
	g->declare("JSON", detail::make_json());
	{
		// Date, the UTC subset: new Date() / new Date(ms) / new Date(str)
		// with the usual getters + toISOString; Date.now() is the host
		// wall clock (the one impure global besides console - hosts can
		// rebind it). Local-time getters alias the UTC ones; no setters.
		value date_fn = value::function(
		    [](context & cx, const std::vector<value> & a) -> value {
			    double ms;
			    if (a.empty()) {
				    ms = static_cast<double>(
				        std::chrono::duration_cast<std::chrono::milliseconds>(
				            std::chrono::system_clock::now().time_since_epoch())
				            .count());
			    } else if (a[0].is_number()) {
				    ms = a[0].as_number();
			    } else {
				    ms = parse_date_ms(a[0].to_string());
			    }
			    value d = make_date_object(ms);
			    // fold into the fresh `this` new_op parked, keeping the
			    // __ctor stamp so `x instanceof Date` holds
			    if (cx.current_this.is_object()) {
				    for (auto & [k, pv] : d.as_object()->props) {
					    cx.current_this.as_object()->set(k, pv);
				    }
				    return cx.current_this;
			    }
			    return d;
		    },
		    "Date");
		date_fn.as_function()->props = rc<object_t>::make();
		date_fn.as_function()->props->set(
		    "now", value::function(
		               [](context &, const std::vector<value> &) {
			               return value{static_cast<double>(
			                   std::chrono::duration_cast<std::chrono::milliseconds>(
			                       std::chrono::system_clock::now().time_since_epoch())
			                       .count())};
		               },
		               "now"));
		date_fn.as_function()->props->set(
		    "parse", value::function(
		                 [](context &, const std::vector<value> & a) {
			                 return value{a.empty() ? std::nan("")
			                                        : parse_date_ms(a[0].to_string())};
		                 },
		                 "parse"));
		g->declare("Date", std::move(date_fn));
	}
	{
		// Object: prototype plumbing + the common own-key helpers.
		// __ctor (our instanceof stamp) and accessor markers stay hidden.
		const auto own_ok = [](const std::string & k, const value & v) {
			return k != "__ctor" && !is_accessor(v);
		};
		object_t o;
		o.set("keys", value::function(
		                  [own_ok](context &, const std::vector<value> & a) {
			                  array_t out;
			                  if (!a.empty() && a[0].is_object()) {
				                  for (const auto & [k, v] : a[0].as_object()->props) {
					                  if (own_ok(k, v)) { out.push_back(value{k}); }
				                  }
			                  }
			                  return value::array(std::move(out));
		                  },
		                  "keys"));
		o.set("values", value::function(
		                    [own_ok](context &, const std::vector<value> & a) {
			                    array_t out;
			                    if (!a.empty() && a[0].is_object()) {
				                    for (const auto & [k, v] : a[0].as_object()->props) {
					                    if (own_ok(k, v)) { out.push_back(v); }
				                    }
			                    }
			                    return value::array(std::move(out));
		                    },
		                    "values"));
		o.set("entries", value::function(
		                     [own_ok](context &, const std::vector<value> & a) {
			                     array_t out;
			                     if (!a.empty() && a[0].is_object()) {
				                     for (const auto & [k, v] : a[0].as_object()->props) {
					                     if (own_ok(k, v)) {
						                     out.push_back(value::array({value{k}, v}));
					                     }
				                     }
			                     }
			                     return value::array(std::move(out));
		                     },
		                     "entries"));
		o.set("assign", value::function(
		                    [own_ok](context & cx, const std::vector<value> & a) -> value {
			                    if (a.empty()) { return value{}; }
			                    const value target = a[0];
			                    if (!target.is_object()) { return target; }
			                    for (std::size_t i = 1; i < a.size(); ++i) {
				                    if (!a[i].is_object()) { continue; }
				                    for (const auto & [k, v] : a[i].as_object()->props) {
					                    if (own_ok(k, v)) { set_member(cx, target, k, v); }
				                    }
			                    }
			                    return target;
		                    },
		                    "assign"));
		o.set("create", value::function(
		                    [](context &, const std::vector<value> & a) {
			                    value obj = value::object();
			                    if (!a.empty() && a[0].is_object()) {
				                    obj.as_object()->proto = a[0].as_object();
			                    }
			                    return obj;
		                    },
		                    "create"));
		o.set("getPrototypeOf", value::function(
		                            [](context &, const std::vector<value> & a) -> value {
			                            if (!a.empty() && a[0].is_object()) {
				                            const auto & p = a[0].as_object()->proto;
				                            return p ? value{p} : value::null();
			                            }
			                            return value::null();
		                            },
		                            "getPrototypeOf"));
		o.set("setPrototypeOf", value::function(
		                            [](context &, const std::vector<value> & a) -> value {
			                            if (a.size() >= 2 && a[0].is_object()) {
				                            if (a[1].is_object()) {
					                            a[0].as_object()->proto = a[1].as_object();
				                            } else if (a[1].is_null()) {
					                            a[0].as_object()->proto = nullptr;
				                            }
			                            }
			                            return a.empty() ? value{} : a[0];
		                            },
		                            "setPrototypeOf"));
		o.set("freeze", value::function( // no frozen bit yet: identity
		                    [](context &, const std::vector<value> & a) {
			                    return a.empty() ? value{} : a[0];
		                    },
		                    "freeze"));
		g->declare("Object", value::object(std::move(o)));
	}
	{
		// Promise, the settled subset (see make_promise above):
		// resolve/reject/all - `new Promise(executor)` is deliberately
		// absent, since an executor implies pending state
		object_t p;
		p.set("resolve", value::function(
		                     [](context &, const std::vector<value> & a) {
			                     value v = detail::arg_or_undefined(a, 0);
			                     return is_promise(v) ? v : make_promise(std::move(v), false);
		                     },
		                     "resolve"));
		p.set("reject", value::function(
		                    [](context &, const std::vector<value> & a) {
			                    return make_promise(detail::arg_or_undefined(a, 0), true);
		                    },
		                    "reject"));
		p.set("all", value::function(
		                 [](context &, const std::vector<value> & a) -> value {
			                 array_t out;
			                 if (!a.empty() && a[0].is_array()) {
				                 for (const value & e : *a[0].as_array()) {
					                 if (!is_promise(e)) {
						                 out.push_back(e);
						                 continue;
					                 }
					                 const rc<object_t> o = e.as_object();
					                 const value * st = o->find("__state");
					                 const value * pv = o->find("__value");
					                 value inner = pv != nullptr ? *pv : value{};
					                 if (st != nullptr && st->is_string() &&
					                     st->as_string() == "rejected") {
						                 return make_promise(std::move(inner), true);
					                 }
					                 out.push_back(std::move(inner));
				                 }
			                 }
			                 return make_promise(value::array(std::move(out)), false);
		                 },
		                 "all"));
		g->declare("Promise", value::object(std::move(p)));
	}
	g->declare("parseInt", value::function(
	                           [](context &, const std::vector<value> & a) {
		                           std::string s = detail::arg_or_undefined(a, 0).to_string();
		                           const std::int32_t radix =
		                               a.size() > 1 && !a[1].is_undefined()
		                                   ? static_cast<std::int32_t>(a[1].to_number())
		                                   : 10;
		                           std::size_t at = 0;
		                           while (at < s.size() &&
		                                  (s[at] == ' ' || s[at] == '\t' || s[at] == '\n')) {
			                           ++at;
		                           }
		                           bool neg = false;
		                           if (at < s.size() && (s[at] == '+' || s[at] == '-')) {
			                           neg = s[at] == '-';
			                           ++at;
		                           }
		                           // spec: an unspecified (or 16) radix accepts an
		                           // 0x/0X prefix and parses hexadecimal
		                           std::int32_t r = radix;
		                           if ((r == 10 || r == 16) && at + 1 < s.size() &&
		                               s[at] == '0' && (s[at + 1] == 'x' || s[at + 1] == 'X')) {
			                           const bool defaulted =
			                               a.size() <= 1 || a[1].is_undefined() ||
			                               a[1].to_number() == 0 || r == 16;
			                           if (defaulted) {
				                           r = 16;
				                           at += 2;
			                           }
		                           }
		                           std::int64_t out = 0;
		                           std::size_t digits = 0;
		                           for (; at < s.size(); ++at) {
			                           std::int32_t d = -1;
			                           const char c = s[at];
			                           if (c >= '0' && c <= '9') { d = c - '0'; }
			                           else if (c >= 'a' && c <= 'z') { d = c - 'a' + 10; }
			                           else if (c >= 'A' && c <= 'Z') { d = c - 'A' + 10; }
			                           if (d < 0 || d >= r) { break; }
			                           out = out * r + d;
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

	// RegExp: makes regex literals' .test/.exec work (eval_regex builds via
	// this global), and `new RegExp(src, flags)` / RegExp(existing)
	g->declare("RegExp", value::function(
	                         [](context &, const std::vector<value> & a) -> value {
		                         std::string src, flags;
		                         if (!a.empty() && is_regex(a[0])) {
			                         const rc<object_t> o = a[0].as_object();
			                         if (const value * s = o->find("source")) { src = s->to_string(); }
			                         if (const value * f = o->find("flags")) { flags = f->to_string(); }
		                         } else if (!a.empty() && !a[0].is_undefined()) {
			                         src = a[0].to_string();
		                         }
		                         if (a.size() > 1 && !a[1].is_undefined()) { flags = a[1].to_string(); }
		                         return make_regex(src, flags);
	                         },
	                         "RegExp"));

	// the Error family: `new Error(msg)` / `throw new TypeError(msg)`, with the
	// { name, message } shape catch/instanceof and console printing expect
	const auto err_ctor = [](const char * nm) {
		return [nm](context & cx, const std::vector<value> & a) -> value {
			const std::string msg = a.empty() ? std::string{} : a[0].to_string();
			if (cx.current_this.is_object()) {
				cx.current_this.as_object()->set("name", value{std::string{nm}});
				cx.current_this.as_object()->set("message", value{msg});
				cx.current_this.as_object()->set("stack", value{std::string{nm} + ": " + msg});
				return cx.current_this;
			}
			value e = make_error(nm, msg);
			e.as_object()->set("stack", value{std::string{nm} + ": " + msg});
			return e;
		};
	};
	for (const char * en : {"Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError",
	                        "EvalError", "URIError"}) {
		g->declare(en, value::function(err_ctor(en), en));
	}
	return g;
}

} // namespace ctjs

#endif
