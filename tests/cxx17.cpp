#include <cthtml.hpp>
#include <string_view>

// C++17: inputs and keys are fixed_string variables with linkage
static constexpr auto doc_text = ctll::fixed_string{R"(<cfg port="8080"><host>x</host></cfg>)"};
static constexpr auto bad_text = ctll::fixed_string{"<a></b>"};
static constexpr ctll::fixed_string port_key = "port";
static constexpr ctll::fixed_string host_tag = "host";

static_assert(cthtml::is_valid<doc_text>);
static_assert(!cthtml::is_valid<bad_text>);

constexpr auto doc = cthtml::parse<doc_text>();
static_assert(doc.template attribute<port_key>() == std::string_view{"8080"});
static_assert(doc.template get<host_tag>().text() == std::string_view{"x"});

void empty_symbol() { }

// operator[] needs no C++20: ordinary strings and integer indexes work as-is

static_assert(doc[0].name() == std::string_view{"host"});
static_assert(doc["host"].text() == std::string_view{"x"});

// iteration: uniform views, range-for, constexpr
static_assert([] {
	size_t n = 0;
	for (const auto & node : doc) {
		n += node.name().size() + node.text().size();
	}
	return n;
}() == 4 + 1);
static_assert(cthtml::attributes(doc).size() == 1);

// diagnostics through the variable-form API
static_assert(cthtml::error_info<doc_text>().ok());
static_assert(cthtml::error_info<bad_text>().ok()); // "<a></b>" PARSES; the binder rejects it
constexpr auto mismatch = cthtml::bind_error<bad_text>();
static_assert(mismatch.reason == cthtml::bind_reason::mismatched_tag);
static_assert(mismatch.where == std::string_view{"</b>"});
static constexpr auto unclosed_text = ctll::fixed_string{"<a><b></b>"};
constexpr auto unclosed_info = cthtml::error_info<unclosed_text>();
static_assert(unclosed_info.kind == ctlark::error_kind::parse);
static_assert(unclosed_info.column == 11);
static_assert(!cthtml::error_message<unclosed_text>().empty());
constexpr auto traced = cthtml::debug::traced_parse<unclosed_text>();
static_assert(!traced.ok && traced.log.events > 0);
static_assert(!cthtml::debug::dump_tokens<doc_text>().empty());
