#ifndef CTJS__VPARSE__HPP
#define CTJS__VPARSE__HPP

#include <cstddef>

#include <cstdint>
#include <string_view>
#include <vector>

// Parse-by-VALUE for ctjs.
//
// The established ctjs front end parses BY TYPE: ctlark's Earley chart and the
// parse tree are encoded in the C++ type system. That is O(n^2+) in the input
// AND pays a template instantiation per node - a real 65 KB script exceeds the
// constexpr budget outright (see experiments/parse_by_value.cpp for the data).
//
// This is the value alternative: a constexpr recursive-descent / Pratt parser
// that scans the source ONCE (O(n)) and emits a FLAT VALUE AST - a std::vector
// of `node`s with child indices, zero types per node. It is `constexpr`, so it
// still runs during constant evaluation (the compile-time-browser identity is
// preserved), but as a VALUE computation on the compiler's fast path; it also
// runs verbatim at runtime. A value tree-walking interpreter over this AST
// (vinterp.hpp, forthcoming) will replace the type-specialised program_runner.
//
// Phase 1 (this file): lexer + expression/statement/function/class parser.

namespace ctjs::vp {

// ---------------------------------------------------------------------------
// tokens
// ---------------------------------------------------------------------------
enum class tk : std::uint8_t {
	end, ident, kw, num, str, tmpl_full, tmpl_head, tmpl_mid, tmpl_tail, regex, punct
};

struct token {
	tk kind = tk::end;
	std::string_view s;    // the lexeme (a view into the source)
};

constexpr bool is_id_start(char32_t c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$' ||
	       static_cast<unsigned char>(c) > 127;
}
constexpr bool is_id_part(char32_t c) { return is_id_start(c) || (c >= '0' && c <= '9'); }
constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

// the reserved words ctjs recognises (matches grammar.hpp's IDENT exclusion)
inline constexpr std::string_view keywords[] = {
    "await", "break", "case", "catch", "class", "const", "continue", "default",
    "delete", "do", "else", "extends", "false", "finally", "for", "function",
    "if", "in", "instanceof", "let", "new", "null", "return", "super", "switch",
    "this", "throw", "true", "try", "typeof", "var", "void", "while", "with",
    "yield", "async", "of", "static", "get", "set", "import", "export"};

// CONTEXTUAL keywords: reserved only in the position that gives them meaning,
// and an ordinary identifier everywhere else. `function set(...)` and
// `const of = 1` are both legal JavaScript, and p5.js has both. Each of these
// is recognised by the construct that cares BEFORE anything general looks at
// it - for..of checks `of`, a class body checks `static`/`get`/`set` - so by
// the time a name is being read, it is a name.
constexpr bool is_contextual_keyword(std::string_view w) {
	return w == "get" || w == "set" || w == "of" || w == "static";
}

constexpr bool is_keyword(std::string_view w) {
	for (std::string_view k : keywords) { if (k == w) { return true; } }
	return false;
}

// multi-char operators, longest-first (so longest-match wins). The second '?'
// in "?\?=" is escaped so it can't form the "??=" trigraph.
inline constexpr std::string_view operators[] = {
    ">>>=", "===", "!==", "**=", "<<=", ">>=", "&&=", "||=", "?\?=", "...", ">>>",
    "==", "!=", "<=", ">=", "&&", "||", "?\?", "?.", "=>", "++", "--", "+=", "-=",
    "*=", "/=", "%=", "&=", "|=", "^=", "**", "<<", ">>",
    "{", "}", "(", ")", "[", "]", ";", ",", ".", "?", ":", "=", "+", "-", "*", "/",
    "%", "<", ">", "!", "~", "&", "|", "^"};

// A token after which a '/' is DIVISION (else it begins a regex literal).
constexpr bool div_follows(const token & t) {
	if (t.kind == tk::num || t.kind == tk::str || t.kind == tk::tmpl_full ||
	    t.kind == tk::tmpl_tail || t.kind == tk::regex) {
		return true;
	}
	if (t.kind == tk::ident) { return true; }
	if (t.kind == tk::kw) {
		return t.s == "this" || t.s == "super" || t.s == "true" || t.s == "false" || t.s == "null";
	}
	if (t.kind == tk::punct) { return t.s == ")" || t.s == "]" || t.s == "++" || t.s == "--"; }
	return false;
}

// What the lexer had to throw away. A byte matching no operator is SKIPPED
// rather than reported, which keeps the lexer total - but it also means `#x`
// silently becomes `x`, and a caller that never hears about it cannot tell a
// private field from a public one. Counting them costs nothing and turns a
// silent aliasing bug into a number.
struct lex_report {
	std::size_t skipped = 0;      // bytes matching no token at all
	std::size_t first_skip = 0;   // offset of the first one
};

// Lex the whole source into a token vector (comments and whitespace dropped).
constexpr std::vector<token> lex(std::string_view src, lex_report * report = nullptr) {
	std::vector<token> out;
	const std::size_t n = src.size();
	std::size_t i = 0;
	auto has_div = [&]() { return !out.empty() && div_follows(out.back()); };

	while (i < n) {
		char c = src[i];
		// whitespace
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') { ++i; continue; }
		// comments
		if (c == '/' && i + 1 < n && src[i + 1] == '/') { i += 2; while (i < n && src[i] != '\n') { ++i; } continue; }
		if (c == '/' && i + 1 < n && src[i + 1] == '*') {
			i += 2; while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) { ++i; }
			i = (i + 1 < n) ? i + 2 : n; continue;
		}
		std::size_t start = i;
		// identifier / keyword
		//
		// A PRIVATE NAME lexes as one identifier, `#` and all. It was skipped as
		// an unknown byte, which is worse than an error: `this.#count` became
		// `this.count`, so a private field silently aliased a public one of the
		// same name and nothing anywhere said so. Keeping the `#` in the lexeme
		// makes the two distinct names again, which is all the privacy any
		// program here actually depends on. `#` leads and never follows, so it
		// stays out of is_id_part and `a#b` is still two tokens.
		const bool private_name =
		    c == '#' && i + 1 < n && is_id_start(static_cast<unsigned char>(src[i + 1]));
		if (private_name || is_id_start(static_cast<unsigned char>(c))) {
			if (private_name) { ++i; }
			while (i < n && is_id_part(static_cast<unsigned char>(src[i]))) { ++i; }
			std::string_view w = src.substr(start, i - start);
			out.push_back({!private_name && is_keyword(w) ? tk::kw : tk::ident, w});
			continue;
		}
		// number
		if (is_digit(c) || (c == '.' && i + 1 < n && is_digit(src[i + 1]))) {
			if (c == '0' && i + 1 < n && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
				i += 2; while (i < n && is_id_part(static_cast<unsigned char>(src[i]))) { ++i; }
			} else {
				while (i < n && (is_digit(src[i]) || src[i] == '.')) { ++i; }
				if (i < n && (src[i] == 'e' || src[i] == 'E')) {
					++i; if (i < n && (src[i] == '+' || src[i] == '-')) { ++i; }
					while (i < n && is_digit(src[i])) { ++i; }
				}
			}
			out.push_back({tk::num, src.substr(start, i - start)});
			continue;
		}
		// string
		if (c == '"' || c == '\'') {
			char q = c; ++i;
			while (i < n && src[i] != q) { if (src[i] == '\\' && i + 1 < n) { i += 2; } else { ++i; } }
			if (i < n) { ++i; }
			out.push_back({tk::str, src.substr(start, i - start)});
			continue;
		}
		// template literal (whole thing as one token for now; ${} kept inside)
		if (c == '`') {
			++i; std::int32_t depth = 0;
			while (i < n) {
				char d = src[i];
				if (d == '\\' && i + 1 < n) { i += 2; continue; }
				if (depth == 0 && d == '`') { ++i; break; }
				if (depth == 0 && d == '$' && i + 1 < n && src[i + 1] == '{') { depth = 1; i += 2; continue; }
				if (depth > 0 && d == '{') { ++depth; ++i; continue; }
				if (depth > 0 && d == '}') { --depth; ++i; continue; }
				++i;
			}
			out.push_back({tk::tmpl_full, src.substr(start, i - start)});
			continue;
		}
		// regex vs division
		if (c == '/' && !has_div()) {
			++i; bool cls = false;
			while (i < n) {
				char d = src[i];
				if (d == '\\' && i + 1 < n) { i += 2; continue; }
				if (d == '\n') { break; }
				if (d == '[') { cls = true; ++i; continue; }
				if (d == ']') { cls = false; ++i; continue; }
				if (d == '/' && !cls) { ++i; break; }
				++i;
			}
			while (i < n && is_id_part(static_cast<unsigned char>(src[i]))) { ++i; }
			out.push_back({tk::regex, src.substr(start, i - start)});
			continue;
		}
		// punctuator: longest match among ctjs's operators
		std::string_view matched;
		for (std::string_view op : operators) {
			// FIRST BYTE FIRST. substr() + compare on all 56 was the cost.
			if (op[0] != src[i]) { continue; }
			if (i + op.size() <= n && src.substr(i, op.size()) == op) { matched = op; break; }
		}
		if (matched.empty()) {   // skip unknown byte, but say so
			if (report != nullptr) {
				if (report->skipped == 0) { report->first_skip = i; }
				++report->skipped;
			}
			++i;
			continue;
		}
		out.push_back({tk::punct, src.substr(i, matched.size())});
		i += matched.size();
	}
	out.push_back({tk::end, {}});
	return out;
}

