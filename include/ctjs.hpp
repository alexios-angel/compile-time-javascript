#ifndef CTJS__HPP
#define CTJS__HPP

#include "ctlark.hpp"
#include "ctjs/grammar.hpp"
#include "ctjs/ast.hpp"
#include "ctjs/lower.hpp"
#include "ctjs/value.hpp"
#include "ctjs/builtins.hpp"
#include "ctjs/interp.hpp"
#include "ctjs/script.hpp"

// ctjs: JavaScript parsed while your code compiles, executed when it
// runs.
//
//   constexpr auto & app = ctjs::script<R"(
//       function greet(name) { return "hi " + name + "!"; }
//       let total = 0;
//       for (let i = 1; i <= 10; i++) { total += i; }
//       console.log("total", total);
//   )">;
//
//   auto out = app.run();
//   assert(out.ok());
//   assert(out.console() == "total 55\n");
//   assert(out["total"].to<int>() == 55);
//   assert(out.call("greet", "ctjs").to<std::string>() == "hi ctjs!");
//
//   static_assert(ctjs::script<"let x = 1;">.valid);
//   static_assert(!ctjs::is_valid<"let x = 1">); // semicolons required
//
// The SOURCE is a template argument: the grammar layer (ctlark,
// compile-time Lark) parses it during compilation - a syntax error is
// a compile error carrying a caret diagnostic - and lowering turns the
// parse tree into a type-level AST. The interpreter (interp.hpp) is
// specialized over that AST, so the C++ optimizer compiles each script
// into code generated for THAT script; at runtime there is no parsing,
// no bytecode, no dispatch loop - just the program, plus a dynamic
// value model with real closures, JS coercions and try/catch.
//
// Hosts inject native functions as globals (ctjs::binding +
// ctjs::native), and call script-defined functions from C++ through
// run_result::call - the seam a compile-time browser hangs DOM APIs on.

namespace ctjs {

// does the source parse as ctjs's JavaScript subset? (after ASI normalisation)
CTLL_EXPORT template <CTJS_STRING_INPUT Src> constexpr bool is_valid =
	ctlark::is_valid<detail::js_grammar, detail::asi_src<Src>, detail::js_start>;

// what failed and where, when it does not: kind, byte offset, line,
// column and the expected terminals (kind none = the syntax is fine)
CTLL_EXPORT template <CTJS_STRING_INPUT Src> constexpr ctlark::error_info_t error_info() noexcept {
	return ctlark::error_info<detail::js_grammar, detail::asi_src<Src>, detail::js_start>();
}

// the rendered diagnostic - location, snippet with a caret, expected
// terminals - as a static string ("" when the syntax is fine)
CTLL_EXPORT template <CTJS_STRING_INPUT Src> constexpr std::string_view error_message() noexcept {
	return ctlark::error_message<detail::js_grammar, detail::asi_src<Src>, detail::js_start>();
}

// the ctlark debugging toolbox with the JavaScript grammar baked in
namespace debug {

CTLL_EXPORT template <CTJS_STRING_INPUT Src, size_t Cap = 4096> constexpr auto traced_parse() noexcept {
	return ctlark::debug::traced_parse<detail::js_grammar, Src, detail::js_start, Cap>();
}

CTLL_EXPORT template <CTJS_STRING_INPUT Src> constexpr std::string_view dump_tokens() noexcept {
	return ctlark::debug::dump_tokens<detail::js_grammar, Src, detail::js_start>();
}

CTLL_EXPORT constexpr std::string_view dump_grammar() noexcept {
	return ctlark::debug::dump_grammar<detail::js_grammar>();
}

CTLL_EXPORT template <size_t MaxTokens = 1024>
ctlark::debug::runtime_result parse_runtime(std::string_view in) {
	return ctlark::debug::parse_runtime<detail::js_grammar, MaxTokens>(in, "start");
}

} // namespace debug

} // namespace ctjs

#endif
