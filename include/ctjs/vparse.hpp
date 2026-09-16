#ifndef CTJS__VPARSE__HPP
#define CTJS__VPARSE__HPP

#include <cstddef>
#include <memory>

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
	std::string_view s;    // the lexeme (a view into the source, or a decoded name)
	// WHERE IN THE SOURCE, as offsets: a decoded identifier's lexeme is not a
	// view into the source, and comparing its pointer against the source is
	// not a constant expression - so the position is carried, not derived.
	std::uint32_t begin = 0, end = 0;
};

// A BYTE OR A CODE POINT: the byte-at-a-time scan passes UTF-8 lead and
// continuation bytes (all > 127, all accepted, so any non-ASCII spelling is an
// identifier), and the escape decoder passes the code point it read.
// U+2E2F VERTICAL TILDE is Pattern_Syntax and in neither ID set; ZWNJ and
// ZWJ (U+200C, U+200D) are ID_Continue and not ID_Start (12.7). Only a
// decoded escape reaches here as a code point; the byte scan passes bytes.
constexpr bool is_id_start(char32_t c) {
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$' ||
	       (c > 127 && c != 0x200C && c != 0x200D && c != 0x2E2F);
}
constexpr bool is_id_part(char32_t c) {
	return is_id_start(c) || (c >= '0' && c <= '9') || c == 0x200C || c == 0x200D;
}
constexpr std::size_t unicode_space_at(std::string_view src, std::size_t i);
constexpr std::size_t line_terminator_at(std::string_view src, std::size_t i);
// The byte at src[i] continues an identifier: is_id_part on the byte, except
// that a non-ASCII whitespace or line terminator ends it - every byte over
// 127 is an identifier byte to the scan, and `x\u00A0+= 1` must be three
// tokens, not one name.
constexpr bool id_part_at(std::string_view src, std::size_t i) {
	const unsigned char c = static_cast<unsigned char>(src[i]);
	if (c <= 127) { return is_id_part(c); }
	return unicode_space_at(src, i) == 0 && line_terminator_at(src, i) == 0;
}
constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

// WHITESPACE AND LINE TERMINATORS BEYOND ASCII (12.2, 12.3). The byte scan
// takes every byte over 127 as an identifier character, so U+00A0 between
// two tokens glued them into one name and a U+2028 never ended a `//`
// comment. Both are answered here on the UTF-8 bytes at src[i]: how many
// bytes of whitespace start there (0 when none), and whether a line
// terminator does. The set is the specification's: TAB VT FF SP NBSP ZWNBSP
// and Zs for whitespace; LF CR LS PS for line terminators.
constexpr std::size_t unicode_space_at(std::string_view src, std::size_t i) {
	const std::size_t n = src.size();
	const unsigned char c = static_cast<unsigned char>(src[i]);
	if (c == 0xC2 && i + 1 < n && static_cast<unsigned char>(src[i + 1]) == 0xA0) { return 2; } // NBSP
	if (c == 0xE1 && i + 2 < n && static_cast<unsigned char>(src[i + 1]) == 0x9A &&
	    static_cast<unsigned char>(src[i + 2]) == 0x80) { return 3; } // U+1680 OGHAM SPACE MARK
	if (c == 0xE2 && i + 2 < n) {
		const unsigned char b = static_cast<unsigned char>(src[i + 1]);
		const unsigned char d = static_cast<unsigned char>(src[i + 2]);
		if (b == 0x80 && ((d >= 0x80 && d <= 0x8A) || d == 0xAF)) { return 3; } // U+2000..U+200A, U+202F
		if (b == 0x81 && d == 0x9F) { return 3; }                              // U+205F
	}
	if (c == 0xE3 && i + 2 < n && static_cast<unsigned char>(src[i + 1]) == 0x80 &&
	    static_cast<unsigned char>(src[i + 2]) == 0x80) { return 3; } // U+3000 IDEOGRAPHIC SPACE
	if (c == 0xEF && i + 2 < n && static_cast<unsigned char>(src[i + 1]) == 0xBB &&
	    static_cast<unsigned char>(src[i + 2]) == 0xBF) { return 3; } // U+FEFF ZWNBSP
	return 0;
}
constexpr std::size_t line_terminator_at(std::string_view src, std::size_t i) {
	const char c = src[i];
	if (c == '\n' || c == '\r') { return 1; }
	if (static_cast<unsigned char>(c) == 0xE2 && i + 2 < src.size() &&
	    static_cast<unsigned char>(src[i + 1]) == 0x80 &&
	    (static_cast<unsigned char>(src[i + 2]) == 0xA8 || static_cast<unsigned char>(src[i + 2]) == 0xA9)) {
		return 3; // U+2028 LINE SEPARATOR, U+2029 PARAGRAPH SEPARATOR
	}
	return 0;
}

// the reserved words ctjs recognises (matches grammar.hpp's IDENT exclusion)
inline constexpr std::string_view keywords[] = {
    "await", "break", "case", "catch", "class", "const", "continue", "default",
    "delete", "do", "else", "extends", "false", "finally", "for", "function",
    "if", "in", "instanceof", "let", "new", "null", "return", "super", "switch",
    "this", "throw", "true", "try", "typeof", "var", "void", "while", "with",
    "yield", "async", "of", "static", "get", "set", "import", "export", "debugger"};

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
		// The contextual keywords are names outside their one position, and a
		// name is followed by division: `instance/of/g` is not a regex.
		return t.s == "this" || t.s == "super" || t.s == "true" || t.s == "false" || t.s == "null" ||
		       is_contextual_keyword(t.s) || t.s == "let" || t.s == "async";
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

// WHERE A DECODED IDENTIFIER LIVES. A token's lexeme is a view into the
// source, and an identifier written with a unicode escape - `\u{6F}bj`,
// `#\u2118` - has no spelling in the source that names it: `obj` is the name,
// and `obj` must compare equal to `\u{6F}bj`. So the lexer decodes such an
// identifier into a string it appends here, and the token views THAT. The
// vector is the ast's, so the views live as long as the tree; unique_ptr so
// the vector may grow without moving the bytes a view points at.
using decoded_names = std::vector<std::unique_ptr<std::string>>;

// The UTF-8 of one code point, appended.
constexpr void append_utf8(std::string & out, char32_t cp) {
	if (cp < 0x80) { out.push_back(static_cast<char>(cp)); return; }
	if (cp < 0x800) {
		out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		return;
	}
	if (cp < 0x10000) {
		out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
		out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		return;
	}
	out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
	out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
	out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
	out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
}

// `\uXXXX` or `\u{X...}` at src[i] (the backslash). Answers the code point and
// moves i past it, or answers false and leaves i alone.
constexpr bool read_unicode_escape(std::string_view src, std::size_t & i, char32_t & cp) {
	const std::size_t n = src.size();
	if (i + 1 >= n || src[i] != '\\' || src[i + 1] != 'u') { return false; }
	auto hex = [](char c) -> int {
		if (c >= '0' && c <= '9') { return c - '0'; }
		if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
		if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
		return -1;
	};
	std::size_t j = i + 2;
	char32_t v = 0;
	if (j < n && src[j] == '{') {
		++j;
		std::size_t digits = 0;
		while (j < n && src[j] != '}') {
			const int h = hex(src[j]);
			if (h < 0 || ++digits > 8) { return false; }
			v = v * 16 + static_cast<char32_t>(h);
			++j;
		}
		if (j >= n || digits == 0 || v > 0x10FFFF) { return false; }
		++j;
	} else {
		for (int k = 0; k < 4; ++k, ++j) {
			const int h = j < n ? hex(src[j]) : -1;
			if (h < 0) { return false; }
			v = v * 16 + static_cast<char32_t>(h);
		}
	}
	i = j;
	cp = v;
	return true;
}

