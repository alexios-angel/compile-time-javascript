// The hero demo: this JavaScript is PARSED WHILE THIS FILE COMPILES -
// misspell a keyword or drop a semicolon and the build fails with a
// caret diagnostic - then EXECUTES when the binary runs, as code the
// C++ optimizer specialized for exactly this script.
//
// Build: make hello   (the first build bakes the grammar PCH - that
// one-time step is the slow part, every build after is quick)

#include <ctjs.hpp>
#include <iostream>
#include <string>

int main() {
	auto out = ctjs::run<R"(
		function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

		let squares = [1, 2, 3, 4, 5].map((x) => x * x);
		console.log("squares", squares.join(","));

		let user = { name: "ada", level: 7 };
		user.level += 1;
		console.log(user.name, "reached level", user.level);
	)">();

	if (!out.ok()) {
		std::cerr << "uncaught: " << out.exception_message() << "\n";
		return 1;
	}

	// everything the script printed
	std::cout << out.console();

	// read globals the script left behind
	std::cout << "level is " << out["user"]["level"].to<int>() << "\n";

	// call script functions from C++ (closures intact)
	std::cout << "fib(20) = " << out.call("fib", 20).to<int>() << "\n";
}
