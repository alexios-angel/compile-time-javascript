// Walking a document generically: every node carries its kind, children
// and attributes are iterable, so a recursive if-constexpr visitor can
// pretty-print (or transform) any document - the traversal is resolved
// at compile time, only the printing runs.
//
// Build: make introspection

#include <cthtml.hpp>
#include <iostream>
#include <string>

template <typename Node> void print(Node node, int indent = 0) {
	const std::string pad(static_cast<size_t>(indent) * 2, ' ');
	if constexpr (Node::type == cthtml::kind::text) {
		std::cout << pad << '"' << Node::view() << '"' << "\n";
	} else {
		std::cout << pad << '<' << Node::name();
		cthtml::for_each_attribute(node, [](auto name, auto value) {
			std::cout << ' ' << name.view() << "=\"" << value.view() << '"';
		});
		std::cout << ">\n";
		if constexpr (Node::child_count() != 0) {
			cthtml::for_each_child(node, [&](auto child) { print(child, indent + 1); });
			std::cout << pad << "</" << Node::name() << ">\n";
		}
	}
}

constexpr auto doc = cthtml::parse<R"(<!DOCTYPE html>
<title>feed</title>
<section id=s1 class=starred>
	<h2>Compile-time everything</h2>
	<p>types &amp; templates
</section>
<section id=s2>
	<h2>Parsers as tables</h2>
</section>
<hr>)">();

int main() {
	print(doc);

	// the same document, re-serialized to minified form at compile time
	constexpr auto minified = cthtml::serialize(doc);
	std::cout << "\nminified (" << minified.size() << " bytes):\n" << minified << "\n";
}
