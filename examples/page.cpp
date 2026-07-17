// The classic use: a web page baked into the binary at compile time.
// A typo in the markup, a crossing close tag or a missing attribute is
// a build failure, and every lookup below compiles down to a constant.
// The document is written the way HTML is actually written - void
// elements, unquoted attributes, <li> without </li> - and lands in a
// browser-shaped DOM: html > (head, body).
//
// Build: make page

#include <cthtml.hpp>
#include <iostream>

constexpr auto page = cthtml::parse<R"(<!DOCTYPE html>
<html lang=en>
<head>
	<meta charset=utf-8>
	<title>demo &mdash; releases</title>
</head>
<body data-build=42>
	<h1>Releases</h1>
	<ul id=nav>
		<li><a href=/docs>docs &amp; guides</a>
		<li><a href=/code>code</a>
		<li><a href=/chat>chat</a>
	</ul>
</body>
</html>)">();

// requirements checked at build time
static_assert(page.get<"head">().get<"title">().text() == "demo \xe2\x80\x94 releases");
static_assert(page.get<"body">().get<"ul">().count<"li">() == 3);
static_assert(page.get<"body">().get<"ul">().get<"li">().get<"a">().has_attribute<"href">());

// values usable as constants
constexpr int build = [] {
	constexpr auto text = page.get<"body">().attribute<"data-build">();
	int value = 0;
	for (const char c : text.view()) {
		value = value * 10 + (c - '0');
	}
	return value;
}();
int build_slots[build];

int main() {
	std::cout << "title: " << page.get<"head">().get<"title">().text() << "\n";
	std::cout << "build: " << build << " (slots: " << sizeof(build_slots) / sizeof(int) << ")\n";

	std::cout << "nav:\n";
	cthtml::for_each_child(page.get<"body">().get<"ul">(), [](auto li) {
		if constexpr (decltype(li)::type == cthtml::kind::element) {
			constexpr auto a = decltype(li)::template get<"a">();
			std::cout << "  " << a.template attribute<"href">().view()
			          << "  (" << a.text() << ")\n";
		}
	});
}
