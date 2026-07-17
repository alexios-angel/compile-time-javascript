// Acceptability as a compile-time property: is_valid answers as a
// bool without failing the build, so shipping broken markup becomes
// impossible - the checks below run in the compiler, not in
// production. HTML5's conveniences are not errors; author mistakes
// browsers would silently repair are.
//
// Build: make wellformed

#include <cthtml.hpp>
#include <iostream>

// the conveniences: all valid
static_assert(cthtml::is_valid<"<p>no closing tag needed">);
static_assert(cthtml::is_valid<"<br>">);                       // void element
static_assert(cthtml::is_valid<"<ul><li>a<li>b</ul>">);        // li auto-closes
static_assert(cthtml::is_valid<"<table><tr><td>x<td>y</table>">);
static_assert(cthtml::is_valid<"<INPUT type=checkbox CHECKED>">); // case, unquoted, boolean
static_assert(cthtml::is_valid<"<!DOCTYPE html><title>t</title>">);
static_assert(cthtml::is_valid<R"(<script>if(a<b)say("</p>")</script>)">); // raw text
static_assert(cthtml::is_valid<"<p>&copy; &#169; &notaref;</p>">); // refs never fail

// the mistakes: all compile errors (or false from is_valid)
static_assert(!cthtml::is_valid<"<b><i>crossed</b></i>">);  // crossing close tag
static_assert(!cthtml::is_valid<"<p>x</p></p>">);           // stray close tag
static_assert(!cthtml::is_valid<"<a x='1' x='2'></a>">);    // duplicate attribute
static_assert(!cthtml::is_valid<"<div/>">);                 // self-closed non-void
static_assert(!cthtml::is_valid<"</br>">);                  // closing a void
static_assert(!cthtml::is_valid<"<div><b>x</div>">);        // </div> cannot close <b>
static_assert(!cthtml::is_valid<"a < b">);                  // write &lt;

// and the reason is queryable
static_assert(cthtml::bind_error<"<div/>">().reason ==
              cthtml::bind_reason::self_closing_non_void);

int main() {
	std::cout << "every claim in this file was proven during compilation\n";
}