// ---------------------------------------------------------------------------
// value AST
// ---------------------------------------------------------------------------
enum class nk : std::uint8_t {
	// literals / primary
	num, str, tmpl, regex, ident, true_lit, false_lit, null_lit, this_lit, super_lit,
	array, object, prop, spread, arrow, func_expr,
	// operators
	unary, update, binary, logical, assign, ternary, seq,
	member, index, call, new_expr, opt_member, opt_index, opt_call, tagged,
	// statements
	program, block, var_decl, declarator, empty, expr_stmt,
	if_stmt, for_stmt, forof_stmt, while_stmt, do_stmt,
	return_stmt, break_stmt, continue_stmt, throw_stmt, labeled,
	try_stmt, catch_clause, switch_stmt, case_clause,
	func_decl, class_decl, class_member, param, yield_expr,
	// ES modules. APPENDED, like the destructuring kinds below and for the same
	// reason: a consumer that does not know a kind must fall through its
	// default rather than silently mean something else.
	//
	//   import_decl   text = specifier, list = import_spec
	//   import_spec   text = LOCAL name, c: 0 named, 1 default, 2 namespace,
	//                 a = a str node holding the IMPORTED name when renamed
	//   export_decl   a = the declaration, list = export_spec,
	//                 text = specifier for a re-export, c: 1 = default
	//   export_spec   text = LOCAL name, a = a str node holding the EXPORTED
	//                 name when renamed
	//   import_meta   `import.meta`
	//   dynamic_import  a = the specifier expression
	import_decl, import_spec, export_decl, export_spec, import_meta, dynamic_import,
	// destructuring patterns. APPENDED, not inserted: the interpreter in this
	// repo switches on these values, and a consumer that does not know a kind
	// should fall through its default rather than silently mean something else.
	//
	//   array_pattern   list = elements, -1 for a hole
	//   object_pattern  list = entries (pattern_prop or rest_element)
	//   pattern_prop    text = key, a = computed key (d & 2), b = target
	//   assign_pattern  a = target, b = the default
	//   rest_element    a = target
	array_pattern, object_pattern, pattern_prop, assign_pattern, rest_element,
	//   new_target      `new.target`, the meta-property. No children: it reads
	//                   the constructor the enclosing frame was invoked with,
	//                   or undefined outside a construct call.
	//
	// APPENDED for the same reason the note above gives. `new.target` reached
	// this parser as a parse error - `new` was followed by `.`, which is not a
	// callee, and the message was "expression" - and it is not exotic: every
	// transpiler emits it, and Babylon.js 9.18.2 uses it in its decorator
	// metadata support, which is the first thing in that bundle this parser
	// stopped on.
	new_target
};

struct node {
	nk kind;
	std::string_view text;                 // op / name / literal lexeme / flag
	std::int32_t a = -1, b = -1, c = -1, d = -1;    // fixed child slots
	std::int32_t list = -1, list_len = 0;           // variable-arity children (into ast::pool)
	// WHERE IT CAME FROM, as byte offsets into the source.
	//
	// Set for FUNCTIONS only, because that is what needs it: `f.toString()`
	// has to hand back the text a function was written as, and an engine with
	// no answer to that cannot run a library that reads its own source - which
	// p5.js's error system does. The same span is what an Error's stack needs
	// to name a line.
	//
	// Zero on every other kind. A token's lexeme is already a view INTO the
	// source, so both ends are a subtraction rather than any bookkeeping.
	std::uint32_t begin = 0, end = 0;
};

struct ast {
	std::vector<node> nodes;
	std::vector<std::int32_t> pool;                 // child-index pool for list nodes
	std::int32_t root = -1;
	bool ok = true;
	std::string_view error;
	std::size_t error_tok = 0;
	// `error_tok` indexes a token vector the caller never sees, so on its own it
	// cannot be turned into a line and column. The offset can: every token's
	// lexeme is a view INTO the source, so parse() resolves it once and a
	// caller holding only the source can say where it stopped.
	std::size_t error_offset = 0;
	std::size_t skipped_bytes = 0;    // see lex_report
	std::size_t first_skip_offset = 0;

	constexpr std::int32_t add(node nd) { nodes.push_back(nd); return static_cast<std::int32_t>(nodes.size()) - 1; }
	constexpr std::int32_t add_list(const std::vector<std::int32_t> & kids) {
		std::int32_t at = static_cast<std::int32_t>(pool.size());
		for (std::int32_t k : kids) { pool.push_back(k); }
		return at;
	}
};

// ---------------------------------------------------------------------------
// parser: Pratt expressions + recursive-descent statements. Semicolons are
// optional (recursive descent knows statement boundaries structurally, so ASI
// is free). Errors are recorded, not thrown, so it stays constexpr-usable.
// ---------------------------------------------------------------------------
struct parser {
	const std::vector<token> & t;
	ast & a;
	std::size_t p = 0;
	std::string_view src{};   // for node spans; see node::begin

