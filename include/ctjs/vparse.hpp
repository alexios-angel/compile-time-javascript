#ifndef CTJS__VPARSE__HPP
#define CTJS__VPARSE__HPP

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
    "yield", "async", "of", "static", "get", "set"};

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

// Lex the whole source into a token vector (comments and whitespace dropped).
constexpr std::vector<token> lex(std::string_view src) {
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
		if (is_id_start(static_cast<unsigned char>(c))) {
			while (i < n && is_id_part(static_cast<unsigned char>(src[i]))) { ++i; }
			std::string_view w = src.substr(start, i - start);
			out.push_back({is_keyword(w) ? tk::kw : tk::ident, w});
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
			++i; int depth = 0;
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
			if (i + op.size() <= n && src.substr(i, op.size()) == op) { matched = op; break; }
		}
		if (matched.empty()) { ++i; continue; }   // skip unknown byte
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
	func_decl, class_decl, class_member, param
};

struct node {
	nk kind;
	std::string_view text;                 // op / name / literal lexeme / flag
	int a = -1, b = -1, c = -1, d = -1;    // fixed child slots
	int list = -1, list_len = 0;           // variable-arity children (into ast::pool)
};

struct ast {
	std::vector<node> nodes;
	std::vector<int> pool;                 // child-index pool for list nodes
	int root = -1;
	bool ok = true;
	std::string_view error;
	std::size_t error_tok = 0;

