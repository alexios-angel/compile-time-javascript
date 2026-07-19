#ifndef CTJS__SCRIPT__HPP
#define CTJS__SCRIPT__HPP

#include "grammar.hpp"
#include "lower.hpp"
#include "interp.hpp"
#ifndef CTJS_IN_A_MODULE
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#endif

// The public surface. A script is parsed at COMPILE time - the source
// is a template argument, a syntax error is a compile error naming the
// diagnostic query to run - and executes at RUNTIME:
//
//   constexpr auto & src = ctjs::script<R"(
//       let total = 0;
//       for (let i = 1; i <= 10; i++) { total += i; }
//       console.log("total", total);
//   )">;
//   auto out = src.run();
//   out.ok();          // no uncaught exception
//   out.console();     // "total 55\n"
//   out["total"].to<int>();
//
// run() seeds the default globals (console, Math, JSON, parseInt...).
// Pass ctjs::binding{"name", value} instances to inject host values -
// including native functions, which is how a host application exposes
// its own API (DOM, anyone?) to scripts. After run(), functions the
// script declared are callable from C++ through result.call("fn", ...).

namespace ctjs {

// a host-provided global: a value or a native function under a name
struct binding {
	std::string name;
	value v;
};

// make a native function value out of any callable taking
// (context&, const std::vector<value>&) or (const std::vector<value>&)
template <typename F> value native(F && f, std::string name = {}) {
	if constexpr (std::is_invocable_v<F, context &, const std::vector<value> &>) {
		return value::function(std::forward<F>(f), std::move(name));
	} else {
		return value::function(
		    [g = std::forward<F>(f)](context &, const std::vector<value> & args) -> value {
			    if constexpr (std::is_void_v<decltype(g(args))>) {
				    g(args);
				    return value{};
			    } else {
				    return value{g(args)};
			    }
		    },
		    std::move(name));
	}
}

class run_result {
public:
	run_result(std::shared_ptr<context> cx, env_ptr globals)
	    : cx_(std::move(cx)), globals_(std::move(globals)) { }

	bool ok() const { return !failed_; }
	const value & exception() const { return error_; }
	std::string exception_message() const { return error_to_string(error_); }

	// everything console.log printed, in order
	std::string_view console() const { return cx_->console; }
	// the value of the last expression statement (like a REPL)
	const value & result() const { return cx_->last; }

	// read a global the script left behind
	value operator[](std::string_view name) const {
		if (const value * slot = globals_->find(name)) { return *slot; }
		return value{};
	}

	// write a global between calls - how hosts model the web's live
	// globals (p5's mouseX/frameCount update before every draw())
	void set_global(std::string_view name, value v) {
		globals_->declare(name, std::move(v));
	}

	// invoke a function the script defined; console output continues to
	// accumulate on this result
	template <typename... Args> value call(std::string_view fn, Args &&... args) {
		const value * slot = globals_->find(fn);
		if (slot == nullptr) {
			throw_error("ReferenceError", std::string{fn} + " is not defined");
		}
		std::vector<value> argv{value{std::forward<Args>(args)}...};
		return call_value(*cx_, *slot, std::move(argv));
	}

	void mark_failed(value err) {
		failed_ = true;
		error_ = std::move(err);
	}

private:
	std::shared_ptr<context> cx_;
	env_ptr globals_;
	bool failed_ = false;
	value error_;
};

namespace detail {

template <typename Program> struct program_runner;
template <typename... Ss> struct program_runner<ast::program<Ss...>> {
	static void go(const env_ptr & env, context & cx) {
		hoist_functions<Ss...>(env, cx);
		value ret;
		(void)exec_all<Ss...>(env, cx, ret);
	}
};

} // namespace detail

template <CTJS_STRING_INPUT Src> struct script_t {
	static constexpr bool valid =
	    ctlark::is_valid<detail::js_grammar, Src, detail::js_start>;

	static run_result run(std::vector<binding> host = {}) {
		static_assert(ctlark::is_valid<detail::js_grammar, Src, detail::js_start>,
		              "ctjs: the script is not valid JavaScript (within the supported "
		              "subset) - print ctjs::error_message<Src>() for the location and "
		              "the expected tokens");
		auto cx = rc<context>::make();
		env_ptr globals = make_globals();
		for (binding & b : host) { globals->declare(b.name, std::move(b.v)); }
		run_result out{cx, globals};
		if constexpr (valid) {
			using tree = decltype(ctlark::parse<detail::js_grammar, Src, detail::js_start>());
			using p0 = typename detail::lower_program<tree>::type;
			// whole-program pass: constant-evaluate calls to named functions
			using program = detail::rewrite_t<p0, typename detail::collect_fns<p0>::type>;
			try {
				detail::program_runner<program>::go(globals, *cx);
			} catch (js_throw & t) {
				out.mark_failed(std::move(t.thrown));
			}
		}
		return out;
	}
};

#if CTLL_CNTTP_COMPILER_CHECK
template <ctll::fixed_string Src> inline constexpr script_t<Src> script{};

// --- compile-time constant evaluation (the folder, fold.hpp).
//
// A script that is a single constant expression statement is evaluated
// AT COMPILE TIME, so its value is a `constexpr` usable in a
// static_assert - no interpreter runs:
//   static_assert(ctjs::is_constant<"2 ** 10 + 24;">);
//   static_assert(ctjs::constant<"2 ** 10 + 24;"> == 1048.0);
// (This is the same fold that, inside any larger script, collapses
// constant subexpressions and dead ternary/`&&`/`||`/`??` branches
// before the interpreter runs.) `is_constant` is false for a
// non-constant expression; `constant` requires a numeric constant.
namespace detail {
template <ctll::fixed_string Src> struct const_of {
	using tree = decltype(ctlark::parse<js_grammar, Src, js_start>());
	using program = typename lower_program<tree>::type;
	static constexpr folded value = program_constant<program>::value;
};
} // namespace detail

template <ctll::fixed_string Src>
inline constexpr bool is_constant =
    ctlark::is_valid<detail::js_grammar, Src, detail::js_start> &&
    detail::const_of<Src>::value.ok();

template <ctll::fixed_string Src>
inline constexpr double constant = detail::const_of<Src>::value.num;
#endif

// one-shot convenience: parse at compile time, run now
template <CTJS_STRING_INPUT Src> run_result run(std::vector<binding> host = {}) {
	return script_t<Src>::run(std::move(host));
}

} // namespace ctjs

#endif