	// The offset a token starts at, and the offset just past the one before
	// the current position - which together bound everything consumed so far.
	constexpr std::uint32_t offset_at(std::size_t token_index) const {
		if (src.empty() || token_index >= t.size()) { return 0; }
		const std::string_view lexeme = t[token_index].s;
		if (lexeme.data() == nullptr) { return static_cast<std::uint32_t>(src.size()); }
		return static_cast<std::uint32_t>(lexeme.data() - src.data());
	}
	constexpr std::uint32_t offset_consumed() const {
		if (src.empty() || p == 0) { return 0; }
		const std::string_view lexeme = t[p - 1].s;
		if (lexeme.data() == nullptr) { return static_cast<std::uint32_t>(src.size()); }
		return static_cast<std::uint32_t>(
		    static_cast<std::size_t>(lexeme.data() - src.data()) + lexeme.size());
	}

	constexpr const token & cur() const { return t[p]; }
	constexpr const token & nxt() const { return p + 1 < t.size() ? t[p + 1] : t.back(); }
	constexpr bool at_end() const { return cur().kind == tk::end; }
	constexpr bool is_p(std::string_view s) const { return cur().kind == tk::punct && cur().s == s; }
	constexpr bool is_kw(std::string_view s) const { return cur().kind == tk::kw && cur().s == s; }
	constexpr void advance() { if (p + 1 < t.size()) { ++p; } }
	constexpr bool eat_p(std::string_view s) { if (is_p(s)) { advance(); return true; } return false; }
	constexpr bool eat_kw(std::string_view s) { if (is_kw(s)) { advance(); return true; } return false; }
	// `from` and `as` are NOT keywords - they are ordinary identifiers that mean
	// something only inside an import or export, which is why `from` stays
	// usable as a variable name. So they are matched by SPELLING rather than by
	// token kind; eat_kw would never see them.
	constexpr bool is_word(std::string_view s) const {
		return (cur().kind == tk::ident || cur().kind == tk::kw) && cur().s == s;
	}
	constexpr bool eat_word(std::string_view s) {
		if (is_word(s)) { advance(); return true; }
		return false;
	}

	constexpr std::int32_t fail(std::string_view msg) {
		if (a.ok) { a.ok = false; a.error = msg; a.error_tok = p; }
		return -1;
	}
	constexpr void expect_p(std::string_view s) { if (!eat_p(s)) { fail(s); } }
	constexpr void semi() { eat_p(";"); }   // optional (ASI)

	// --- binding powers ------------------------------------------------------
	static constexpr bool is_assign_op(std::string_view o) {
		return o == "=" || o == "+=" || o == "-=" || o == "*=" || o == "/=" || o == "%=" ||
		       o == "**=" || o == "<<=" || o == ">>=" || o == ">>>=" || o == "&=" || o == "|=" ||
		       o == "^=" || o == "&&=" || o == "||=" || o == "?\?=";
	}
	// left binding power of the current (infix) token; -1 if not infix
	constexpr std::int32_t lbp() const {
		if (cur().kind == tk::kw) {
			if (cur().s == "in" || cur().s == "instanceof") { return 13; }
			return -1;
		}
		if (cur().kind != tk::punct) { return -1; }
		std::string_view o = cur().s;
		// THE COMMA OPERATOR, at the loosest binding power there is.
		//
		// This is safe precisely because every comma-SEPARATED context already
		// parses its elements at 2 or tighter - argument lists, array elements,
		// object values, parameter defaults, declarator initialisers - so a
		// comma there still ends the element instead of joining it to the next.
		// Only the positions that parse at 0 see a sequence, and those are the
		// ones where JavaScript says it is one: a for clause, an expression
		// statement, a parenthesised group.
		if (o == ",") { return 1; }
		if (is_assign_op(o)) { return 2; }
		if (o == "?") { return 4; }
		if (o == "??") { return 6; }
		if (o == "||") { return 7; }
		if (o == "&&") { return 8; }
		if (o == "|") { return 9; }
		if (o == "^") { return 10; }
		if (o == "&") { return 11; }
		if (o == "==" || o == "!=" || o == "===" || o == "!==") { return 12; }
		if (o == "<" || o == ">" || o == "<=" || o == ">=") { return 13; }
		if (o == "<<" || o == ">>" || o == ">>>") { return 14; }
		if (o == "+" || o == "-") { return 15; }
		if (o == "*" || o == "/" || o == "%") { return 16; }
		if (o == "**") { return 17; }
		return -1;
	}

	// --- expressions ---------------------------------------------------------
	constexpr std::int32_t expr(std::int32_t min_bp) {
		std::int32_t left = unary();
		for (;;) {
			std::int32_t bp = lbp();
			if (bp < 0 || bp < min_bp) { break; }
			std::string_view o = cur().s;
			if (cur().kind == tk::punct && o == "?") {           // ternary
				advance();
				std::int32_t cons = expr(0);
				expect_p(":");
				std::int32_t alt = expr(2);                                // right side down to assignment
				node nd{nk::ternary, "?:"}; nd.a = left; nd.b = cons; nd.c = alt;
				left = a.add(nd);
				continue;
			}
			if (cur().kind == tk::punct && is_assign_op(o)) {    // assignment (right-assoc)
				advance();
				std::int32_t right = expr(bp);
				node nd{nk::assign, o}; nd.a = left; nd.b = right;
				left = a.add(nd);
				continue;
			}
			if (cur().kind == tk::punct && o == ",") {           // sequence
				advance();
				node nd{nk::seq, ","}; nd.a = left; nd.b = expr(2);
				left = a.add(nd);
				continue;
			}
			// binary / logical / relational (left-assoc; ** right-assoc)
			bool logical = (o == "&&" || o == "||" || o == "??");
			advance();
			std::int32_t right = expr(o == "**" ? bp : bp + 1);
			node nd{logical ? nk::logical : nk::binary, o}; nd.a = left; nd.b = right;
			left = a.add(nd);
		}
		return left;
	}

	constexpr std::int32_t unary() {
		if (cur().kind == tk::punct) {
			std::string_view o = cur().s;
			if (o == "!" || o == "-" || o == "+" || o == "~") {
				advance(); node nd{nk::unary, o}; nd.a = unary(); return a.add(nd);
			}
			if (o == "++" || o == "--") {
				advance(); node nd{nk::update, o}; nd.a = unary(); nd.b = 1; /*prefix*/ return a.add(nd);
			}
		}
		// `import(...)` and `import.meta` are the two places `import` is an
		// EXPRESSION rather than a declaration. Both have to be recognised here
		// or `import` reads as an ordinary identifier - which is exactly what it
		// did, and why `import('./m.js')` failed at run time with "`import` is
		// undefined, not a function".
		if (cur().kind == tk::kw && cur().s == "import") {
			if (nxt().kind == tk::punct && nxt().s == "(") {
				advance();
				expect_p("(");
				node nd{nk::dynamic_import, ""};
				nd.a = expr(2);
				// A trailing comma and an options argument are both legal; the
				// specifier is what matters and the rest is skipped rather than
				// refused.
				while (eat_p(",") && !is_p(")") && !at_end()) { (void)expr(2); }
				expect_p(")");
				return a.add(nd);
			}
			if (nxt().kind == tk::punct && nxt().s == ".") {
				advance();
				advance();
				if (cur().s != "meta") {
					fail("import. must be followed by meta");
					return -1;
				}
				advance();
				return a.add({nk::import_meta, "import.meta"});
			}
		}
		if (cur().kind == tk::kw && cur().s == "yield") {
			// yield [expr] - only meaningful inside a generator body; the
			// interpreter enforces that at run time. yield* delegates in the
			// eager engine by yielding the operand value itself.
			advance();
			eat_p("*");
			node y{nk::yield_expr, ""};
			if (!is_p(";") && !is_p(")") && !is_p("}") && !is_p(",") && !is_p("]") && !at_end()) { y.a = expr(2); }
			return a.add(y);
		}
		if (cur().kind == tk::kw) {
			std::string_view o = cur().s;
			if (o == "typeof" || o == "delete" || o == "void" || o == "await") {
				advance(); node nd{nk::unary, o}; nd.a = unary(); return a.add(nd);
			}
		}
		return postfix();
	}

