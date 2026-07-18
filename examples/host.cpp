// The host seam - the shape a compile-time browser will use. The host
// (here: a pretend UI toolkit; later: DOM + SDL3) exposes native
// functions to the script as globals, the script wires up its logic at
// startup, and the host calls script functions back when events
// happen. The script was parsed at compile time; the event handlers
// run as optimizer-specialized native code.
//
// Build: make host

#include <ctjs.hpp>
#include <iostream>
#include <string>
#include <vector>

int main() {
	std::string title = "untitled";
	std::vector<std::string> widgets;

	auto ui = ctjs::run<R"(
		setTitle("counter demo");
		addWidget("label#count");
		addWidget("button#plus");

		let clicks = 0;
		function onClick(id) {
			if (id === "button#plus") {
				clicks += 1;
				setTitle("clicked " + clicks + (clicks === 1 ? " time" : " times"));
			}
			return clicks;
		}
	)">({
	    ctjs::binding{"setTitle", ctjs::native([&](const std::vector<ctjs::value> & a) {
		                  title = a[0].to_string();
	                  })},
	    ctjs::binding{"addWidget", ctjs::native([&](const std::vector<ctjs::value> & a) {
		                  widgets.push_back(a[0].to_string());
	                  })},
	});

	if (!ui.ok()) {
		std::cerr << "uncaught: " << ui.exception_message() << "\n";
		return 1;
	}

	std::cout << "title after setup: " << title << "\n";
	std::cout << "widgets: " << widgets.size() << "\n";

	// the host's event loop delivers events to the script
	for (int i = 0; i < 3; ++i) {
		ui.call("onClick", "button#plus");
	}
	std::cout << "title after clicks: " << title << "\n";
	std::cout << "clicks global: " << ui["clicks"].to<int>() << "\n";
}
