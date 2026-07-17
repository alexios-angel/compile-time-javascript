#ifndef CTJS__GRAMMAR__HPP
#define CTJS__GRAMMAR__HPP

#include "../ctlark.hpp"

// The grammar layer: HTML5 written FLAT in lark's grammar language and
// parsed by ctlark. Unlike XML, HTML tag nesting is not context-free -
// end tags may be omitted (<li>, <p>, <td>...) and <html>/<head>/<body>
// are implied - so the grammar does not nest elements at all: a
// document is a sequence of chunks (open tags, close tags, text, whole
// raw-text elements) and the tree-construction layer (treebuild.hpp)
// builds the DOM from that stream the way a browser's tree builder
// does.
//
// The grammar only tokenizes because ctlark's lexer is CONTEXTUAL,
// like lark's: TEXT is a candidate only where character data is
// expected, and the *_BODY terminals are the only candidates right
// after the ">" of a raw-text element's open tag, so <script>/<style>
// (raw text) and <title>/<textarea> (RCDATA) swallow their content -
// markup, "</div>", stray <, all of it - up to the first matching
// close tag, unrolled letter by letter because the regex subset has no
// lookarounds. The *_OPEN terminals take priority .2 so they beat OPEN
// on a tie ("<script"), while "<scripty" still lexes as OPEN because
// longest-match wins before priority breaks ties.
//
// Attributes may be double-quoted, single-quoted, unquoted (UNQVAL) or
// bare boolean (the [...] maybe in attr). DOCTYPE and comments (HTML
// rules: "--" is fine inside, the first --> ends it) are _-prefixed so
// they vanish from trees; CDATA sections are tolerated and dropped the
// same way. Entity references are NOT validated here - HTML never
// rejects them, so the binder decodes known ones and leaves the rest
// literal.

namespace ctjs::detail {

inline constexpr ctll::fixed_string html_grammar = R"x(
start: (open_tag | script_el | style_el | title_el | textarea_el
      | CLOSE | TEXT | _COMMENT | _CDATA | _DOCTYPE)*

open_tag: OPEN attr* ">"
        | OPEN attr* "/>" -> self_tag

script_el: SCRIPT_OPEN attr* ">" SCRIPT_BODY
style_el: STYLE_OPEN attr* ">" STYLE_BODY
title_el: TITLE_OPEN attr* ">" TITLE_BODY
textarea_el: TEXTAREA_OPEN attr* ">" TEXTAREA_BODY

attr: NAME ["=" (DQVAL | SQVAL | UNQVAL)]

SCRIPT_OPEN.2: /<script/i
STYLE_OPEN.2: /<style/i
TITLE_OPEN.2: /<title/i
TEXTAREA_OPEN.2: /<textarea/i

SCRIPT_BODY: /([^<]|<+[^\/<]|<+\/[^s<]|<+\/s[^c<]|<+\/sc[^r<]|<+\/scr[^i<]|<+\/scri[^p<]|<+\/scrip[^t<])*<+\/script[ \x09\x0a\x0d]*>/i
STYLE_BODY: /([^<]|<+[^\/<]|<+\/[^s<]|<+\/s[^t<]|<+\/st[^y<]|<+\/sty[^l<]|<+\/styl[^e<])*<+\/style[ \x09\x0a\x0d]*>/i
TITLE_BODY: /([^<]|<+[^\/<]|<+\/[^t<]|<+\/t[^i<]|<+\/ti[^t<]|<+\/tit[^l<]|<+\/titl[^e<])*<+\/title[ \x09\x0a\x0d]*>/i
TEXTAREA_BODY: /([^<]|<+[^\/<]|<+\/[^t<]|<+\/t[^e<]|<+\/te[^x<]|<+\/tex[^t<]|<+\/text[^a<]|<+\/texta[^r<]|<+\/textar[^e<]|<+\/textare[^a<])*<+\/textarea[ \x09\x0a\x0d]*>/i

OPEN: /<[A-Za-z][A-Za-z0-9:._\-]*/
CLOSE: /<\/[A-Za-z][A-Za-z0-9:._\-]*[ \x09\x0a\x0d]*>/
NAME: /[^ \x09\x0a\x0d\/>="'<]+/
DQVAL: /"[^"]*"/
SQVAL: /'[^']*'/
UNQVAL: /[^ \x09\x0a\x0d"'=<>`]+/
TEXT: /[^<]+/
_DOCTYPE: /<!doctype[^>]*>/i
_COMMENT: /<!--([^-]|-+[^->])*-+->/
_CDATA: /<!\[CDATA\[([^\]]|\]+[^\]>])*\]+\]>/

%ignore /[ \x09\x0a\x0d]+/
)x";

inline constexpr ctll::fixed_string html_start = "start";

static_assert(ctlark::grammar_valid<html_grammar>,
              "ctjs: internal error - the HTML grammar failed to compile");

} // namespace ctjs::detail

#endif