	constexpr std::int32_t postfix() {
		std::int32_t e = primary();
		for (;;) {
			if (is_p(".")) { advance(); node nd{nk::member, cur().s}; nd.a = e; advance(); e = a.add(nd); }
			else if (is_p("?.")) {
				advance();
				if (is_p("(")) { node nd{nk::opt_call, ""}; nd.a = e; nd.list = args(nd.list_len); e = a.add(nd); }
				else if (is_p("[")) { advance(); node nd{nk::opt_index, ""}; nd.a = e; nd.b = expr(0); expect_p("]"); e = a.add(nd); }
				else { node nd{nk::opt_member, cur().s}; nd.a = e; advance(); e = a.add(nd); }
			}
			else if (is_p("[")) { advance(); node nd{nk::index, ""}; nd.a = e; nd.b = expr(0); expect_p("]"); e = a.add(nd); }
			else if (is_p("(")) { node nd{nk::call, ""}; nd.a = e; nd.list = args(nd.list_len); e = a.add(nd); }
			else if (is_p("++") || is_p("--")) { node nd{nk::update, cur().s}; nd.a = e; nd.b = 0; /*postfix*/ advance(); e = a.add(nd); }
			else { break; }
		}
		return e;
	}

	// call/array argument list starting at '(' or '['; returns pool offset, sets len
	constexpr std::int32_t args(std::int32_t & len) {
		std::string_view open = cur().s, close = (open == "(") ? ")" : "]";
		advance();
		std::vector<std::int32_t> kids;
		while (!is_p(close) && !at_end()) {
			// An ELISION: `[, ref]` and `[a, , b]` are legal array literals with
			// a hole, and the hole is a real element position - which matters
			// most when the literal is being used as an assignment pattern and
			// the hole means "skip this one".
			if (close == "]" && is_p(",")) { advance(); kids.push_back(-1); continue; }
			if (is_p("...")) { advance(); node nd{nk::spread, ""}; nd.a = expr(2); kids.push_back(a.add(nd)); }
			else { kids.push_back(expr(2)); }
			if (!eat_p(",")) { break; }
		}
		expect_p(close);
		len = static_cast<std::int32_t>(kids.size());
		return a.add_list(kids);
	}

	// the constructor of a `new`: a member expression with NO call (the call
	// after it, if any, supplies the new's arguments; further chains apply to
	// the constructed object)
	constexpr std::int32_t new_callee() {
		std::int32_t e = primary();
		for (;;) {
			if (is_p(".")) { advance(); node nd{nk::member, cur().s}; nd.a = e; advance(); e = a.add(nd); }
			else if (is_p("[")) { advance(); node nd{nk::index, ""}; nd.a = e; nd.b = expr(0); expect_p("]"); e = a.add(nd); }
			else { break; }
		}
		return e;
	}

	constexpr std::int32_t primary() {
		const token & c = cur();
		if (c.kind == tk::num) { node nd{nk::num, c.s}; advance(); return a.add(nd); }
		if (c.kind == tk::str) { node nd{nk::str, c.s}; advance(); return a.add(nd); }
		if (c.kind == tk::tmpl_full) { node nd{nk::tmpl, c.s}; advance(); return a.add(nd); }
		if (c.kind == tk::regex) { node nd{nk::regex, c.s}; advance(); return a.add(nd); }
		if (c.kind == tk::ident) {
			// arrow with single param:  x => ...
			if (nxt().kind == tk::punct && nxt().s == "=>") { return arrow_single(); }
			node nd{nk::ident, c.s}; advance(); return a.add(nd);
		}
		if (c.kind == tk::kw) {
			if (c.s == "true") { advance(); return a.add({nk::true_lit, "true"}); }
			if (c.s == "false") { advance(); return a.add({nk::false_lit, "false"}); }
			if (c.s == "null") { advance(); return a.add({nk::null_lit, "null"}); }
			if (c.s == "this") { advance(); return a.add({nk::this_lit, "this"}); }
			if (c.s == "super") { advance(); return a.add({nk::super_lit, "super"}); }
			if (c.s == "new") {
				advance();
				// `new.target` BEFORE the callee, because `.` is not one. The
				// meta-property is the only place a `.` may follow `new`, so
				// this is a lookahead of exactly one token with no ambiguity to
				// resolve - and without it the callee parser met a `.` and
				// reported "expression", which is where Babylon.js stopped.
				if (is_p(".")) {
					advance();
					// `new.` may only be followed by `target`. Anything else is
					// a real error and says which word it got, rather than
					// falling back to the callee path and failing further on
					// with a message about something unrelated.
					// `cur()`, NOT `c`: that is a reference bound when primary()
					// was entered and two advances ago by now. The branch above
					// uses is_p() for the same reason.
					if (cur().kind != tk::ident || cur().s != "target") {
						fail("new. must be followed by target");
						return -1;
					}
					advance();
					return a.add({nk::new_target, "new.target"});
				}
				node nd{nk::new_expr, ""};
				nd.a = new_callee();                       // member-expr only (no trailing call)
				if (is_p("(")) { nd.list = args(nd.list_len); nd.d = 1; }
				return a.add(nd);                          // postfix() continues for `.m()` on the result
			}
			if (c.s == "function") { return func(true); }
			// A CLASS EXPRESSION. `const X = class {...}` is as ordinary as a
			// function expression, and without this the `class` fell through to
			// the bare-identifier path: the declaration became a read of a
			// global named `class`, and the body's members leaked out as
			// top-level statements. Nothing said so.
			if (c.s == "class") { return class_decl(true); }
			if (c.s == "async") {
				if (nxt().kind == tk::kw && nxt().s == "function") { advance(); return func(true, true); }
				// AN ASYNC ARROW. `async` fell through to being a bare
				// identifier, so `async (a, b) => {}` parsed as a CALL to
				// something named `async` and then met a `=>` it had nowhere to
				// put. Both spellings are checked by lookahead and the position
				// is restored if it turns out to be an ordinary use of the name.
				if (nxt().kind == tk::punct && nxt().s == "(") {
					const std::size_t save = p;
					advance();
					if (arrow_ahead()) {
						const std::int32_t r = paren_or_arrow();
						if (r >= 0) { a.nodes[static_cast<std::size_t>(r)].c = 1; }   // async
						return r;
					}
					p = save;
				}
				if (nxt().kind == tk::ident) {
					const std::size_t save = p;
					advance();
					if (nxt().kind == tk::punct && nxt().s == "=>") {
						const std::int32_t r = arrow_single();
						if (r >= 0) { a.nodes[static_cast<std::size_t>(r)].c = 1; }
						return r;
					}
					p = save;
				}
			}
			// A contextual keyword can be an arrow's single parameter too:
			// `swizzleSets.some(set => ...)` names one `set`.
			if (is_contextual_keyword(c.s) && nxt().kind == tk::punct && nxt().s == "=>") {
				return arrow_single();
			}
			// keyword used as a bare identifier (property contexts) - be lenient
			node nd{nk::ident, c.s}; advance(); return a.add(nd);
		}
		if (c.kind == tk::punct) {
			if (c.s == "(") { return paren_or_arrow(); }
			if (c.s == "[") { node nd{nk::array, ""}; nd.list = args(nd.list_len); return a.add(nd); }
			if (c.s == "{") { return object(); }
		}
		return fail("expression");
	}

