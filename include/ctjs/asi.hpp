#ifndef CTJS__ASI__HPP
#define CTJS__ASI__HPP

#include <cstddef>
#include <cstdint>

#include "../ctlark.hpp"   // ctll::fixed_string

// Automatic Semicolon Insertion (ASI).
//
// ctjs's grammar requires explicit semicolons - but real-world JavaScript
// omits them and relies on ASI (ECMA-262 sec. 12.9.1). Rather than teach the
// scannerless Earley grammar to treat newlines as terminators (which would
// ripple through every rule), ctjs NORMALISES the source first: a constexpr
// token-level pass that inserts the semicolons ASI would, BEFORE the parse.
// The grammar stays pristine; every parse entry point runs `asi_transform`.
//
// The pass is token-level, matching the spec's observable behaviour: a ';' is
// inserted in an inter-token gap that contains a line terminator when the
// previous token can END a statement and the next cannot CONTINUE it, or when
// the previous token is a restricted-production keyword (return/throw/break/
// continue/yield); and, regardless of newline, before a block-closing '}' and
// at end of input. A small brace/paren stack distinguishes a block '{' from an
// object-literal '{' and a control-header ')' (`if (...)`) from an expression
// ')', which is what makes the "can end / can continue" classification right.
//
// Known intentional imperfections (all vanishingly rare, and never in code
// that already has its semicolons): a `{` right after `:` is read as an object
// (nested-object case), and nested do-while `)` termination is heuristic.

