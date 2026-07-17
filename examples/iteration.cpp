// Brackets and iteration: operator[] accepts ordinary tags and indexes
// (case-insensitively), returning uniform views; begin/end give an
// element's children those same views (kind + name + text) out of
// static storage, so range-for and <algorithm> work; attributes(...)
// does the same for its attributes.
//
// Build: make iteration

#include <cthtml.hpp>
#include <algorithm>
#include <iostream>

constexpr auto doc = cthtml::parse<R"(<main id=releases data-count=2>
	<article id=a1><h2>Brackets</h2></article>
	<article id=a2><h2>Iterators</h2></article>
	<footer>fin</footer>
</main>)">();

constexpr auto releases = doc.get<"body">().get<"main">();

// --- operator[]: get (first child with the tag) and child (by position)

static_assert(releases["article"].attribute("id") == "a1");
static_assert(releases["ARTICLE"]["h2"].text() == "Brackets");
static_assert(releases[1].attribute("id") == "a2");
static_assert(releases[2].text() == "fin");

// --- iteration: children and attributes as uniform views

static_assert(std::count_if(begin(releases), end(releases),
    [](const cthtml::node_view & n) { return n.name() == "article"; }) == 2);

static_assert(cthtml::attributes(releases).size() == 2);

// range-for in constant evaluation: a named constexpr function (gcc 10
// mishandles such loops inside a constexpr lambda)
constexpr size_t attribute_chars() noexcept {
	size_t total = 0;
	for (const auto & a : cthtml::attributes(releases)) {
		total += a.name.size() + a.value.size();
	}
	return total;
}
static_assert(attribute_chars() == (2 + 8) + (10 + 1));

int main() {
	// walk any element's children: views are plain kinds and string_views
	for (const auto & n : releases) {
		if (n.type == cthtml::kind::element) {
			std::cout << "<" << n.name() << "> " << n.text() << "\n";
		}
	}

	for (const auto & a : cthtml::attributes(releases)) {
		std::cout << "@" << a.name << " = " << a.value << "\n";
	}
}