	constexpr std::int32_t arrow_single() {
		const std::uint32_t span_begin = offset_at(p);
		std::vector<std::int32_t> ps;
		node pn{nk::param, cur().s}; ps.push_back(a.add(pn)); advance();   // ident
		expect_p("=>");
		node nd{nk::arrow, ""}; nd.list = a.add_list(ps); nd.list_len = 1;
		nd.a = arrow_body();
		nd.begin = span_begin;
		nd.end = offset_consumed();
		return a.add(nd);
	}

	// disambiguate "(expr)" from "(params) =>" by scanning to the matching ')'
	constexpr bool arrow_ahead() const {
		std::size_t q = p; std::int32_t depth = 0;
		for (; q < t.size(); ++q) {
			if (t[q].kind == tk::punct && t[q].s == "(") { ++depth; }
			else if (t[q].kind == tk::punct && t[q].s == ")") { if (--depth == 0) { break; } }
			else if (t[q].kind == tk::end) { return false; }
		}
		std::size_t after = q + 1;
		return after < t.size() && t[after].kind == tk::punct && t[after].s == "=>";
	}

	constexpr std::int32_t paren_or_arrow() {
		if (arrow_ahead()) {
			const std::uint32_t span_begin = offset_at(p);
			node nd{nk::arrow, ""};
			std::int32_t len = 0; nd.list = params(len); nd.list_len = len;
			expect_p("=>");
			nd.a = arrow_body();
			nd.begin = span_begin;
			nd.end = offset_consumed();
			return a.add(nd);
		}
		advance();                       // '('
		std::int32_t e = expr(0);
		expect_p(")");
		return e;
	}
	constexpr std::int32_t arrow_body() {
		if (is_p("{")) { return block(); }
		return expr(2);
	}

	// --- destructuring patterns ---------------------------------------------
	//
	// A binding position may hold a whole shape rather than a name. Everywhere
	// one is accepted used to take exactly one identifier token, so `const {a} =
	// o` read `{` AS THE NAME and the parser desynchronised from there - which
	// is what stopped seventeen of p5.js's modules, all of them at the first
	// destructuring in the file.
	[[nodiscard]] constexpr bool at_pattern() const { return is_p("[") || is_p("{"); }

	constexpr std::int32_t pattern() {
		if (is_p("[")) { return array_pattern(); }
		if (is_p("{")) { return object_pattern(); }
		node id{nk::ident, cur().s}; advance();
		return a.add(id);
	}

	// A target that may carry a default: `[a = 1]`, `{b: c = 2}`.
	constexpr std::int32_t pattern_target() {
		const std::int32_t target = pattern();
		if (eat_p("=")) {
			node nd{nk::assign_pattern, ""}; nd.a = target; nd.b = expr(2);
			return a.add(nd);
		}
		return target;
	}

	constexpr std::int32_t array_pattern() {
		expect_p("[");
		std::vector<std::int32_t> elements;
		while (!is_p("]") && !at_end()) {
			if (is_p(",")) { advance(); elements.push_back(-1); continue; }   // a hole
			if (is_p("...")) {
				advance();
				node r{nk::rest_element, ""}; r.a = pattern(); elements.push_back(a.add(r));
			} else {
				elements.push_back(pattern_target());
			}
			if (!eat_p(",")) { break; }
		}
		expect_p("]");
		node nd{nk::array_pattern, ""};
		nd.list = a.add_list(elements); nd.list_len = static_cast<std::int32_t>(elements.size());
		return a.add(nd);
	}

	constexpr std::int32_t object_pattern() {
		expect_p("{");
		std::vector<std::int32_t> entries;
		while (!is_p("}") && !at_end()) {
			if (is_p("...")) {
				advance();
				node r{nk::rest_element, ""}; r.a = pattern(); entries.push_back(a.add(r));
			} else {
				node e{nk::pattern_prop, ""}; e.d = 0;   // d defaults to -1; zero it first
				if (is_p("[")) { advance(); e.a = expr(0); expect_p("]"); e.d |= 2; }
				else if (cur().kind == tk::str || cur().kind == tk::num) {
					// a quoted or numeric key rides the computed path, the same
					// way an object LITERAL's does - evaluating the literal is
					// what cooks the quotes and escapes
					node k{cur().kind == tk::str ? nk::str : nk::num, cur().s}; advance();
					e.a = a.add(k); e.d |= 2;
				} else { e.text = cur().s; advance(); }
				if (eat_p(":")) { e.b = pattern_target(); }
				else {
					// shorthand: `{a}` and `{a = 1}` both bind the key's own name
					node id{nk::ident, e.text}; std::int32_t t = a.add(id);
					if (eat_p("=")) { node d{nk::assign_pattern, ""}; d.a = t; d.b = expr(2); t = a.add(d); }
					e.b = t;
				}
				entries.push_back(a.add(e));
			}
			if (!eat_p(",")) { break; }
		}
		expect_p("}");
		node nd{nk::object_pattern, ""};
		nd.list = a.add_list(entries); nd.list_len = static_cast<std::int32_t>(entries.size());
		return a.add(nd);
	}

	// parameter list at '(' -> pool; supports defaults, rest and patterns
	constexpr std::int32_t params(std::int32_t & len) {
		expect_p("(");
		std::vector<std::int32_t> ps;
		while (!is_p(")") && !at_end()) {
			if (is_p("...")) {
				advance();
				node nd{nk::param, ""}; nd.d = 1; /*rest*/
				if (at_pattern()) { nd.b = pattern(); } else { nd.text = cur().s; advance(); }
				ps.push_back(a.add(nd));
			}
			else {
				// `b` is the pattern when the parameter is a shape rather than a
				// name; `text` stays empty in that case and the compiler binds
				// through the pattern instead.
				node nd{nk::param, ""};
				if (at_pattern()) { nd.b = pattern(); } else { nd.text = cur().s; advance(); }
				if (eat_p("=")) { nd.a = expr(2); }
				ps.push_back(a.add(nd));
			}
			if (!eat_p(",")) { break; }
		}
		expect_p(")");
		len = static_cast<std::int32_t>(ps.size());
		return a.add_list(ps);
	}

