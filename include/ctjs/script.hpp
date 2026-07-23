#ifndef CTJS__SCRIPT__HPP
#define CTJS__SCRIPT__HPP

#include "../ctll/fixed_string.hpp" // the NTTP string carrier for script<Src>
#include "vinterp.hpp"              // the value parser + interpreter
#ifndef CTJS_IN_A_MODULE
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#endif

// The public surface. A script can be handed over as a template
// argument (script<Src> - its validity is checked during compilation by
// the constexpr value parser) or as an ordinary runtime string
// (run_value); both execute at RUNTIME through the same interpreter:
//
//   constexpr auto & src = ctjs::script<R"(
//       let total = 0;
//       for (let i = 1; i <= 10; i++) { total += i; }
//       console.log("total", total);
//   )">;
//   auto out = src.run();
//   out.ok();          // no uncaught exception
//   out.console();     // "total 55\n"
//   out["total"].to<std::int32_t>();
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
	run_result(rc<context> cx, env_ptr globals)
	    : cx_(std::move(cx)), globals_(std::move(globals)) { }

	bool ok() const { return !failed_; }
	const value & exception() const { return error_; }
	std::string exception_message() const { return error_to_string(error_); }
	// the error's captured call-stack trace ("Msg\n  at f\n  at g"), or just the
	// message when no trace was attached (call_value builds the trace)
	std::string exception_stack() const {
		if (error_.is_object()) {
			if (const value * s = error_.as_object()->find("stack")) { return s->to_string(); }
		}
		return error_to_string(error_);
	}

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

	// keep an opaque owner alive for the lifetime of this result - the value
	// interpreter's vm, whose function closures the globals still reference
	void keep_alive(std::shared_ptr<void> owner) { owner_ = std::move(owner); }

private:
	rc<context> cx_;
	env_ptr globals_;
	bool failed_ = false;
	value error_;
	std::shared_ptr<void> owner_;
};

// --- the NTTP surface, value-backed ---------------------------------
// The source rides as a ctll::fixed_string template argument; validity
// is proven during compilation by the CONSTEXPR value parser, and run()
// executes through the same run_value machinery as a runtime string.

namespace detail {

// ctll::fixed_string stores wide code units; materialize the script's
// bytes once per Src as a static char array
template <ctll::fixed_string Src> struct src_bytes {
	static constexpr auto compute() noexcept {
		std::array<char, Src.size() + 1> out{};
		for (std::size_t i = 0; i < Src.size(); ++i) {
			out[i] = static_cast<char>(Src.content[i]);
		}
		return out;
	}
	static constexpr std::array<char, Src.size() + 1> storage = compute();
	static constexpr std::string_view view() noexcept {
		return std::string_view{storage.data(), Src.size()};
	}
};

} // namespace detail

// does the source parse as ctjs's JavaScript subset?
CTLL_EXPORT template <ctll::fixed_string Src>
inline constexpr bool is_valid = vp::is_valid(detail::src_bytes<Src>::view());

CTLL_EXPORT template <ctll::fixed_string Src> struct script_t {
	// queryable without error (v8diff skips parse gaps through it)
	static constexpr bool valid = is_valid<Src>;

	static run_result run(std::vector<binding> host = {}) {
		static_assert(is_valid<Src>,
		              "ctjs: the script is not valid JavaScript (within the "
		              "supported subset) - run vp::parse on the source at "
		              "runtime for the offending token");
		return run_value(detail::src_bytes<Src>::view(), std::move(host));
	}
};

CTLL_EXPORT template <ctll::fixed_string Src> inline constexpr script_t<Src> script{};

// one-shot convenience: validity proven at compile time, run now
CTLL_EXPORT template <ctll::fixed_string Src> run_result run(std::vector<binding> host = {}) {
	return script_t<Src>::run(std::move(host));
}

// Run a script BY VALUE: parse it with the recursive-descent value parser and
// execute it with the value tree-walking interpreter, both at RUNTIME - no
// Earley parse, no per-script template instantiation. The `src` is an ordinary
// runtime string (e.g. an embedded asset), so a large program costs the host
// nothing at compile time. Returns a run_result whose API (ok/console/call/[])
// is identical to the type-based path, because the value interpreter reuses the
// same value/environment/context machinery; the backing vm is kept alive so the
// script's functions stay callable (event handlers, onFrame, ...).
inline run_result run_value(std::string_view src, std::vector<binding> host = {}) {
	auto machine = std::make_shared<vp::vm>(vp::parse(src));
	for (binding & b : host) { machine->globals->declare(b.name, std::move(b.v)); }
	auto cx = rc<context>::make();
	run_result out{cx, machine->scope};
	try {
		machine->run(*cx);
	} catch (js_throw & t) {
		out.mark_failed(std::move(t.thrown));
	}
	out.keep_alive(machine);
	return out;
}

} // namespace ctjs

#endif