namespace ctjs::detail::asi {

enum frame { PAREN_CTRL, PAREN_NORM, BRACKET, BRACE_BLOCK, BRACE_OBJ, BRACE_FNBODY };

struct tok {
	bool cont = false;        // as NEXT token, continues prev -> suppress ';'
	bool ender = false;       // as PREV token, can end a statement
	bool div_ok = false;      // after this token, '/' is division (else regex)
	bool restricted = false;  // return/throw/break/continue/yield
	bool block_close = false; // a '}' that may need a ';' inserted before it
	bool operand = false;     // an operand is expected AFTER this -> next '{' is object
	bool ctrl_kw = false;     // if/for/while/switch/catch/with (next '(' is a header)
	bool arrow = false;       // '=>' (the next '{' is an arrow-function BODY block)
	bool member_dot = false;  // '.' or '?.' (the next identifier is a PROPERTY name)
};

constexpr bool id_start(char32_t c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$' || c > 127;
}
constexpr bool id_part(char32_t c) { return id_start(c) || (c >= '0' && c <= '9'); }
constexpr bool digit(char32_t c) { return c >= '0' && c <= '9'; }

// does s[a,b) equal the ASCII keyword k?
constexpr bool kw_eq(const char32_t * s, std::size_t a, std::size_t b, const char * k) {
	std::size_t len = 0;
	while (k[len] != '\0') { ++len; }
	if (b - a != len) { return false; }
	for (std::size_t i = 0; i < len; ++i) {
		if (s[a + i] != static_cast<char32_t>(k[i])) { return false; }
	}
	return true;
}

// The core: scan `s[0,n)` and call emit(char32_t) for every output code point,
// inserting ';' where ASI requires. Allocation-free (fixed stack) so it folds
// at compile time; also usable at runtime for testing.
template <class Emit>
constexpr void run(const char32_t * s, std::size_t n, Emit emit) {
	frame stack[512];
	std::int32_t sp = 0;
	std::int32_t fn_expr_depth = -1;   // stack depth at a `function` EXPRESSION keyword;
	                          // the body '{' opened back at this depth is an
	                          // expression block whose '}' ends the statement
	tok prev;
	bool have_prev = false;
	std::size_t i = 0;

	while (true) {
		// ---- gap: whitespace + comments; remember if a line terminator appears
		std::size_t gap_start = i;
		bool nl = false;
		while (i < n) {
			char32_t c = s[i];
			if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f') { ++i; continue; }
			if (c == '\n') { nl = true; ++i; continue; }
			if (c == '/' && i + 1 < n && s[i + 1] == '/') { i += 2; while (i < n && s[i] != '\n') { ++i; } continue; }
			if (c == '/' && i + 1 < n && s[i + 1] == '*') {
				i += 2;
				while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) { if (s[i] == '\n') { nl = true; } ++i; }
				if (i + 1 < n) { i += 2; } else { i = n; }
				continue;
			}
			break;
		}
		std::size_t gap_end = i;

		if (i >= n) { // EOF: terminate a dangling statement
			if (have_prev && (prev.ender || prev.restricted)) { emit(U';'); }
			for (std::size_t k = gap_start; k < gap_end; ++k) { emit(s[k]); }
			break;
		}

		// ---- scan one significant token: [tok_start, i)
		std::size_t tok_start = i;
		tok t;
		char32_t c = s[i];

		if (id_start(c)) {
			while (i < n && id_part(s[i])) { ++i; }
			std::size_t a = tok_start, b = i;
			if (have_prev && prev.member_dot) {
				t.ender = true; t.div_ok = true;                 // property name (e.g. `p.catch`, `.then`)
			} else if (kw_eq(s, a, b, "return") || kw_eq(s, a, b, "throw") || kw_eq(s, a, b, "yield")) {
				t.restricted = true; t.operand = true;
			} else if (kw_eq(s, a, b, "break") || kw_eq(s, a, b, "continue")) {
				t.restricted = true;
			} else if (kw_eq(s, a, b, "this") || kw_eq(s, a, b, "super") || kw_eq(s, a, b, "true") ||
			           kw_eq(s, a, b, "false") || kw_eq(s, a, b, "null")) {
				t.ender = true; t.div_ok = true;
			} else if (kw_eq(s, a, b, "in") || kw_eq(s, a, b, "instanceof")) {
				t.cont = true; t.operand = true;
			} else if (kw_eq(s, a, b, "else") || kw_eq(s, a, b, "catch") || kw_eq(s, a, b, "finally") ||
			           kw_eq(s, a, b, "while")) {
				t.cont = true;
				if (kw_eq(s, a, b, "while") || kw_eq(s, a, b, "catch")) { t.ctrl_kw = true; }
			} else if (kw_eq(s, a, b, "typeof") || kw_eq(s, a, b, "delete") || kw_eq(s, a, b, "void") ||
			           kw_eq(s, a, b, "new") || kw_eq(s, a, b, "await") || kw_eq(s, a, b, "case")) {
				t.operand = true;
			} else if (kw_eq(s, a, b, "if") || kw_eq(s, a, b, "for") || kw_eq(s, a, b, "switch") ||
			           kw_eq(s, a, b, "with")) {
				t.ctrl_kw = true;
			} else if (kw_eq(s, a, b, "function")) {
				// a function EXPRESSION (operand position) has an expression body:
				// remember the depth so its body '{' is tagged BRACE_FNBODY
				if (have_prev && prev.operand) { fn_expr_depth = sp; }
			} else if (kw_eq(s, a, b, "async")) {
				t.operand = have_prev && prev.operand; // propagate expr position to `function`
			} else if (kw_eq(s, a, b, "class") || kw_eq(s, a, b, "let") || kw_eq(s, a, b, "const") ||
			           kw_eq(s, a, b, "var") || kw_eq(s, a, b, "try") || kw_eq(s, a, b, "do") ||
			           kw_eq(s, a, b, "default") || kw_eq(s, a, b, "extends") || kw_eq(s, a, b, "of") ||
			           kw_eq(s, a, b, "static") || kw_eq(s, a, b, "get") || kw_eq(s, a, b, "set")) {
				// construct-introducing keyword; a following '{' is a block
			} else {
				t.ender = true; t.div_ok = true; // plain identifier
			}
		} else if (digit(c) || (c == '.' && i + 1 < n && digit(s[i + 1]))) {
			if (c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
				i += 2; while (i < n && id_part(s[i])) { ++i; }
			} else {
				while (i < n && (digit(s[i]) || s[i] == '.')) { ++i; }
				if (i < n && (s[i] == 'e' || s[i] == 'E')) {
					++i; if (i < n && (s[i] == '+' || s[i] == '-')) { ++i; }
					while (i < n && digit(s[i])) { ++i; }
				}
			}
			t.ender = true; t.div_ok = true;
		} else if (c == '"' || c == '\'') {
			char32_t q = c; ++i;
			while (i < n && s[i] != q) { if (s[i] == '\\' && i + 1 < n) { i += 2; } else { ++i; } }
			if (i < n) { ++i; }
			t.ender = true; t.div_ok = true;
		} else if (c == '`') {
			++i; std::int32_t depth = 0;
			while (i < n) {
				char32_t d = s[i];
				if (d == '\\' && i + 1 < n) { i += 2; continue; }
				if (depth == 0 && d == '`') { ++i; break; }
				if (depth == 0 && d == '$' && i + 1 < n && s[i + 1] == '{') { depth = 1; i += 2; continue; }
				if (depth > 0 && d == '{') { ++depth; ++i; continue; }
				if (depth > 0 && d == '}') { --depth; ++i; continue; }
				if (depth > 0 && (d == '"' || d == '\'')) {
					char32_t q = d; ++i;
					while (i < n && s[i] != q) { if (s[i] == '\\' && i + 1 < n) { i += 2; } else { ++i; } }
					if (i < n) { ++i; }
					continue;
				}
				++i;
			}
			t.ender = true; t.div_ok = true;
		} else if (c == '/' && !(have_prev && prev.div_ok)) {
			++i; bool inclass = false;
			while (i < n) {
				char32_t d = s[i];
				if (d == '\\' && i + 1 < n) { i += 2; continue; }
				if (d == '\n') { break; }
				if (d == '[') { inclass = true; ++i; continue; }
				if (d == ']') { inclass = false; ++i; continue; }
				if (d == '/' && !inclass) { ++i; break; }
				++i;
			}
			while (i < n && id_part(s[i])) { ++i; } // flags
			t.ender = true; t.div_ok = true;
		} else {
			// punctuator: longest-match the operators we care about
			if (c == '?' && i + 1 < n && s[i + 1] == '.' && !(i + 2 < n && digit(s[i + 2]))) { i += 2; t.cont = true; t.member_dot = true; }
			else if (c == '=' && i + 1 < n && s[i + 1] == '>') { i += 2; t.cont = true; t.arrow = true; } // => : next '{' is an arrow body
			else if (c == '.' && i + 2 < n && s[i + 1] == '.' && s[i + 2] == '.') { i += 3; t.cont = true; t.operand = true; }
			else if (c == '*' && i + 1 < n && s[i + 1] == '*') { i += (i + 2 < n && s[i + 2] == '=') ? 3 : 2; t.cont = true; t.operand = true; }
			else if ((c == '=' || c == '!') && i + 1 < n && s[i + 1] == '=') { i += (i + 2 < n && s[i + 2] == '=') ? 3 : 2; t.cont = true; t.operand = true; }
			else if ((c == '<' || c == '>') && i + 1 < n && s[i + 1] == '=') { i += 2; t.cont = true; t.operand = true; }
			else if (c == '<' && i + 1 < n && s[i + 1] == '<') { i += (i + 2 < n && s[i + 2] == '=') ? 3 : 2; t.cont = true; t.operand = true; }
			else if (c == '>' && i + 1 < n && s[i + 1] == '>') { std::size_t k = i + 2; if (k < n && s[k] == '>') { ++k; } if (k < n && s[k] == '=') { ++k; } i = k; t.cont = true; t.operand = true; }
			else if (c == '&' && i + 1 < n && s[i + 1] == '&') { i += (i + 2 < n && s[i + 2] == '=') ? 3 : 2; t.cont = true; t.operand = true; }
			else if (c == '|' && i + 1 < n && s[i + 1] == '|') { i += (i + 2 < n && s[i + 2] == '=') ? 3 : 2; t.cont = true; t.operand = true; }
			else if (c == '?' && i + 1 < n && s[i + 1] == '?') { i += (i + 2 < n && s[i + 2] == '=') ? 3 : 2; t.cont = true; t.operand = true; }
			else if (c == '+' && i + 1 < n && s[i + 1] == '+') { i += 2; t.ender = true; t.div_ok = true; } // ++
			else if (c == '-' && i + 1 < n && s[i + 1] == '-') { i += 2; t.ender = true; t.div_ok = true; } // --
			else {
				++i;
				switch (static_cast<char>(c)) {
				case '(': {
					bool ctrl = have_prev && prev.ctrl_kw;
					if (sp < 512) { stack[sp++] = ctrl ? PAREN_CTRL : PAREN_NORM; }
					t.cont = true; t.operand = true; break;
				}
				case '[': if (sp < 512) { stack[sp++] = BRACKET; } t.cont = true; t.operand = true; break;
				case '{': {
					frame fr;
					if (have_prev && prev.arrow) { fr = BRACE_FNBODY; }       // () => { ... }
					else if (fn_expr_depth >= 0 && sp == fn_expr_depth) { fr = BRACE_FNBODY; fn_expr_depth = -1; } // function(){...}
					else if (have_prev && prev.operand) { fr = BRACE_OBJ; }   // = { ... }, ( { ... }
					else { fr = BRACE_BLOCK; }                                // statement block
					if (sp < 512) { stack[sp++] = fr; }
					break;
				}
				case ')': {
					frame f = sp > 0 ? stack[--sp] : PAREN_NORM;
					t.cont = true; t.div_ok = true;
					t.ender = (f != PAREN_CTRL); // control-header ')' does not end a statement
					break;
				}
				case ']': if (sp > 0) { --sp; } t.cont = true; t.ender = true; t.div_ok = true; break;
				case '}': {
					frame f = sp > 0 ? stack[--sp] : BRACE_BLOCK;
					// object close: an expression ender, suppress ';' before it.
					if (f == BRACE_OBJ) { t.cont = true; t.ender = true; t.div_ok = true; }
					// function/arrow body: a ';' may be needed INSIDE before '}',
					// and the whole expression ENDS here (enclosing stmt needs ';').
					else if (f == BRACE_FNBODY) { t.block_close = true; t.ender = true; t.div_ok = true; }
					// statement block: ';' may be needed inside; block itself ends no statement.
					else { t.block_close = true; }
					break;
				}
				case ';': t.cont = true; break;
				case ',': case ':': case '?': t.cont = true; t.operand = true; break;
				case '.': t.cont = true; t.member_dot = true; break;
				case '=': case '<': case '>': case '+': case '-': case '*': case '/':
				case '%': case '&': case '|': case '^': t.cont = true; t.operand = true; break;
				case '!': case '~': t.operand = true; break;
				default: break;
				}
			}
		}
		std::size_t tok_end = i;

		// ---- ASI decision for the gap between prev and this token
		bool insert = false;
		if (have_prev) {
			if (prev.restricted && (nl || t.block_close)) { insert = true; }
			else if (prev.ender && ((nl && !t.cont) || t.block_close)) { insert = true; }
		}

		if (insert) { emit(U';'); }
		for (std::size_t k = gap_start; k < gap_end; ++k) { emit(s[k]); }
		for (std::size_t k = tok_start; k < tok_end; ++k) { emit(s[k]); }

		prev = t; have_prev = true;
	}
}

} // namespace ctjs::detail::asi

namespace ctjs::detail {

#if CTLL_CNTTP_COMPILER_CHECK
// Output length of the ASI-normalised source.
template <ctll::fixed_string Src>
constexpr std::size_t asi_len() {
	std::size_t c = 0;
	asi::run(Src.content, Src.size(), [&](char32_t) { ++c; });
	return c;
}

// The ASI-normalised source, as a fixed_string usable as a parser NTTP.
template <ctll::fixed_string Src>
constexpr auto asi_transform() {
	constexpr std::size_t N = asi_len<Src>();
	ctll::fixed_string<N> out{};
	std::size_t o = 0;
	asi::run(Src.content, Src.size(), [&](char32_t ch) { if (o < N) { out.content[o++] = ch; } });
	out.real_size = o;
	return out;
}

// Every parse entry point feeds `asi_src<Src>` to the grammar instead of the
// raw `Src`, so the normalisation happens once per distinct source.
template <ctll::fixed_string Src>
inline constexpr auto asi_src = asi_transform<Src>();
#endif

} // namespace ctjs::detail

#endif