	constexpr std::int32_t object() {
		expect_p("{");
		std::vector<std::int32_t> props;
		while (!is_p("}") && !at_end()) {
			if (is_p("...")) { advance(); node sp{nk::spread, ""}; sp.a = expr(2); props.push_back(a.add(sp)); }
			else {
				const std::uint32_t member_begin = offset_at(p);
			node pr{nk::prop, ""};
				pr.d = 0;   // bit0 = computed key, bit2 = accessor is a SETTER
				// `{ *g() {} }` and `{ async *g() {} }`. Neither parsed: the
				// star was not expected anywhere in an object literal, so the
				// key parse took `*` as the property name and the `(` after it
				// was a syntax error.
				const bool pasync = is_kw("async") && !(nxt().kind == tk::punct &&
				                                        (nxt().s == "(" || nxt().s == ":" ||
				                                         nxt().s == "," || nxt().s == "}"));
				if (pasync) { advance(); }
				const bool pgen = eat_p("*");
				// key ("quoted" and 1-numeric keys ride the computed path -
				// evaluating the literal cooks quotes/escapes into the name)
				if (is_p("[")) { advance(); pr.a = expr(0); expect_p("]"); pr.d = 1; /*computed*/ }
				else if (cur().kind == tk::str) { node k{nk::str, cur().s}; advance(); pr.a = a.add(k); pr.d = 1; }
				else if (cur().kind == tk::num) { node k{nk::num, cur().s}; advance(); pr.a = a.add(k); pr.d = 1; }
				else { pr.text = cur().s; advance(); }
				if ((pr.text == "get" || pr.text == "set") &&
				    !is_p("(") && !is_p(":") && !is_p(",") && !is_p("}")) {
					// accessor:  get name() {...} / set name(v) {...}  (the
					// name may itself be computed) - mirrors the class path
					const bool is_setter = pr.text == "set";
					if (is_p("[")) { advance(); pr.a = expr(0); expect_p("]"); pr.d = 1; pr.text = ""; }
					else { pr.text = cur().s; advance(); }
					std::int32_t len = 0; std::int32_t pl = params(len);
					std::int32_t body = block();
					node fn{nk::func_expr, ""}; fn.list = pl; fn.list_len = len; fn.a = body;
					fn.begin = member_begin; fn.end = offset_consumed();
					pr.b = a.add(fn); pr.c = 3; /*accessor*/ if (is_setter) { pr.d |= 4; }
				} else if (is_p("(")) {                 // method shorthand
					std::int32_t len = 0; std::int32_t pl = params(len);
					std::int32_t body = block();
					node fn{nk::func_expr, ""}; fn.list = pl; fn.list_len = len; fn.a = body;
					fn.begin = member_begin; fn.end = offset_consumed();
					if (pasync || pgen) { fn.c = (pasync ? 1 : 0) | (pgen ? 2 : 0); }
					pr.b = a.add(fn); pr.c = 1; /*method*/
				} else if (eat_p(":")) {
					pr.b = expr(2);
				} else {
					pr.c = 2; /*shorthand*/
				}
				props.push_back(a.add(pr));
			}
			if (!eat_p(",")) { break; }
		}
		expect_p("}");
		node nd{nk::object, ""}; nd.list = a.add_list(props); nd.list_len = static_cast<std::int32_t>(props.size());
		return a.add(nd);
	}

	// function expression/declaration; `expr` true => expression context;
	// `is_async` records `async` so the interpreter wraps the return in a promise
	constexpr std::int32_t func(bool is_expr, bool is_async = false) {
		// The span starts at `function`, or at the `async` before it - the
		// caller has already consumed that, so it passes the offset in.
		const std::uint32_t span_begin = offset_at(p);
		eat_kw("function");
		const bool is_gen = eat_p("*");
		std::string_view name;
		if (cur().kind == tk::ident || (cur().kind == tk::kw && is_contextual_keyword(cur().s))) {
			name = cur().s; advance();
		}
		std::int32_t len = 0; std::int32_t pl = params(len);
		std::int32_t body = block();
		node nd{is_expr ? nk::func_expr : nk::func_decl, name};
		nd.list = pl; nd.list_len = len; nd.a = body;
		// c: bit0 = async, bit1 = generator
		if (is_async || is_gen) { nd.c = (is_async ? 1 : 0) | (is_gen ? 2 : 0); }
		nd.begin = span_begin;
		nd.end = offset_consumed();
		return a.add(nd);
	}

	// --- statements ----------------------------------------------------------
	constexpr std::int32_t block() {
		expect_p("{");
		std::vector<std::int32_t> stmts;
		while (!is_p("}") && !at_end()) { stmts.push_back(stmt()); if (!a.ok) { break; } }
		expect_p("}");
		node nd{nk::block, ""}; nd.list = a.add_list(stmts); nd.list_len = static_cast<std::int32_t>(stmts.size());
		return a.add(nd);
	}

	constexpr std::int32_t var_decl() {
		std::string_view kw = cur().s; advance();      // let/const/var
		std::vector<std::int32_t> decls;
		for (;;) {
			// `b` is the pattern when the declarator binds a shape rather than a
			// name. This line used to take `{` AS THE NAME and desynchronise.
			node d{nk::declarator, ""};
			if (at_pattern()) { d.b = pattern(); } else { d.text = cur().s; advance(); }
			if (eat_p("=")) { d.a = expr(2); }
			decls.push_back(a.add(d));
			if (!eat_p(",")) { break; }
		}
		semi();
		node nd{nk::var_decl, kw}; nd.list = a.add_list(decls); nd.list_len = static_cast<std::int32_t>(decls.size());
		return a.add(nd);
	}

	constexpr std::int32_t class_decl(bool /*is_expr*/) {
		eat_kw("class");
		std::string_view name;
		if (cur().kind == tk::ident || (cur().kind == tk::kw && is_contextual_keyword(cur().s))) {
			name = cur().s; advance();
		}
		std::int32_t super = -1;
		if (eat_kw("extends")) { super = unary(); }
		expect_p("{");
		std::vector<std::int32_t> members;
		while (!is_p("}") && !at_end()) {
			if (eat_p(";")) { continue; }
			const std::uint32_t member_begin = offset_at(p);
			node m{nk::class_member, ""};
			m.d = 0;   // bit0 = static, bit1 = computed key, bit2 = accessor is a SETTER
			if (is_kw("static")) { advance(); m.d |= 1; }
			// get/set accessor: remember the kind, but the PROPERTY NAME follows
			bool is_getter = false, is_setter = false;
			if ((is_kw("get") || is_kw("set")) && !(nxt().kind == tk::punct && (nxt().s == "(" || nxt().s == "=" || nxt().s == ";"))) {
				is_getter = cur().s == "get";
				is_setter = cur().s == "set";
				advance();
			}
			const bool masync = is_kw("async");
			eat_kw("async");
			// `*method() {}` IS A GENERATOR, and the star used to be eaten and
			// thrown away - so a class generator method parsed cleanly and then
			// compiled as an ordinary function, whose `yield` had nowhere to go.
			// Babylon.js has 162 of them.
			const bool mgen = eat_p("*");
			// member name
			std::string_view mname;
			if (is_p("[")) { advance(); m.a = expr(0); expect_p("]"); m.d |= 2; /*computed*/ }
			else if (cur().kind == tk::str) { node k{nk::str, cur().s}; advance(); m.a = a.add(k); m.d |= 2; }
			else if (cur().kind == tk::num) { node k{nk::num, cur().s}; advance(); m.a = a.add(k); m.d |= 2; }
			else { mname = cur().s; advance(); }
			m.text = mname;                            // always the property name
			if (is_p("(")) {                          // method or accessor
				std::int32_t len = 0; std::int32_t pl = params(len);
				std::int32_t body = block();
				node fn{nk::func_expr, ""}; fn.list = pl; fn.list_len = len; fn.a = body;
				fn.begin = member_begin; fn.end = offset_consumed();
				// c: bit0 = async, bit1 = generator - the same encoding func() uses
				if (masync || mgen) { fn.c = (masync ? 1 : 0) | (mgen ? 2 : 0); }
				m.b = a.add(fn);
				if (is_getter || is_setter) { m.c = 2; if (is_setter) { m.d |= 4; } } // accessor
				else { m.c = 1; }                     // plain method
			} else {                                   // field
				if (eat_p("=")) { m.b = expr(2); }
				m.c = 0; semi();
			}
			members.push_back(a.add(m));
			if (!a.ok) { break; }
		}
		expect_p("}");
		node nd{nk::class_decl, name}; nd.a = super;
		nd.list = a.add_list(members); nd.list_len = static_cast<std::int32_t>(members.size());
		return a.add(nd);
	}