	constexpr int add(node nd) { nodes.push_back(nd); return static_cast<int>(nodes.size()) - 1; }
	constexpr int add_list(const std::vector<int> & kids) {
		int at = static_cast<int>(pool.size());
		for (int k : kids) { pool.push_back(k); }
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

	constexpr const token & cur() const { return t[p]; }
	constexpr const token & nxt() const { return p + 1 < t.size() ? t[p + 1] : t.back(); }
	constexpr bool at_end() const { return cur().kind == tk::end; }
	constexpr bool is_p(std::string_view s) const { return cur().kind == tk::punct && cur().s == s; }
	constexpr bool is_kw(std::string_view s) const { return cur().kind == tk::kw && cur().s == s; }
	constexpr void advance() { if (p + 1 < t.size()) { ++p; } }
	constexpr bool eat_p(std::string_view s) { if (is_p(s)) { advance(); return true; } return false; }
	constexpr bool eat_kw(std::string_view s) { if (is_kw(s)) { advance(); return true; } return false; }

	constexpr int fail(std::string_view msg) {
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
	constexpr int lbp() const {
		if (cur().kind == tk::kw) {
			if (cur().s == "in" || cur().s == "instanceof") { return 13; }
			return -1;
		}
		if (cur().kind != tk::punct) { return -1; }
		std::string_view o = cur().s;
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
	constexpr int expr(int min_bp) {
		int left = unary();
		for (;;) {
			int bp = lbp();
			if (bp < 0 || bp < min_bp) { break; }
			std::string_view o = cur().s;
			if (cur().kind == tk::punct && o == "?") {           // ternary
				advance();
				int cons = expr(0);
				expect_p(":");
				int alt = expr(2);                                // right side down to assignment
				node nd{nk::ternary, "?:"}; nd.a = left; nd.b = cons; nd.c = alt;
				left = a.add(nd);
				continue;
			}
			if (cur().kind == tk::punct && is_assign_op(o)) {    // assignment (right-assoc)
				advance();
				int right = expr(bp);
				node nd{nk::assign, o}; nd.a = left; nd.b = right;
				left = a.add(nd);
				continue;
			}
			// binary / logical / relational (left-assoc; ** right-assoc)
			bool logical = (o == "&&" || o == "||" || o == "??");
			advance();
			int right = expr(o == "**" ? bp : bp + 1);
			node nd{logical ? nk::logical : nk::binary, o}; nd.a = left; nd.b = right;
			left = a.add(nd);
		}
		return left;
	}

	constexpr int unary() {
		if (cur().kind == tk::punct) {
			std::string_view o = cur().s;
			if (o == "!" || o == "-" || o == "+" || o == "~") {
				advance(); node nd{nk::unary, o}; nd.a = unary(); return a.add(nd);
			}
			if (o == "++" || o == "--") {
				advance(); node nd{nk::update, o}; nd.a = unary(); nd.b = 1; /*prefix*/ return a.add(nd);
			}
		}
		if (cur().kind == tk::kw) {
			std::string_view o = cur().s;
			if (o == "typeof" || o == "delete" || o == "void" || o == "await") {
				advance(); node nd{nk::unary, o}; nd.a = unary(); return a.add(nd);
			}
		}
		return postfix();
	}

	constexpr int postfix() {
		int e = primary();
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
	constexpr int args(int & len) {
		std::string_view open = cur().s, close = (open == "(") ? ")" : "]";
		advance();
		std::vector<int> kids;
		while (!is_p(close) && !at_end()) {
			if (is_p("...")) { advance(); node nd{nk::spread, ""}; nd.a = expr(2); kids.push_back(a.add(nd)); }
			else { kids.push_back(expr(2)); }
			if (!eat_p(",")) { break; }
		}
		expect_p(close);
		len = static_cast<int>(kids.size());
		return a.add_list(kids);
	}

	// the constructor of a `new`: a member expression with NO call (the call
	// after it, if any, supplies the new's arguments; further chains apply to
	// the constructed object)
	constexpr int new_callee() {
		int e = primary();
		for (;;) {
			if (is_p(".")) { advance(); node nd{nk::member, cur().s}; nd.a = e; advance(); e = a.add(nd); }
			else if (is_p("[")) { advance(); node nd{nk::index, ""}; nd.a = e; nd.b = expr(0); expect_p("]"); e = a.add(nd); }
			else { break; }
		}
		return e;
	}

	constexpr int primary() {
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
				node nd{nk::new_expr, ""};
				nd.a = new_callee();                       // member-expr only (no trailing call)
				if (is_p("(")) { nd.list = args(nd.list_len); nd.d = 1; }
				return a.add(nd);                          // postfix() continues for `.m()` on the result
			}
			if (c.s == "function") { return func(true); }
			if (c.s == "async") {
				if (nxt().kind == tk::kw && nxt().s == "function") { advance(); return func(true, true); }
				// async arrow: fall through to arrow handling below
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

	constexpr int arrow_single() {
		std::vector<int> ps;
		node pn{nk::param, cur().s}; ps.push_back(a.add(pn)); advance();   // ident
		expect_p("=>");
		node nd{nk::arrow, ""}; nd.list = a.add_list(ps); nd.list_len = 1;
		nd.a = arrow_body();
		return a.add(nd);
	}

	// disambiguate "(expr)" from "(params) =>" by scanning to the matching ')'
	constexpr bool arrow_ahead() const {
		std::size_t q = p; int depth = 0;
		for (; q < t.size(); ++q) {
			if (t[q].kind == tk::punct && t[q].s == "(") { ++depth; }
			else if (t[q].kind == tk::punct && t[q].s == ")") { if (--depth == 0) { break; } }
			else if (t[q].kind == tk::end) { return false; }
		}
		std::size_t after = q + 1;
		return after < t.size() && t[after].kind == tk::punct && t[after].s == "=>";
	}

	constexpr int paren_or_arrow() {
		if (arrow_ahead()) {
			node nd{nk::arrow, ""};
			int len = 0; nd.list = params(len); nd.list_len = len;
			expect_p("=>");
			nd.a = arrow_body();
			return a.add(nd);
		}
		advance();                       // '('
		int e = expr(0);
		expect_p(")");
		return e;
	}
	constexpr int arrow_body() {
		if (is_p("{")) { return block(); }
		return expr(2);
	}

	// parameter list at '(' -> pool; supports defaults and rest
	constexpr int params(int & len) {
		expect_p("(");
		std::vector<int> ps;
		while (!is_p(")") && !at_end()) {
			if (is_p("...")) { advance(); node nd{nk::param, cur().s}; nd.text = cur().s; nd.d = 1; /*rest*/ advance(); ps.push_back(a.add(nd)); }
			else {
				node nd{nk::param, cur().s}; advance();
				if (eat_p("=")) { nd.a = expr(2); }
				ps.push_back(a.add(nd));
			}
			if (!eat_p(",")) { break; }
		}
		expect_p(")");
		len = static_cast<int>(ps.size());
		return a.add_list(ps);
	}

	constexpr int object() {
		expect_p("{");
		std::vector<int> props;
		while (!is_p("}") && !at_end()) {
			if (is_p("...")) { advance(); node sp{nk::spread, ""}; sp.a = expr(2); props.push_back(a.add(sp)); }
			else {
				node pr{nk::prop, ""};
				// key
				if (is_p("[")) { advance(); pr.a = expr(0); expect_p("]"); pr.d = 1; /*computed*/ }
				else { pr.text = cur().s; advance(); }
				if (is_p("(")) {                        // method shorthand
					int len = 0; int pl = params(len);
					int body = block();
					node fn{nk::func_expr, ""}; fn.list = pl; fn.list_len = len; fn.a = body;
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
		node nd{nk::object, ""}; nd.list = a.add_list(props); nd.list_len = static_cast<int>(props.size());
		return a.add(nd);
	}

	// function expression/declaration; `expr` true => expression context;
	// `is_async` records `async` so the interpreter wraps the return in a promise
	constexpr int func(bool is_expr, bool is_async = false) {
		eat_kw("function");
		eat_p("*");
		std::string_view name;
		if (cur().kind == tk::ident) { name = cur().s; advance(); }
		int len = 0; int pl = params(len);
		int body = block();
		node nd{is_expr ? nk::func_expr : nk::func_decl, name};
		nd.list = pl; nd.list_len = len; nd.a = body;
		if (is_async) { nd.c = 1; }
		return a.add(nd);
	}

	// --- statements ----------------------------------------------------------
	constexpr int block() {
		expect_p("{");
		std::vector<int> stmts;
		while (!is_p("}") && !at_end()) { stmts.push_back(stmt()); if (!a.ok) { break; } }
		expect_p("}");
		node nd{nk::block, ""}; nd.list = a.add_list(stmts); nd.list_len = static_cast<int>(stmts.size());
		return a.add(nd);
	}

	constexpr int var_decl() {
		std::string_view kw = cur().s; advance();      // let/const/var
		std::vector<int> decls;
		for (;;) {
			node d{nk::declarator, cur().s}; advance();  // name (destructuring TODO)
			if (eat_p("=")) { d.a = expr(2); }
			decls.push_back(a.add(d));
			if (!eat_p(",")) { break; }
		}
		semi();
		node nd{nk::var_decl, kw}; nd.list = a.add_list(decls); nd.list_len = static_cast<int>(decls.size());
		return a.add(nd);
	}

	constexpr int class_decl(bool /*is_expr*/) {
		eat_kw("class");
		std::string_view name;
		if (cur().kind == tk::ident) { name = cur().s; advance(); }
		int super = -1;
		if (eat_kw("extends")) { super = unary(); }
		expect_p("{");
		std::vector<int> members;
		while (!is_p("}") && !at_end()) {
			if (eat_p(";")) { continue; }
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
			eat_kw("async"); eat_p("*");
			// member name
			std::string_view mname;
			if (is_p("[")) { advance(); m.a = expr(0); expect_p("]"); m.d |= 2; /*computed*/ }
			else { mname = cur().s; advance(); }
			m.text = mname;                            // always the property name
			if (is_p("(")) {                          // method or accessor
				int len = 0; int pl = params(len);
				int body = block();
				node fn{nk::func_expr, ""}; fn.list = pl; fn.list_len = len; fn.a = body;
				if (masync) { fn.c = 1; }             // async method -> promise-wrapped return
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
		nd.list = a.add_list(members); nd.list_len = static_cast<int>(members.size());
		return a.add(nd);
	}

	constexpr int stmt() {
		const token & c = cur();
		if (c.kind == tk::punct && c.s == "{") { return block(); }
		if (c.kind == tk::punct && c.s == ";") { advance(); return a.add({nk::empty, ""}); }
		if (c.kind == tk::kw) {
			std::string_view k = c.s;
			if (k == "let" || k == "const" || k == "var") { return var_decl(); }
			if (k == "function") { return func(false); }
			if (k == "async" && nxt().kind == tk::kw && nxt().s == "function") { advance(); return func(false, true); }
			if (k == "class") { return class_decl(false); }
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

	constexpr int if_stmt() {
		eat_kw("if"); expect_p("(");
		node nd{nk::if_stmt, ""}; nd.a = expr(0); expect_p(")");
		nd.b = stmt();
		if (eat_kw("else")) { nd.c = stmt(); }
		return a.add(nd);
	}
	constexpr int while_stmt() {
		eat_kw("while"); expect_p("(");
		node nd{nk::while_stmt, ""}; nd.a = expr(0); expect_p(")"); nd.b = stmt();
		return a.add(nd);
	}
	constexpr int do_stmt() {
		eat_kw("do");
		node nd{nk::do_stmt, ""}; nd.a = stmt();
		eat_kw("while"); expect_p("("); nd.b = expr(0); expect_p(")"); semi();
		return a.add(nd);
	}
	constexpr int for_stmt() {
		eat_kw("for"); expect_p("(");
		// init: var decl or expr or empty
		int init = -1; std::string_view forkw;
		if (is_kw("let") || is_kw("const") || is_kw("var")) {
			forkw = cur().s; advance();
			node d{nk::declarator, cur().s}; advance();
			// for-of / for-in?
			if (is_kw("of") || is_kw("in")) {
				std::string_view rel = cur().s; advance();
				node nd{nk::forof_stmt, rel}; nd.text = rel;
				node dd{nk::declarator, d.text}; dd.text = d.text;
				nd.a = a.add(dd); nd.d = (forkw == "const"); nd.b = expr(0); expect_p(")"); nd.c = stmt();
				return a.add(nd);
			}
			if (eat_p("=")) { d.a = expr(2); }
			std::vector<int> decls; decls.push_back(a.add(d));
			while (eat_p(",")) { node d2{nk::declarator, cur().s}; advance(); if (eat_p("=")) { d2.a = expr(2); } decls.push_back(a.add(d2)); }
			node vd{nk::var_decl, forkw}; vd.list = a.add_list(decls); vd.list_len = static_cast<int>(decls.size());
			init = a.add(vd);
		} else if (!is_p(";")) {
			init = expr(0);
		}
		expect_p(";");
		node nd{nk::for_stmt, ""}; nd.a = init;
		if (!is_p(";")) { nd.b = expr(0); } expect_p(";");
		if (!is_p(")")) { nd.c = expr(0); } expect_p(")");
		nd.d = stmt();
		return a.add(nd);
	}
	constexpr int try_stmt() {
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
	constexpr int switch_stmt() {
		eat_kw("switch"); expect_p("(");
		node nd{nk::switch_stmt, ""}; nd.a = expr(0); expect_p(")"); expect_p("{");
		std::vector<int> clauses;
		while (!is_p("}") && !at_end()) {
			node cl{nk::case_clause, ""};
			if (eat_kw("case")) { cl.a = expr(0); expect_p(":"); }
			else { eat_kw("default"); expect_p(":"); cl.d = 1; }
			std::vector<int> body;
			while (!is_kw("case") && !is_kw("default") && !is_p("}") && !at_end()) { body.push_back(stmt()); if (!a.ok) { break; } }
			cl.list = a.add_list(body); cl.list_len = static_cast<int>(body.size());
			clauses.push_back(a.add(cl));
			if (!a.ok) { break; }
		}
		expect_p("}");
		nd.list = a.add_list(clauses); nd.list_len = static_cast<int>(clauses.size());
		return a.add(nd);
	}

	constexpr int program() {
		std::vector<int> stmts;
		while (!at_end()) { stmts.push_back(stmt()); if (!a.ok) { break; } }
		node nd{nk::program, ""}; nd.list = a.add_list(stmts); nd.list_len = static_cast<int>(stmts.size());
		return a.add(nd);
	}
};

// Parse a source string into a flat value AST (constexpr or runtime).
constexpr ast parse(std::string_view src) {
	ast a;
	std::vector<token> toks = lex(src);
	parser ps{toks, a, 0};
	a.root = ps.program();
	return a;
}

// does it parse cleanly? (mirrors ctjs::is_valid, by value)
constexpr bool is_valid(std::string_view src) { return parse(src).ok; }

} // namespace ctjs::vp

#endif
