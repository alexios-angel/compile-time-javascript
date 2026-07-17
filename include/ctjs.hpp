#ifndef CTJS__HPP
#define CTJS__HPP

#include "ctlark.hpp"
#include "ctjs/grammar.hpp"
#include "ctjs/types.hpp"
#include "ctjs/entities.hpp"
#include "ctjs/bind.hpp"
#include "ctjs/treebuild.hpp"
#include "ctjs/serialize.hpp"
#include "ctjs/views.hpp"

// ctjs: compile-time HTML5.
//
//   constexpr auto doc = ctjs::parse<R"(
//       <!DOCTYPE html>
//       <title>Hi</title>
//       <ul id=nav>
//           <li>Docs
//           <li>Code
//       </ul>)">();
//
//   static_assert(doc.name() == "html");
//   static_assert(doc.get<"head">().get<"title">().text() == "Hi");
//   static_assert(doc.get<"body">().get<"ul">().count<"li">() == 2);
//   static_assert(ctjs::is_valid<"<p>fragments are fine">);
//   static_assert(!ctjs::is_valid<"<b><i>misnested</b></i>">);
//
// The document is parsed while your code compiles, and the result is a
// TYPE - html > (head, body), like a browser DOM - whose accessors are
// all constexpr. HTML5's conveniences are understood: void elements
// (<br>), optional end tags (<li>, <p>, <td>...), implied
// <html>/<head>/<body>, case-insensitive names, boolean and unquoted
// attributes, DOCTYPE, raw-text <script>/<style> and RCDATA
// <title>/<textarea>, named and numeric character references. Author
// MISTAKES are compile errors (or `false` from is_valid): a stray or
// crossing close tag, a duplicate attribute, <div/>.
//
// The grammar layer is ctlark (compile-time Lark): grammar.hpp lexes
// the document into a FLAT chunk stream - HTML tag nesting is not
// context-free - bind.hpp lowers chunks (names, attributes, character
// references), and treebuild.hpp runs the HTML5 tree-construction
// logic that a browser's parser would, at compile time.

namespace ctjs {

#if CTLL_CNTTP_COMPILER_CHECK
#define CTJS_STRING_INPUT ctll::fixed_string
#else
// C++17: pass a constexpr ctll::fixed_string variable with linkage
#define CTJS_STRING_INPUT const auto &
#endif

namespace detail {

// grammar validity is a given (static_assert in grammar.hpp); input
// validity is the parse plus tree construction's well-formedness walk
template <CTJS_STRING_INPUT input> constexpr bool valid_document() noexcept {
	if constexpr (!ctlark::is_valid<html_grammar, input, html_start>) {
		return false;
	} else {
		return treebuild<decltype(ctlark::parse<html_grammar, input, html_start>())>::ok;
	}
}

} // namespace detail

// does the input parse as acceptable HTML5 (within the supported subset)?
CTLL_EXPORT template <CTJS_STRING_INPUT input> constexpr bool is_valid =
	detail::valid_document<input>();

// what failed and where, when it does not: kind, byte offset, line,
// column and the expected terminals (kind none = the syntax is fine)
CTLL_EXPORT template <CTJS_STRING_INPUT input> constexpr ctlark::error_info_t error_info() noexcept {
	return ctlark::error_info<detail::html_grammar, input, detail::html_start>();
}

// the rendered diagnostic - location, snippet with a caret, expected
// terminals - as a static string ("" when the syntax is fine)
CTLL_EXPORT template <CTJS_STRING_INPUT input> constexpr std::string_view error_message() noexcept {
	return ctlark::error_message<detail::html_grammar, input, detail::html_start>();
}

// why tree construction rejected a document that PARSES - a stray or
// crossing close tag, a duplicate attribute, a self-closed non-void;
// reason none when the document is valid or the syntax already failed
CTLL_EXPORT template <CTJS_STRING_INPUT input> constexpr bind_error_t bind_error() noexcept {
	if constexpr (!ctlark::is_valid<detail::html_grammar, input, detail::html_start>) {
		return bind_error_t{};
	} else {
		return detail::treebuild<decltype(ctlark::parse<detail::html_grammar, input, detail::html_start>())>::fail;
	}
}

// parse the input into its html root element; invalid HTML fails to
// compile. The result is always html > (head, body): fragments land in
// body, metadata elements in head, like a browser DOM.
CTLL_EXPORT template <CTJS_STRING_INPUT input> constexpr auto parse() noexcept {
#ifdef CTLARK_VERBOSE_ERRORS
	(void)ctlark::verbose_report<detail::html_grammar, input, detail::html_start>();
#endif
	static_assert(ctlark::is_valid<detail::html_grammar, input, detail::html_start>,
	              "ctjs: the input is not lexable/parsable HTML - print ctjs::error_message<input>() "
	              "for the location and the expected tokens");
	static_assert(!ctlark::is_valid<detail::html_grammar, input, detail::html_start> || is_valid<input>,
	              "ctjs: the input parses but is not acceptable HTML (stray or crossing close tag, "
	              "duplicate attribute, or self-closed non-void element) - print "
	              "ctjs::bind_error<input>() for the reason");
	if constexpr (is_valid<input>) {
		using built = detail::treebuild<decltype(ctlark::parse<detail::html_grammar, input, detail::html_start>())>;
		return typename built::type{};
	} else {
		return element<text<>, ctll::list<>>{};
	}
}

// the ctlark debugging toolbox with the HTML grammar baked in: traced
// parses (also runnable at runtime under a debugger), runtime inputs
// against the compile-time tables, token and grammar dumps
namespace debug {

CTLL_EXPORT template <CTJS_STRING_INPUT input, size_t Cap = 4096> constexpr auto traced_parse() noexcept {
	return ctlark::debug::traced_parse<detail::html_grammar, input, detail::html_start, Cap>();
}

CTLL_EXPORT template <CTJS_STRING_INPUT input> constexpr std::string_view dump_tokens() noexcept {
	return ctlark::debug::dump_tokens<detail::html_grammar, input, detail::html_start>();
}

CTLL_EXPORT constexpr std::string_view dump_grammar() noexcept {
	return ctlark::debug::dump_grammar<detail::html_grammar>();
}

CTLL_EXPORT template <size_t MaxTokens = 1024>
ctlark::debug::runtime_result parse_runtime(std::string_view in) {
	return ctlark::debug::parse_runtime<detail::html_grammar, MaxTokens>(in, "start");
}

} // namespace debug

} // namespace ctjs

#endif