	// --- ES module declarations ------------------------------------------
	//
	// `import` in STATEMENT position. The expression forms - `import(...)` and
	// `import.meta` - are handled in primary(), and this must not swallow them,
	// which is why the caller checks the token after `import` first.
	constexpr std::int32_t import_decl_stmt() {
		eat_kw("import");
		node nd{nk::import_decl, ""};
		std::vector<std::int32_t> specs;
		// `import "./side-effect.js"` - no bindings at all.
		if (cur().kind == tk::str) {
			nd.text = cur().s;
			advance();
			semi();
			nd.list = a.add_list(specs);
			nd.list_len = 0;
			return a.add(nd);
		}
		// `import d from ...`
		if (cur().kind == tk::ident || (cur().kind == tk::kw && is_contextual_keyword(cur().s))) {
			node spec{nk::import_spec, cur().s};
			spec.c = 1; // default
			advance();
			specs.push_back(a.add(spec));
			eat_p(",");
		}
		// `import * as ns from ...`
		if (eat_p("*")) {
			if (!eat_word("as")) { fail("import * must be followed by as"); return -1; }
			node spec{nk::import_spec, cur().s};
			spec.c = 2; // namespace
			advance();
			specs.push_back(a.add(spec));
		} else if (eat_p("{")) {
			// `import { a, b as c } from ...`
			while (!is_p("}") && !at_end()) {
				const std::string_view imported = cur().s;
				advance();
				node spec{nk::import_spec, imported};
				spec.c = 0; // named
				if (eat_word("as")) {
					// RENAMED: text becomes the LOCAL name and the imported one
					// is kept beside it, because the local name is what the body
					// refers to and the imported name is what the module exports.
					spec.a = a.add({nk::str, imported});
					spec.text = cur().s;
					advance();
				}
				specs.push_back(a.add(spec));
				if (!eat_p(",")) { break; }
			}
			expect_p("}");
		}
		if (!eat_word("from")) { fail("import needs `from`"); return -1; }
		if (cur().kind != tk::str) { fail("import needs a module specifier"); return -1; }
		nd.text = cur().s;
		advance();
		semi();
		nd.list = a.add_list(specs);
		nd.list_len = static_cast<std::int32_t>(specs.size());
		return a.add(nd);
	}

	constexpr std::int32_t export_decl_stmt() {
		eat_kw("export");
		node nd{nk::export_decl, ""};
		std::vector<std::int32_t> specs;
		// `export default <expr>`
		if (is_kw("default")) {
			advance();
			nd.c = 1;
			nd.a = expr(2);
			semi();
			nd.list = a.add_list(specs);
			nd.list_len = 0;
			return a.add(nd);
		}
		// `export * from "./m.js"` and `export * as ns from "./m.js"`
		if (eat_p("*")) {
			if (eat_word("as")) {
				node spec{nk::export_spec, cur().s};
				advance();
				specs.push_back(a.add(spec));
			}
			nd.c = 2; // star
			if (!eat_word("from")) { fail("export * needs `from`"); return -1; }
			if (cur().kind != tk::str) { fail("export * needs a module specifier"); return -1; }
			nd.text = cur().s;
			advance();
			semi();
			nd.list = a.add_list(specs);
			nd.list_len = static_cast<std::int32_t>(specs.size());
			return a.add(nd);
		}
		// `export { a, b as c }` and the same with `from`
		if (eat_p("{")) {
			while (!is_p("}") && !at_end()) {
				const std::string_view local = cur().s;
				advance();
				node spec{nk::export_spec, local};
				if (eat_word("as")) {
					spec.a = a.add({nk::str, cur().s});
					advance();
				}
				specs.push_back(a.add(spec));
				if (!eat_p(",")) { break; }
			}
			expect_p("}");
			if (eat_word("from")) {
				if (cur().kind != tk::str) { fail("export from needs a module specifier"); return -1; }
				nd.text = cur().s;
				advance();
			}
			semi();
			nd.list = a.add_list(specs);
			nd.list_len = static_cast<std::int32_t>(specs.size());
			return a.add(nd);
		}
		// `export const x = 1`, `export function f() {}`, `export class C {}` -
		// the declaration is parsed as itself and simply hangs off the export.
		nd.a = stmt();
		nd.list = a.add_list(specs);
		nd.list_len = 0;
		return a.add(nd);
	}

	constexpr std::int32_t stmt() {
		const token & c = cur();
		if (c.kind == tk::punct && c.s == "{") { return block(); }
		if (c.kind == tk::punct && c.s == ";") { advance(); return a.add({nk::empty, ""}); }
		if (c.kind == tk::kw) {
			std::string_view k = c.s;
			if (k == "let" || k == "const" || k == "var") { return var_decl(); }
			if (k == "function") { return func(false); }
			if (k == "async" && nxt().kind == tk::kw && nxt().s == "function") { advance(); return func(false, true); }
			if (k == "class") { return class_decl(false); }
			// `import` is a declaration UNLESS it is `import(` or `import.`,
			// which are the expression forms and belong to primary().
			if (k == "import" && !(nxt().kind == tk::punct && (nxt().s == "(" || nxt().s == "."))) {
				return import_decl_stmt();
			}
			if (k == "export") { return export_decl_stmt(); }
			if (k == "if") { return if_stmt(); }
			if (k == "for") { return for_stmt(); }
			if (k == "while") { return while_stmt(); }
			if (k == "do") { return do_stmt(); }
			if (k == "return") { advance(); node nd{nk::return_stmt, ""}; if (!is_p(";") && !is_p("}") && !at_end()) { nd.a = expr(0); } semi(); return a.add(nd); }
			if (k == "break") { advance(); node nd{nk::break_stmt, ""}; if (cur().kind == tk::ident) { nd.text = cur().s; advance(); } semi(); return a.add(nd); }
			if (k == "continue") { advance(); node nd{nk::continue_stmt, ""}; if (cur().kind == tk::ident) { nd.text = cur().s; advance(); } semi(); return a.add(nd); }
			if (k == "throw") { advance(); node nd{nk::throw_stmt, ""}; nd.a = expr(0); semi(); return a.add(nd); }
			if (k == "try") { return try_stmt(); }
			if (k == "switch") { return switch_stmt(); }
		}
		// labeled statement:  ident ':'
		if (c.kind == tk::ident && nxt().kind == tk::punct && nxt().s == ":") {
			node nd{nk::labeled, c.s}; advance(); advance(); nd.a = stmt(); return a.add(nd);
		}
		// expression statement
		node nd{nk::expr_stmt, ""}; nd.a = expr(0); semi(); return a.add(nd);
	}