// Lex the whole source into a token vector (comments and whitespace dropped).
// `names` receives identifiers that needed decoding (see decoded_names);
// without it an escaped identifier keeps its raw spelling.
constexpr std::vector<token> lex(std::string_view src, lex_report * report = nullptr,
                                 decoded_names * names = nullptr) {
	std::vector<token> out;
	const std::size_t n = src.size();
	std::size_t i = 0;
	// A HASHBANG COMMENT (12.5): `#!` as the very first two bytes runs to the
	// end of its line. Only there - anywhere else `#` is what it always was.
	if (n >= 2 && src[0] == '#' && src[1] == '!') {
		while (i < n && line_terminator_at(src, i) == 0) { ++i; }
	}
	// WHICH `{` A `}` CLOSES decides what a `/` after it is: an object
	// literal's `}` is followed by division (`({a: 1} / 2)`), a block's by a
	// regex (`} /re/.test(x)`). A brace opens an object literal when what
	// precedes it wants an operand - an operator, `(`, `[`, `,`, `=`, `:`,
	// `?`, `return`, `typeof`... - and a block after `)`, `=>`, `;`, `{`,
	// `}`, `else`, `do`, `try`, `finally` or at the start.
	std::vector<bool> brace_is_object;
	auto opens_object = [&]() {
		if (out.empty()) { return false; }
		const token & prev = out.back();
		if (prev.kind == tk::punct) {
			return prev.s != ")" && prev.s != "}" && prev.s != ";" && prev.s != "{" && prev.s != "=>";
		}
		if (prev.kind == tk::kw) {
			return prev.s == "return" || prev.s == "typeof" || prev.s == "void" || prev.s == "delete" ||
			       prev.s == "in" || prev.s == "instanceof" || prev.s == "of" || prev.s == "await" ||
			       prev.s == "yield" || prev.s == "throw" || prev.s == "case" || prev.s == "extends" ||
			       prev.s == "new";
		}
		return false;   // a name, a literal: `x {` is not an expression anyway
	};
	bool last_brace_closed_object = false;
	auto has_div = [&]() {
		if (out.empty()) { return false; }
		if (out.back().kind == tk::punct && out.back().s == "}") { return last_brace_closed_object; }
		return div_follows(out.back());
	};
	// An identifier may START with an escape: `\u{6F}bj`.
	auto escape_starts_id = [&](std::size_t at) {
		std::size_t j = at;
		char32_t cp = 0;
		return read_unicode_escape(src, j, cp) && is_id_start(cp);
	};

	while (i < n) {
		char c = src[i];
		// whitespace
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') { ++i; continue; }
		if (static_cast<unsigned char>(c) > 127) {
			const std::size_t space = unicode_space_at(src, i);
			const std::size_t terminator = space != 0 ? 0 : line_terminator_at(src, i);
			if (space + terminator != 0) { i += space + terminator; continue; }
		}
		// comments - a `//` one ends at ANY line terminator, CR and the two
		// Unicode ones included (12.4 SingleLineComment)
		if (c == '/' && i + 1 < n && src[i + 1] == '/') { i += 2; while (i < n && line_terminator_at(src, i) == 0) { ++i; } continue; }
		if (c == '/' && i + 1 < n && src[i + 1] == '*') {
			const std::size_t opened = i;
			i += 2; while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) { ++i; }
			// UNTERMINATED: a `/*` that never closes is a SyntaxError (12.4),
			// not a comment to the end of the file - a token nothing accepts.
			if (i + 1 >= n) { out.push_back({tk::punct, src.substr(opened, 2), static_cast<std::uint32_t>(opened), static_cast<std::uint32_t>(opened + 2)}); i = n; continue; }
			i += 2; continue;
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
		    c == '#' && i + 1 < n &&
		    (is_id_start(static_cast<unsigned char>(src[i + 1])) || escape_starts_id(i + 1));
		if (private_name || is_id_start(static_cast<unsigned char>(c)) || escape_starts_id(i)) {
			if (private_name) { ++i; }
			// The decoded spelling, built only if an escape turns up.
			std::string decoded;
			bool escaped = false;
			while (i < n) {
				if (src[i] == '\\') {
					std::size_t j = i;
					char32_t cp = 0;
					if (!read_unicode_escape(src, j, cp) || !is_id_part(cp)) { break; }
					if (!escaped) {
						decoded.assign(src.substr(start, i - start));
						escaped = true;
					}
					append_utf8(decoded, cp);
					i = j;
					continue;
				}
				if (!id_part_at(src, i)) { break; }
				if (escaped) { decoded.push_back(src[i]); }
				++i;
			}
			std::string_view w = src.substr(start, i - start);
			if (escaped && names != nullptr) {
				names->push_back(std::make_unique<std::string>(std::move(decoded)));
				w = *names->back();
			}
			// AN ESCAPED WORD IS NEVER A KEYWORD (12.7.2): `\u0067et m() {}` is
			// not an accessor and `\u0061sync () => {}` is not an async arrow -
			// both are the identifier, and the parse fails on what follows.
			out.push_back({!private_name && !escaped && is_keyword(w) ? tk::kw : tk::ident, w, static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(i)});
			continue;
		}
		// number
		if (is_digit(c) || (c == '.' && i + 1 < n && is_digit(src[i + 1]))) {
			// 0x, 0o AND 0b, which all take the same shape: a two-character
			// prefix and then id-part characters, which covers the digits, the
			// a-f of hex and the `_` separators alike. Only 0x was recognised
			// before, so `0o17` lexed as the number `0` followed by the
			// identifier `o17` and the parse failed on ordinary modern code.
			const char p = i + 1 < n ? src[i + 1] : '\0';
			if (c == '0' && (p == 'x' || p == 'X' || p == 'o' || p == 'O' || p == 'b' || p == 'B')) {
				i += 2; while (i < n && id_part_at(src, i)) { ++i; }
			} else {
				// `_` IS A NUMERIC SEPARATOR (ES2021): 1_000_000. It belongs to
				// the token here and is stripped before the value is read, so
				// `1_000` no longer lexes as `1` and the identifier `_000`.
				// ONE DOT. `0..toString(2)` is the number `0.` and then a
				// member access, which a loop that ate every dot lexed as one
				// token `0..` and refused; the second dot ends the literal.
				bool dotted = false;
				while (i < n && (is_digit(src[i]) || (src[i] == '.' && !dotted) || src[i] == '_')) {
					dotted = dotted || src[i] == '.';
					++i;
				}
				if (i < n && (src[i] == 'e' || src[i] == 'E')) {
					++i; if (i < n && (src[i] == '+' || src[i] == '-')) { ++i; }
					while (i < n && (is_digit(src[i]) || src[i] == '_')) { ++i; }
				}
			}
			// THE BigInt SUFFIX. `1n` is one token, not the number 1 followed
			// by the identifier `n` - and getting that wrong is not a local
			// failure: the parse breaks at that point, so a bundle carrying a
			// single BigInt literal anywhere fails as a WHOLE and the page goes
			// blank. It rides on the number token because whether the digits
			// are a valid BigInt (they must be an integer - `1.5n` is not) is a
			// question for the consumer, not the lexer.
			if (i < n && src[i] == 'n') { ++i; }
			// 12.9.3: "The SourceCharacter immediately following a NumericLiteral
			// must not be an IdentifierStart or DecimalDigit" - `3in []` and
			// `0\u00620` are errors. The offending characters ride along in the
			// token, and the consumer that reads the literal refuses it.
			while (i < n && (id_part_at(src, i) || src[i] == '\\')) {
				if (src[i] == '\\') {
					std::size_t j = i; char32_t cp = 0;
					if (!read_unicode_escape(src, j, cp)) { break; }
					i = j; continue;
				}
				++i;
			}
			out.push_back({tk::num, src.substr(start, i - start), static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(i)});
			continue;
		}
		// string
		if (c == '"' || c == '\'') {
			char q = c; ++i;
			while (i < n && src[i] != q) { if (src[i] == '\\' && i + 1 < n) { i += 2; } else { ++i; } }
			if (i < n) { ++i; }
			out.push_back({tk::str, src.substr(start, i - start), static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(i)});
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
			out.push_back({tk::tmpl_full, src.substr(start, i - start), static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(i)});
			continue;
		}
		// regex vs division
		if (c == '/' && !has_div()) {
			++i; bool cls = false;
			while (i < n) {
				char d = src[i];
				if (d == '\\' && i + 1 < n) { i += 2; continue; }
				if (line_terminator_at(src, i) != 0) { break; }
				if (d == '[') { cls = true; ++i; continue; }
				if (d == ']') { cls = false; ++i; continue; }
				if (d == '/' && !cls) { ++i; break; }
				++i;
			}
			while (i < n && id_part_at(src, i)) { ++i; }
			out.push_back({tk::regex, src.substr(start, i - start), static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(i)});
			continue;
		}
		// punctuator: longest match among ctjs's operators
		std::string_view matched;
		for (std::string_view op : operators) {
			// FIRST BYTE FIRST. substr() + compare on all 56 was the cost.
			if (op[0] != src[i]) { continue; }
			if (i + op.size() <= n && src.substr(i, op.size()) == op) { matched = op; break; }
		}
		if (matched.empty()) {
			// AN UNKNOWN BYTE IS A TOKEN OF ITS OWN, which no rule of the parser
			// accepts - so `# x`, a stray `@` or a `\` outside an identifier is
			// the SyntaxError it should be. It used to be skipped, so `# x` in
			// a class body declared a public `x`. The count is kept for the
			// callers that read it.
			if (report != nullptr) {
				if (report->skipped == 0) { report->first_skip = i; }
				++report->skipped;
			}
			out.push_back({tk::punct, src.substr(i, 1), static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i + 1)});
			++i;
			continue;
		}
		// `?.` followed by a digit is `?` and a decimal: `a ?.5 : b` (13.3).
		if (matched == "?." && i + 2 < n && is_digit(src[i + 2])) { matched = "?"; }
		if (matched == "{") { brace_is_object.push_back(opens_object()); }
		if (matched == "}") {
			last_brace_closed_object = !brace_is_object.empty() && brace_is_object.back();
			if (!brace_is_object.empty()) { brace_is_object.pop_back(); }
		}
		out.push_back({tk::punct, src.substr(i, matched.size()), static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i + matched.size())});
		i += matched.size();
	}
	out.push_back({tk::end, {}, static_cast<std::uint32_t>(src.size()), static_cast<std::uint32_t>(src.size())});
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
	new_target,
	//   with_stmt   `with (a) b` - a = the object expression, b = the body.
	//               The keyword was lexed and never parsed, so the statement
	//               read as a CALL of a name `with` and threw at run time.
	with_stmt
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
	// Identifiers the lexer had to decode (unicode escapes); their tokens - and
	// so the nodes' `text` - view into these. See decoded_names.
	decoded_names decoded;

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
	// INSIDE A GENERATOR'S PARAMETERS OR BODY, `yield` is the YieldExpression;
	// anywhere else - a plain function, an arrow (its body is
	// FunctionBody[~Yield]), the top level - it is an identifier in sloppy
	// code, which `var yield = 23` in test262 and old bundles rely on.
	bool in_generator = false;
	// INSIDE AN ASYNC FUNCTION `await` is the AwaitExpression; inside a plain
	// function, generator or method it is an identifier in script code
	// (`function f(await) { return await; }` is legal there). TRUE AT THE TOP
	// LEVEL: this engine's embedding contract makes a script's top level an
	// async body - `return await x;` is how unittests/js reads a promise -
	// and a module's top level is [+Await] anyway. A plain arrow's body is
	// ConciseBody[~Await], an async arrow's is [+Await] - see async_arrow.
	bool in_async = true;
	// Set by the `async` lookahead just before an arrow is parsed, read and
	// cleared by arrow_body: a plain arrow's body is ConciseBody[~Await]
	// (`await` is a name there), an async arrow's is [+Await].
	bool async_arrow = false;
	struct generator_scope {
		bool & flag; bool saved;
		constexpr generator_scope(bool & f, bool on) : flag(f), saved(f) { flag = on; }
		constexpr ~generator_scope() { flag = saved; }
	};

	// IS THIS TOKEN'S LEXEME THE SOURCE'S OWN BYTES? A decoded identifier's
	// is not (decoded_names): `\u006deta` is not `meta`. Decided by content
	// against the token's span, never by pointer - a pointer into another
	// object is not comparable in a constant expression.
	constexpr bool in_source(const token & tok) const {
		return tok.s.size() == tok.end - tok.begin && tok.begin <= src.size() &&
		       tok.s == src.substr(tok.begin, tok.s.size());
	}
	// The offset a token starts at, and the offset just past the one before
	// the current position - which together bound everything consumed so far.
	constexpr std::uint32_t offset_at(std::size_t token_index) const {
		if (src.empty() || token_index >= t.size()) { return 0; }
		return t[token_index].begin;
	}
	constexpr std::uint32_t offset_consumed() const {
		if (src.empty() || p == 0) { return 0; }
		return t[p - 1].end;
	}

	// IS THERE A LINE TERMINATOR between the previous token and this one? The
	// restricted productions - `return`, `break`, `continue` - end at a line
	// break ([no LineTerminator here], 12.10.1), which no token records.
	constexpr bool newline_before_cur() const {
		if (src.empty() || p == 0) { return false; }
		const std::uint32_t from = offset_consumed();
		const std::uint32_t to = offset_at(p);
		for (std::uint32_t i = from; i < to && i < src.size(); ++i) {
			if (line_terminator_at(src, i) != 0) { return true; }
		}
		return false;
	}

	// MAY THE CURRENT TOKEN BE A BINDING NAME? An identifier, a contextual
	// keyword, `async`/`let` (keywords to this lexer, identifiers to the
	// grammar outside their one position), and `await`/`yield` where the
	// enclosing function does not reserve them - `function f(await) {}` in a
	// script, `class yield {}` outside a generator.
	constexpr bool at_name() const {
		if (cur().kind == tk::ident) { return true; }
		if (cur().kind != tk::kw) { return false; }
		const std::string_view w = cur().s;
		return is_contextual_keyword(w) || w == "async" || w == "let" ||
		       (w == "await" && !in_async) || (w == "yield" && !in_generator);
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
	// THE END OF A STATEMENT (12.10 automatic semicolon insertion): a `;`,
	// or nothing at all when the next token is `}`, the end of input, or on
	// a new line. `a b` on one line is the SyntaxError ASI does not repair;
	// a do-while's closing `)` takes a virtual `;` regardless (12.10.1).
	constexpr void semi() {
		if (eat_p(";") || is_p("}") || at_end() || newline_before_cur()) { return; }
		fail("expected `;` or a line break before this token");
	}
	// `with { type: "json" }` after a module specifier (16.2.2 WithClause).
	// The attributes are the loader's to read; nothing here consumes them
	// yet, so the clause is skipped whole. Before `with` was a statement the
	// clause read, by accident, as a name followed by a labelled block.
	constexpr void import_attributes() {
		if (!is_kw("with") && !is_word("assert")) { return; }
		advance();
		if (!eat_p("{")) { fail("import attributes need `{`"); return; }
		// WithEntries: `key : "value"` pairs, the key a name or a string, the
		// value a string; a key twice is the early error of 16.2.2.1.
		std::vector<std::string_view> keys;
		while (!is_p("}") && !at_end()) {
			if (cur().kind != tk::ident && cur().kind != tk::kw && cur().kind != tk::str) {
				fail("an import attribute key is a name or a string"); return;
			}
			std::string_view key = cur().s;
			if (cur().kind == tk::str && key.size() >= 2) { key = key.substr(1, key.size() - 2); }
			for (std::string_view seen : keys) {
				if (seen == key) { fail("an import attribute key may appear only once"); return; }
			}
			keys.push_back(key);
			advance();
			if (!eat_p(":")) { fail("an import attribute needs `:`"); return; }
			if (cur().kind != tk::str) { fail("an import attribute value is a string"); return; }
			advance();
			if (!eat_p(",")) { break; }
		}
		if (!eat_p("}")) { fail("import attributes need `}`"); }
	}

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
	constexpr std::int32_t expr(std::int32_t min_bp) { return expr_rest(unary(), min_bp); }
	// The Pratt loop from an operand already read - for_stmt reads the head's
	// first operand itself, to see whether `in`/`of` follows it.
	constexpr std::int32_t expr_rest(std::int32_t left, std::int32_t min_bp) {
		for (;;) {
			std::int32_t bp = lbp();
			if (bp < 0 || bp < min_bp) { break; }
			// A YieldExpression is an AssignmentExpression (15.5): it is no
			// operand of anything tighter than `,` unless parenthesised (c = 1).
			if (left >= 0 && a.nodes[static_cast<std::size_t>(left)].kind == nk::yield_expr &&
			    a.nodes[static_cast<std::size_t>(left)].c != 1 && bp > 1) {
				fail("`yield` is not an operand; parenthesise it"); return -1;
			}
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
			{
				const node & l = a.nodes[static_cast<std::size_t>(left)];
				// 13.6: the base of `**` is an UpdateExpression - `-a ** b` is
				// not in the grammar, `(-a) ** b` is (see paren_or_arrow).
				if (o == "**" && l.kind == nk::unary && l.d != 1) {
					fail("a unary operator cannot be the base of `**`; parenthesise it"); return -1;
				}
				// 13.13: `??` does not mix with `&&`/`||` without parentheses.
				if (l.kind == nk::logical && l.d != 1 &&
				    ((o == "??" && l.text != "??") || (o != "??" && logical && l.text == "??"))) {
					fail("`??` cannot be mixed with `&&` or `||` without parentheses"); return -1;
				}
			}
			advance();
			std::int32_t right = expr(o == "**" ? bp : bp + 1);
			if (right >= 0) {
				const node & r = a.nodes[static_cast<std::size_t>(right)];
				if (r.kind == nk::logical && r.d != 1 &&
				    ((o == "??" && r.text != "??") || (o != "??" && logical && r.text == "??"))) {
					fail("`??` cannot be mixed with `&&` or `||` without parentheses"); return -1;
				}
			}
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
		if (cur().kind == tk::kw && cur().s == "yield" && in_generator) {
			// yield [expr] - inside a generator (see in_generator; outside one
			// the keyword falls through to primary() as a name). `yield*` is DELEGATION
			// (27.5.3.7 / 14.4.14): d = 1 says so, and the operand is then
			// required and iterated by the consumer rather than yielded.
			advance();
			// `yield` [no LineTerminator here] `*`/operand (15.5): what follows a
			// line break is the next statement, and this yield has no operand.
			if (newline_before_cur()) { return a.add({nk::yield_expr, ""}); }
			const bool delegate = eat_p("*");
			node y{nk::yield_expr, ""};
			if (delegate) { y.d = 1; }
			if (!is_p(";") && !is_p(")") && !is_p("}") && !is_p(",") && !is_p("]") && !is_p(":") && !at_end()) { y.a = expr(2); }
			return a.add(y);
		}
		if (cur().kind == tk::kw) {
			std::string_view o = cur().s;
			if (o == "typeof" || o == "delete" || o == "void" || (o == "await" && in_async)) {
				advance(); node nd{nk::unary, o}; nd.a = unary(); return a.add(nd);
			}
		}
		return postfix();
	}

	constexpr std::int32_t postfix() {
		std::int32_t e = primary();
		bool optional_chain = false;   // a `?.` was seen in this chain
		for (;;) {
			if (is_p(".")) { advance(); node nd{nk::member, cur().s}; nd.a = e; advance(); e = a.add(nd); }
			else if (cur().kind == tk::tmpl_full) {
				// A TAGGED TEMPLATE, `tag\`...\``: a call with the template as its
				// argument. Not in an optional chain (13.3.1: `a?.b\`\`` is an
				// error), and nothing else ends a template's tag.
				if (optional_chain) { fail("a template literal cannot follow `?.` in a chain"); return -1; }
				node nd{nk::tagged, ""}; nd.a = e;
				node t{nk::tmpl, cur().s}; advance(); nd.b = a.add(t);
				e = a.add(nd);
			}
			else if (is_p("?.")) {
				optional_chain = true;
				advance();
				if (cur().kind == tk::tmpl_full) { fail("a template literal cannot follow `?.`"); return -1; }
				if (is_p("(")) { node nd{nk::opt_call, ""}; nd.a = e; nd.list = args(nd.list_len); e = a.add(nd); }
				else if (is_p("[")) { advance(); node nd{nk::opt_index, ""}; nd.a = e; nd.b = expr(0); expect_p("]"); e = a.add(nd); }
				else { node nd{nk::opt_member, cur().s}; nd.a = e; advance(); e = a.add(nd); }
			}
			else if (is_p("[")) { advance(); node nd{nk::index, ""}; nd.a = e; nd.b = expr(0); expect_p("]"); e = a.add(nd); }
			else if (is_p("(")) { node nd{nk::call, ""}; nd.a = e; nd.list = args(nd.list_len); e = a.add(nd); }
			// A postfix `++`/`--` is [no LineTerminator here] (13.4): across a
			// line break it is the next statement's prefix operator.
			else if ((is_p("++") || is_p("--")) && !newline_before_cur()) { node nd{nk::update, cur().s}; nd.a = e; nd.b = 0; /*postfix*/ advance(); e = a.add(nd); }
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
				// The options argument (import attributes) is `b`; a trailing
				// comma after either is legal, a third argument is not
				// (ImportCall takes at most two).
				if (eat_p(",") && !is_p(")") && !at_end()) {
					nd.b = expr(2);
					if (eat_p(",") && !is_p(")") && !at_end()) {
						fail("import() takes a specifier and at most an options argument");
						return -1;
					}
				}
				expect_p(")");
				return a.add(nd);
			}
			if (nxt().kind == tk::punct && nxt().s == ".") {
				advance();
				advance();
				// `import.source(x)` and `import.defer(x)` (ES2026 source phase
				// and deferred imports): an ImportCall with a phase, filed as
				// the dynamic_import node with c = 1 (source) or 2 (defer).
				// They take exactly one argument - no options, no spread.
				if ((cur().s == "source" || cur().s == "defer") && nxt().kind == tk::punct &&
				    nxt().s == "(") {
					const std::int32_t phase = cur().s == "source" ? 1 : 2;
					advance();
					expect_p("(");
					node nd{nk::dynamic_import, ""};
					nd.c = phase;
					if (is_p("...")) { fail("import.source/import.defer take one argument, not a spread"); return -1; }
					nd.a = expr(2);
					if (eat_p(",") && !is_p(")")) {
						fail("import.source/import.defer take exactly one argument");
						return -1;
					}
					expect_p(")");
					return a.add(nd);
				}
				if (cur().s != "meta" || !in_source(cur())) {
					fail("import. must be followed by meta");
					return -1;
				}
				advance();
				return a.add({nk::import_meta, "import.meta"});
			}
			// Bare `import` in expression position - `typeof import` - is
			// neither of the two and not an identifier either.
			fail("`import` is only an expression as import(...) or import.meta");
			return -1;
		}
		const token & c = cur();
		if (c.kind == tk::num) { node nd{nk::num, c.s}; advance(); return a.add(nd); }
		if (c.kind == tk::str) { node nd{nk::str, c.s}; advance(); return a.add(nd); }
		if (c.kind == tk::tmpl_full) { node nd{nk::tmpl, c.s}; advance(); return a.add(nd); }
		if (c.kind == tk::regex) { node nd{nk::regex, c.s}; advance(); return a.add(nd); }
		if (c.kind == tk::ident) {
			// arrow with single param:  x => ...
			if (nxt().kind == tk::punct && nxt().s == "=>") { return arrow_single(); }
			// A NAME KNOWS WHERE IT WAS READ: the compiler's static temporal
			// dead zone compares it against the declarator that initialises
			// the binding (below).
			node nd{nk::ident, c.s}; nd.begin = offset_at(p); advance(); nd.end = offset_consumed(); return a.add(nd);
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
					if (cur().kind != tk::ident || cur().s != "target" || !in_source(cur())) {
						fail("new. must be followed by target");
						return -1;
					}
					advance();
					return a.add({nk::new_target, "new.target"});
				}
				node nd{nk::new_expr, ""};
				nd.a = new_callee();                       // member-expr only (no trailing call)
				// `new import(x)` is not in the grammar: ImportCall is a CallExpression,
				// never a MemberExpression a `new` could apply to.
				for (std::int32_t base = nd.a; base >= 0;) {
					const node & callee = a.nodes[static_cast<std::size_t>(base)];
					if (callee.kind == nk::dynamic_import && callee.d != 1) {
						fail("`new` cannot be applied to import()");
						return -1;
					}
					if (callee.kind != nk::member && callee.kind != nk::index) { break; }
					base = callee.a;   // `new import(x).prop`: down the member chain
				}
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
				if (nxt().kind == tk::kw && nxt().s == "function") {
					const std::uint32_t from = offset_at(p);
					advance();
					return func(true, true, from);
				}
				// AN ASYNC ARROW. `async` fell through to being a bare
				// identifier, so `async (a, b) => {}` parsed as a CALL to
				// something named `async` and then met a `=>` it had nowhere to
				// put. Both spellings are checked by lookahead and the position
				// is restored if it turns out to be an ordinary use of the name.
				if (nxt().kind == tk::punct && nxt().s == "(") {
					const std::size_t save = p;
					const std::uint32_t from = offset_at(p);
					advance();
					// `async` [no LineTerminator here] (15.9): on its own line it
					// is a call of something named async, and the `=>` after
					// the arguments is then the error it should be.
					if (arrow_ahead() && !newline_before_cur()) {
						async_arrow = true;
						const std::int32_t r = paren_or_arrow();
						if (r >= 0) {
							a.nodes[static_cast<std::size_t>(r)].c = 1;   // async
							a.nodes[static_cast<std::size_t>(r)].begin = from;   // the text starts at `async`
						}
						return r;
					}
					p = save;
				}
				if (nxt().kind == tk::ident) {
					const std::size_t save = p;
					const std::uint32_t from = offset_at(p);
					advance();
					if (nxt().kind == tk::punct && nxt().s == "=>" && !newline_before_cur()) {
						async_arrow = true;
						const std::int32_t r = arrow_single();
						if (r >= 0) {
							a.nodes[static_cast<std::size_t>(r)].c = 1;
							a.nodes[static_cast<std::size_t>(r)].begin = from;
						}
						return r;
					}
					p = save;
				}
			}
			// A contextual keyword can be an arrow's single parameter too:
			// `swizzleSets.some(set => ...)` names one `set`, and `yield => 1`
			// outside a generator names `yield`.
			if (at_name() && nxt().kind == tk::punct && nxt().s == "=>") {
				return arrow_single();
			}
			// keyword used as a bare identifier (property contexts) - be lenient
			node nd{nk::ident, c.s}; nd.begin = offset_at(p); advance(); nd.end = offset_consumed(); return a.add(nd);
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
		if (newline_before_cur()) { fail("no line break is allowed before `=>`"); return -1; }
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

	// `for ([a, b] of xs)` / `for ({x} in o)`: a bracketed head followed by
	// `of`/`in` is an assignment PATTERN over existing bindings, not an array
	// or object literal starting a classic for. Same one-token-past-the-close
	// look arrow_ahead takes, over the matching bracket kind.
	constexpr bool for_pattern_ahead() const {
		const std::string_view open = t[p].s;
		const std::string_view close = open == "[" ? "]" : "}";
		std::size_t q = p; std::int32_t depth = 0;
		for (; q < t.size(); ++q) {
			if (t[q].kind == tk::punct && t[q].s == open) { ++depth; }
			else if (t[q].kind == tk::punct && t[q].s == close) { if (--depth == 0) { break; } }
			else if (t[q].kind == tk::end) { return false; }
		}
		std::size_t after = q + 1;
		return after < t.size() && t[after].kind == tk::kw && (t[after].s == "of" || t[after].s == "in");
	}

	constexpr std::int32_t paren_or_arrow() {
		if (arrow_ahead()) {
			const std::uint32_t span_begin = offset_at(p);
			node nd{nk::arrow, ""};
			std::int32_t len = 0; nd.list = params(len); nd.list_len = len;
			if (newline_before_cur()) { fail("no line break is allowed before `=>`"); return -1; }
			expect_p("=>");
			nd.a = arrow_body();
			nd.begin = span_begin;
			nd.end = offset_consumed();
			return a.add(nd);
		}
		advance();                       // '('
		std::int32_t e = expr(0);
		expect_p(")");
		// PARENTHESES ARE NOT KEPT, except as a mark on the two kinds whose
		// grammar turns on them: `(-a) ** b` and `(a ?? b) || c` are fine
		// where `-a ** b` and `a ?? b || c` are not. d is otherwise unused on
		// unary and logical nodes.
		if (e >= 0) {
			node & inner = a.nodes[static_cast<std::size_t>(e)];
			if (inner.kind == nk::unary || inner.kind == nk::logical || inner.kind == nk::dynamic_import) { inner.d = 1; }
			if (inner.kind == nk::yield_expr) { inner.c = 1; }
		}
		return e;
	}
	constexpr std::int32_t arrow_body() {
		// ConciseBody is FunctionBody[~Yield]: an arrow inside a generator
		// reads `yield` as a name again (a yield in an arrow is not one).
		const generator_scope inside{in_generator, false};
		const generator_scope awaiting{in_async, async_arrow};
		async_arrow = false;
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
				if (pasync) {
					advance();
					// `async` [no LineTerminator here] (15.6): on its own line it
					// would have to be a shorthand property, and then the next
					// thing is not the `,` a shorthand needs.
					if (newline_before_cur()) { fail("no line break is allowed after `async` here"); return -1; }
				}
				const bool pgen = eat_p("*");
				// key ("quoted" and 1-numeric keys ride the computed path -
				// evaluating the literal cooks quotes/escapes into the name).
				// `get`/`set` are the accessor words only as the unescaped
				// keyword the lexer marks; `\u0067et` is a name.
				bool accessor_word = false;
				if (is_p("[")) { advance(); pr.a = expr(0); expect_p("]"); pr.d = 1; /*computed*/ }
				else if (cur().kind == tk::str) { node k{nk::str, cur().s}; advance(); pr.a = a.add(k); pr.d = 1; }
				else if (cur().kind == tk::num) { node k{nk::num, cur().s}; advance(); pr.a = a.add(k); pr.d = 1; }
				else { pr.text = cur().s; accessor_word = cur().kind == tk::kw; advance(); }
				if (accessor_word && (pr.text == "get" || pr.text == "set") &&
				    !is_p("(") && !is_p(":") && !is_p(",") && !is_p("}")) {
					// accessor:  get name() {...} / set name(v) {...}  (the
					// name may itself be computed) - mirrors the class path
					const bool is_setter = pr.text == "set";
					if (is_p("[")) { advance(); pr.a = expr(0); expect_p("]"); pr.d = 1; pr.text = ""; }
					else { pr.text = cur().s; advance(); }
					const generator_scope plain{in_generator, false};
					const generator_scope sync{in_async, false};
					std::int32_t len = 0; std::int32_t pl = params(len);
					std::int32_t body = block();
					node fn{nk::func_expr, ""}; fn.list = pl; fn.list_len = len; fn.a = body;
					fn.begin = member_begin; fn.end = offset_consumed();
					pr.b = a.add(fn); pr.c = 3; /*accessor*/ if (is_setter) { pr.d |= 4; }
				} else if (is_p("(")) {                 // method shorthand
					const generator_scope inside{in_generator, pgen};
					const generator_scope awaiting{in_async, pasync};
					std::int32_t len = 0; std::int32_t pl = params(len);
					std::int32_t body = block();
					node fn{nk::func_expr, ""}; fn.list = pl; fn.list_len = len; fn.a = body;
					fn.begin = member_begin; fn.end = offset_consumed();
					if (pasync || pgen) { fn.c = (pasync ? 1 : 0) | (pgen ? 2 : 0); }
					pr.b = a.add(fn); pr.c = 1; /*method*/
				} else if (eat_p(":")) {
					pr.b = expr(2);
				} else {
					// A shorthand is an IdentifierReference (13.2.5): a word, not a
					// string, a number or a computed key - `({0})` and `({[x]})`
					// are errors - and `async`/`*` announce a method that then
					// has to follow.
					if (pr.d != 0 || pr.text.empty() || pasync || pgen) {
						fail("expected `:` or `(` after this property name");
						return -1;
					}
					pr.c = 2; /*shorthand*/
					// `{ a = 1 }`: a CoverInitializedName (13.2.5), legal only
					// where the literal is re-read as an assignment pattern -
					// `({ a = 1 } = o)`, a for-of head - and an early error
					// anywhere else. Filed as the shorthand's target with a
					// default, the shape `[a = 1] = xs` already has; the
					// checker refuses it in expression position.
					if (is_p("=")) {
						advance();
						node id{nk::ident, pr.text};
						node dflt{nk::assign, "="}; dflt.a = a.add(id); dflt.b = expr(2);
						pr.b = a.add(dflt);
					}
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
	static constexpr std::uint32_t no_offset = 0xFFFFFFFFu;
	constexpr std::int32_t func(bool is_expr, bool is_async = false,
	                            std::uint32_t span_from = no_offset) {
		// The span starts at `function`, or at the `async` before it - the
		// caller has already consumed that, so it passes the offset in
		// (Function.prototype.toString answers the whole source text, 20.2.3.5).
		const std::uint32_t span_begin = span_from == no_offset ? offset_at(p) : span_from;
		eat_kw("function");
		const bool is_gen = eat_p("*");
		std::string_view name;
		// The NAME of a declaration is bound outside the function, so it is
		// read against the enclosing context; an expression's name is bound
		// inside its own (`function* yield() {}` declares, `(function*
		// yield() {})` is an error - 15.5.1) - so that one is read within.
		std::int32_t len = 0; std::int32_t pl = -1; std::int32_t body = -1;
		if (!is_expr && at_name()) { name = cur().s; advance(); }
		{
			const generator_scope inside{in_generator, is_gen};
			const generator_scope awaiting{in_async, is_async};
			if (is_expr && at_name()) { name = cur().s; advance(); }
			pl = params(len);
			body = block();
		}
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

	// IS THERE A LINE TERMINATOR between tokens i and i + 1?
	constexpr bool newline_between(std::size_t i) const {
		if (src.empty() || i + 1 >= t.size()) { return false; }
		const std::size_t from = t[i].end;
		const std::size_t to = t[i + 1].begin;
		for (std::size_t k = from; k < to && k < src.size(); ++k) {
			if (line_terminator_at(src, k) != 0) { return true; }
		}
		return false;
	}
	// IS A `using` DECLARATION AT TOKEN i (ES2026 explicit resource
	// management)? `using` is an ordinary identifier everywhere else, so the
	// shape decides: `using` [no LineTerminator here] BindingIdentifier -
	// `using x`, not `using (x)`, `using.x`, `using = x` or `using of`.
	constexpr bool using_decl_at(std::size_t i) const {
		if (i + 1 >= t.size() || t[i].kind != tk::ident || t[i].s != "using") { return false; }
		const token & n = t[i + 1];
		const bool name = n.kind == tk::ident ||
		                  (n.kind == tk::kw && (n.s == "get" || n.s == "set" || n.s == "static" ||
		                                        n.s == "async" || n.s == "let" || n.s == "yield" || n.s == "await"));
		return name && !newline_between(i);
	}
	constexpr bool at_using_decl() const { return using_decl_at(p); }
	// `await using x`, only where `await` is the keyword and on one line.
	constexpr bool at_await_using_decl() const {
		return is_kw("await") && in_async && !newline_between(p) && using_decl_at(p + 1);
	}

	constexpr std::int32_t var_decl() {
		// `using` / `await using`: the kind is kept as the declaration's text,
		// as let/const/var are. The initialiser is required and the target
		// must be a name (14.3.2.1); the checker refuses the rest.
		std::string_view kw = cur().s;
		if (is_kw("await")) { advance(); kw = "await using"; }
		advance();      // let/const/var/using
		std::vector<std::int32_t> decls;
		for (;;) {
			// `b` is the pattern when the declarator binds a shape rather than a
			// name. This line used to take `{` AS THE NAME and desynchronise.
			node d{nk::declarator, ""};
			// The declarator's span: `end` is where its binding is initialised,
			// which is what a read before it is measured against (TDZ).
			d.begin = offset_at(p);
			if (at_pattern()) { d.b = pattern(); } else { d.text = cur().s; advance(); }
			if (eat_p("=")) { d.a = expr(2); }
			d.end = offset_consumed();
			decls.push_back(a.add(d));
			if (!eat_p(",")) { break; }
		}
		semi();
		node nd{nk::var_decl, kw}; nd.list = a.add_list(decls); nd.list_len = static_cast<std::int32_t>(decls.size());
		return a.add(nd);
	}

	constexpr std::int32_t class_decl(bool /*is_expr*/) {
		// The span is the whole class text, which is what `String(C)` gives.
		const std::uint32_t span_begin = offset_at(p);
		eat_kw("class");
		std::string_view name;
		if (at_name()) { name = cur().s; advance(); }
		std::int32_t super = -1;
		if (eat_kw("extends")) { super = unary(); }
		expect_p("{");
		std::vector<std::int32_t> members;
		while (!is_p("}") && !at_end()) {
			if (eat_p(";")) { continue; }
			std::uint32_t member_begin = offset_at(p);
			node m{nk::class_member, ""};
			m.d = 0;   // bit0 = static, bit1 = computed key, bit2 = accessor is a SETTER
			// `static` is the modifier only when a member follows it; `static = 1`,
			// `static;`, `static() {}` and `static }` name a member `static`.
			if (is_kw("static") && !(nxt().kind == tk::punct && (nxt().s == "(" || nxt().s == "=" || nxt().s == ";" || nxt().s == "}"))) {
				advance(); m.d |= 1;
				// A METHOD'S SOURCE TEXT IS THE MethodDefinition, which the
				// `static` is not part of (15.7.1 / 20.2.3.5): the span starts
				// after it.
				member_begin = offset_at(p);
				// A STATIC BLOCK, `static { ... }` (15.7.1 ClassStaticBlock): a
				// body run once with `this` = the class, in order with the
				// static fields. c = 3, the block in b. Its body is
				// [~Yield, +Await]-shaped: `await` is refused by the checker,
				// so it parses as the keyword rather than as a name.
				if (is_p("{")) {
					const generator_scope plain{in_generator, false};
					const generator_scope awaiting{in_async, true};
					m.b = block(); m.c = 3;
					members.push_back(a.add(m));
					if (!a.ok) { break; }
					continue;
				}
			}
			// get/set accessor: remember the kind, but the PROPERTY NAME follows
			bool is_getter = false, is_setter = false;
			if ((is_kw("get") || is_kw("set")) && !(nxt().kind == tk::punct && (nxt().s == "(" || nxt().s == "=" || nxt().s == ";"))) {
				is_getter = cur().s == "get";
				is_setter = cur().s == "set";
				advance();
			}
			// `async` [no LineTerminator here] (15.7): followed by a line break
			// it is a FIELD named async and the next line is the next member.
			bool masync = false;
			if (is_kw("async") && !(nxt().kind == tk::punct && (nxt().s == "(" || nxt().s == "=" || nxt().s == ";" || nxt().s == "}"))) {
				const std::size_t save = p;
				advance();
				if (newline_before_cur()) { p = save; } else { masync = true; }
			}
			// `*method() {}` IS A GENERATOR, and the star used to be eaten and
			// thrown away - so a class generator method parsed cleanly and then
			// compiled as an ordinary function, whose `yield` had nowhere to go.
			// Babylon.js has 162 of them.
			const bool mgen = eat_p("*");
			// `accessor` [no LineTerminator here] ClassElementName (the decorators
			// proposal's auto-accessor): read as a plain field of that name.
			// ponytail: a field, not a getter/setter pair over private storage;
			// upgrade when decorators land.
			if (is_word("accessor") && !masync && !mgen && !is_getter && !is_setter &&
			    !(nxt().kind == tk::punct && (nxt().s == "(" || nxt().s == "=" || nxt().s == ";" || nxt().s == "}"))) {
				const std::size_t save = p;
				advance();
				if (newline_before_cur()) { p = save; }
			}
			// member name
			std::string_view mname;
			if (is_p("[")) { advance(); m.a = expr(0); expect_p("]"); m.d |= 2; /*computed*/ }
			// A string-literal name is filed as a computed key holding the str
			// node, and its quoted text is kept in `text` so a checker can tell
			// `'prototype'` (a PropName, an early error when static) from
			// `['prototype']` (a computed key, a runtime TypeError).
			else if (cur().kind == tk::str) { node k{nk::str, cur().s}; mname = cur().s; advance(); m.a = a.add(k); m.d |= 2; }
			else if (cur().kind == tk::num) { node k{nk::num, cur().s}; advance(); m.a = a.add(k); m.d |= 2; }
			else { mname = cur().s; advance(); }
			m.text = mname;                            // always the property name
			if (is_p("(")) {                          // method or accessor
				const generator_scope inside{in_generator, mgen};
				const generator_scope awaiting{in_async, masync};
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
				m.c = 0;
				// A field ends at `;`, at `}`, or at a line break (ASI, 12.10):
				// `x y` on one line is not two fields.
				if (!eat_p(";") && !is_p("}") && !newline_before_cur()) {
					fail("a class field needs a `;` or a line break after it");
					return -1;
				}
			}
			members.push_back(a.add(m));
			if (!a.ok) { break; }
		}
		expect_p("}");
		node nd{nk::class_decl, name}; nd.a = super;
		nd.list = a.add_list(members); nd.list_len = static_cast<std::int32_t>(members.size());
		nd.begin = span_begin;
		nd.end = offset_consumed();
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
			import_attributes();
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
		import_attributes();
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
			// `export default class {}` / `function () {}` is a DECLARATION
			// (16.2.3), which no `;` ends; anything else is an expression.
			const bool declaration = is_kw("class") || is_kw("function") ||
			                         (is_kw("async") && nxt().kind == tk::kw && nxt().s == "function");
			nd.a = expr(2);
			if (!declaration) { semi(); }
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
			import_attributes();
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
				import_attributes();
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
			// `let` opens a declaration only before a name, `[` or `{` (14.3.1
			// with the ExpressionStatement lookahead); `let = 1` and `let.x`
			// are the identifier in sloppy code.
			if (k == "let" && !(nxt().kind == tk::ident || (nxt().kind == tk::kw && nxt().s != "in" && nxt().s != "instanceof") ||
			                    (nxt().kind == tk::punct && (nxt().s == "[" || nxt().s == "{")))) {
				node nd{nk::expr_stmt, ""}; nd.a = expr(0); semi(); return a.add(nd);
			}
			if (k == "let" || k == "const" || k == "var") { return var_decl(); }
			if (at_await_using_decl()) { return var_decl(); }
			if (k == "function") { return func(false); }
			if (k == "async" && nxt().kind == tk::kw && nxt().s == "function") {
				const std::uint32_t from = offset_at(p);
				advance();
				return func(false, true, from);
			}
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
			// `return`, `break` and `continue` end at a line break: what follows
			// on the next line is the next statement, not the operand or label
			// (p5.js has `if (x) return\n const y = ...`, which read `const` as
			// the returned name).
			if (k == "return") { advance(); node nd{nk::return_stmt, ""}; if (!is_p(";") && !is_p("}") && !at_end() && !newline_before_cur()) { nd.a = expr(0); } semi(); return a.add(nd); }
			if (k == "break") { advance(); node nd{nk::break_stmt, ""}; if (cur().kind == tk::ident && !newline_before_cur()) { nd.text = cur().s; advance(); } semi(); return a.add(nd); }
			if (k == "continue") { advance(); node nd{nk::continue_stmt, ""}; if (cur().kind == tk::ident && !newline_before_cur()) { nd.text = cur().s; advance(); } semi(); return a.add(nd); }
			if (k == "throw") { advance(); node nd{nk::throw_stmt, ""}; nd.a = expr(0); semi(); return a.add(nd); }
			// `debugger;` (14.16): a statement that does nothing here.
			if (k == "debugger") { advance(); semi(); return a.add({nk::empty, ""}); }
			if (k == "try") { return try_stmt(); }
			if (k == "switch") { return switch_stmt(); }
			if (k == "with") {
				advance(); expect_p("(");
				node nd{nk::with_stmt, ""}; nd.a = expr(0); expect_p(")"); nd.b = stmt();
				return a.add(nd);
			}
		}
		if (at_using_decl()) { return var_decl(); }
		// labeled statement:  ident ':' - a name, which `yield` and `await` are
		// where they are not the keyword
		if (at_name() && nxt().kind == tk::punct && nxt().s == ":") {
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
		eat_kw("while"); expect_p("("); nd.b = expr(0); expect_p(")"); eat_p(";");
		return a.add(nd);
	}
	constexpr std::int32_t for_stmt() {
		eat_kw("for");
		// `for await (x of y)`: d bit2 on the forof node. Only meaningful in an
		// async function; the compiler is where that is enforced.
		const bool is_await = eat_kw("await");
		expect_p("(");
		// init: var decl or expr or empty
		std::int32_t init = -1; std::string_view forkw;
		// `for (using x of xs)` / `for (await using x of xs)`: d bit4 says
		// `using`, bit5 `await using`; the binding is disposed per iteration.
		// `for (using x = a; ;)` is a using declaration as the init.
		const bool head_using = at_using_decl() || at_await_using_decl();
		if (head_using || is_kw("let") || is_kw("const") || is_kw("var")) {
			forkw = cur().s;
			if (head_using && is_kw("await")) { advance(); forkw = "await using"; }
			advance();
			node d{nk::declarator, ""};
			if (at_pattern()) { d.b = pattern(); } else { d.text = cur().s; advance(); }
			// for-of / for-in?
			if (is_kw("of") || is_kw("in")) {
				std::string_view rel = cur().s; advance();
				node nd{nk::forof_stmt, rel}; nd.text = rel;
				// `for (const [k, v] of pairs)` - the item is a shape too.
				// d: bit0 const, bit1 nothing to declare, bit2 await, bit3 let -
				// so a checker can tell `let` from `var` (14.7.5.1 has rules
				// for the lexical heads only).
				node dd{nk::declarator, d.text}; dd.text = d.text; dd.b = d.b;
				nd.a = a.add(dd); nd.d = (forkw == "const") | (is_await ? 4 : 0) | (forkw == "let" ? 8 : 0) |
				                  (forkw == "using" ? 16 : 0) | (forkw == "await using" ? 32 : 0);
				// `for (x of a, b)` is not in the grammar: `of` takes an
				// AssignmentExpression (14.7.5), `in` an Expression.
				nd.b = rel == "of" ? expr(2) : expr(0); expect_p(")"); nd.c = stmt();
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
			// `for (x in obj)`, `for (o.p of xs)`, `for ([a, b.c] of pairs)`
			// with NO declaration keyword: the head is a LeftHandSideExpression
			// (14.7.5) over bindings that already exist - a name, a member, or
			// an array/object LITERAL the compiler re-reads as a pattern, as it
			// does for `[a, b] = pair`. Read as an operand first; if `in`/`of`
			// follows it is the loop's target, else it is the start of a
			// classic for's init expression. d bit1 says there is nothing to
			// declare. A name is kept in `text` as the declaration forms do.
			const std::int32_t head = unary();
			if (is_kw("in") || is_kw("of")) {
				node dd{nk::declarator, ""};
				const node & h = a.nodes[static_cast<std::size_t>(head)];
				if (h.kind == nk::ident) { dd.text = h.text; } else { dd.b = head; }
				std::string_view rel = cur().s; advance();
				node nd{nk::forof_stmt, rel};
				nd.a = a.add(dd); nd.d = 2 | (is_await ? 4 : 0);
				nd.b = rel == "of" ? expr(2) : expr(0); expect_p(")"); nd.c = stmt();
				return a.add(nd);
			}
			init = expr_rest(head, 0);
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
			// `catch ({message})` and `catch ([first])` bind a pattern (b),
			// as a declarator does; a name is the text.
			if (eat_p("(")) {
				if (at_pattern()) { cc.b = pattern(); } else { cc.text = cur().s; advance(); }
				expect_p(")");
			}
			cc.a = block(); nd.b = a.add(cc);
		}
		if (eat_kw("finally")) { nd.c = block(); }
		if (nd.b < 0 && nd.c < 0) { fail("`try` needs a `catch` or a `finally`"); return -1; }
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
	std::vector<token> toks = lex(src, &report, &a.decoded);
	parser ps{toks, a, 0, src};
	a.root = ps.program();
	a.skipped_bytes = report.skipped;
	a.first_skip_offset = report.first_skip;
	// Resolve the failing token to a source offset while the token vector is
	// still in scope - it is the last moment anyone can. The end token has an
	// empty lexeme pointing nowhere, so fall back to the end of the source.
	if (!a.ok && a.error_tok < toks.size()) { a.error_offset = ps.offset_at(a.error_tok); }
	return a;
}

// does it parse cleanly? (mirrors ctjs::is_valid, by value)
constexpr bool is_valid(std::string_view src) { return parse(src).ok; }

} // namespace ctjs::vp

#endif