	constexpr std::int32_t if_stmt() {
		eat_kw("if"); expect_p("(");
		node nd{nk::if_stmt, ""}; nd.a = expr(0); expect_p(")");
		nd.b = stmt();
		if (eat_kw("else")) { nd.c = stmt(); }
		return a.add(nd);
	}
	constexpr std::int32_t while_stmt() {
		eat_kw("while"); expect_p("(");
		node nd{nk::while_stmt, ""}; nd.a = expr(0); expect_p(")"); nd.b = stmt();
		return a.add(nd);
	}
	constexpr std::int32_t do_stmt() {
		eat_kw("do");
		node nd{nk::do_stmt, ""}; nd.a = stmt();
		eat_kw("while"); expect_p("("); nd.b = expr(0); expect_p(")"); semi();
		return a.add(nd);
	}
	constexpr std::int32_t for_stmt() {
		eat_kw("for"); expect_p("(");
		// init: var decl or expr or empty
		std::int32_t init = -1; std::string_view forkw;
		if (is_kw("let") || is_kw("const") || is_kw("var")) {
			forkw = cur().s; advance();
			node d{nk::declarator, ""};
			if (at_pattern()) { d.b = pattern(); } else { d.text = cur().s; advance(); }
			// for-of / for-in?
			if (is_kw("of") || is_kw("in")) {
				std::string_view rel = cur().s; advance();
				node nd{nk::forof_stmt, rel}; nd.text = rel;
				// `for (const [k, v] of pairs)` - the item is a shape too
				node dd{nk::declarator, d.text}; dd.text = d.text; dd.b = d.b;
				nd.a = a.add(dd); nd.d = (forkw == "const"); nd.b = expr(0); expect_p(")"); nd.c = stmt();
				return a.add(nd);
			}
			if (eat_p("=")) { d.a = expr(2); }
			std::vector<std::int32_t> decls; decls.push_back(a.add(d));
			while (eat_p(",")) {
				node d2{nk::declarator, ""};
				if (at_pattern()) { d2.b = pattern(); } else { d2.text = cur().s; advance(); }
				if (eat_p("=")) { d2.a = expr(2); }
				decls.push_back(a.add(d2));
			}
			node vd{nk::var_decl, forkw}; vd.list = a.add_list(decls); vd.list_len = static_cast<std::int32_t>(decls.size());
			init = a.add(vd);
		} else if (!is_p(";")) {
			// `for (prop in obj)` with NO declaration keyword: the loop variable
			// is a binding that already exists. Without this the head parses as
			// a binary `in` expression and then wants the `;` of a classic for.
			// d bit1 says so, since there is nothing to declare.
			if ((cur().kind == tk::ident || cur().kind == tk::kw) && nxt().kind == tk::kw &&
			    (nxt().s == "in" || nxt().s == "of")) {
				node dd{nk::declarator, cur().s}; advance();
				std::string_view rel = cur().s; advance();
				node nd{nk::forof_stmt, rel};
				nd.a = a.add(dd); nd.d = 2;
				nd.b = expr(0); expect_p(")"); nd.c = stmt();
				return a.add(nd);
			}
			init = expr(0);
		}
		expect_p(";");
		node nd{nk::for_stmt, ""}; nd.a = init;
		if (!is_p(";")) { nd.b = expr(0); } expect_p(";");
		if (!is_p(")")) { nd.c = expr(0); } expect_p(")");
		nd.d = stmt();
		return a.add(nd);
	}
	constexpr std::int32_t try_stmt() {
		eat_kw("try");
		node nd{nk::try_stmt, ""}; nd.a = block();
		if (eat_kw("catch")) {
			node cc{nk::catch_clause, ""};
			if (eat_p("(")) { cc.text = cur().s; advance(); expect_p(")"); }
			cc.a = block(); nd.b = a.add(cc);
		}
		if (eat_kw("finally")) { nd.c = block(); }
		return a.add(nd);
	}
	constexpr std::int32_t switch_stmt() {
		eat_kw("switch"); expect_p("(");
		node nd{nk::switch_stmt, ""}; nd.a = expr(0); expect_p(")"); expect_p("{");
		std::vector<std::int32_t> clauses;
		while (!is_p("}") && !at_end()) {
			node cl{nk::case_clause, ""};
			if (eat_kw("case")) { cl.a = expr(0); expect_p(":"); }
			else { eat_kw("default"); expect_p(":"); cl.d = 1; }
			std::vector<std::int32_t> body;
			while (!is_kw("case") && !is_kw("default") && !is_p("}") && !at_end()) { body.push_back(stmt()); if (!a.ok) { break; } }
			cl.list = a.add_list(body); cl.list_len = static_cast<std::int32_t>(body.size());
			clauses.push_back(a.add(cl));
			if (!a.ok) { break; }
		}
		expect_p("}");
		nd.list = a.add_list(clauses); nd.list_len = static_cast<std::int32_t>(clauses.size());
		return a.add(nd);
	}

	constexpr std::int32_t program() {
		std::vector<std::int32_t> stmts;
		while (!at_end()) { stmts.push_back(stmt()); if (!a.ok) { break; } }
		node nd{nk::program, ""}; nd.list = a.add_list(stmts); nd.list_len = static_cast<std::int32_t>(stmts.size());
		return a.add(nd);
	}
};

// Parse a source string into a flat value AST (constexpr or runtime).
constexpr ast parse(std::string_view src) {
	ast a;
	lex_report report;
	std::vector<token> toks = lex(src, &report);
	parser ps{toks, a, 0, src};
	a.root = ps.program();
	a.skipped_bytes = report.skipped;
	a.first_skip_offset = report.first_skip;
	// Resolve the failing token to a source offset while the token vector is
	// still in scope - it is the last moment anyone can. The end token has an
	// empty lexeme pointing nowhere, so fall back to the end of the source.
	if (!a.ok && a.error_tok < toks.size()) {
		const std::string_view lexeme = toks[a.error_tok].s;
		a.error_offset = lexeme.data() == nullptr
		                     ? src.size()
		                     : static_cast<std::size_t>(lexeme.data() - src.data());
	}
	return a;
}

// does it parse cleanly? (mirrors ctjs::is_valid, by value)
constexpr bool is_valid(std::string_view src) { return parse(src).ok; }

} // namespace ctjs::vp

#endif
