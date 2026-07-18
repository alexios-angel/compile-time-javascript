// The runtime suite: scripts are PARSED AT COMPILE TIME (each
// ctjs::script<...> below went through the grammar during this file's
// compilation) and executed here, with their behavior checked against
// what node does. Run the binary; a non-zero exit is a failure.
#include <ctjs.hpp>
#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++failures; \
		} \
	} while (0)

static void basics() {
	auto out = ctjs::run<R"(
		let total = 0;
		for (let i = 1; i <= 10; i++) { total += i; }
		console.log("total", total);
	)">();
	CHECK(out.ok());
	CHECK(out.console() == "total 55\n");
	CHECK(out["total"].to<int>() == 55);
}

static void closures_and_calls() {
	auto out = ctjs::run<R"(
		function counter() {
			let n = 0;
			return () => { n += 1; return n; };
		}
		let c = counter();
		c(); c();
		let third = c();
		function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
	)">();
	CHECK(out.ok());
	CHECK(out["third"].to<int>() == 3);
	CHECK(out.call("fib", 10).to<int>() == 55);
	CHECK(out.call("fib", 20).to<int>() == 6765);
}

static void arrays() {
	auto out = ctjs::run<R"(
		let xs = [3, 1, 4, 1, 5, 9];
		xs.push(2);
		let doubled = xs.map((x) => x * 2);
		let big = xs.filter((x) => x >= 4);
		let sum = xs.reduce((a, b) => a + b, 0);
		let text = xs.join("-");
		let sliced = xs.slice(1, 3);
		console.log(doubled);
		console.log(xs.length, sum, text);
	)">();
	CHECK(out.ok());
	CHECK(out.console() ==
	      "[ 6, 2, 8, 2, 10, 18, 4 ]\n"
	      "7 25 3-1-4-1-5-9-2\n");
	CHECK(out["big"].as_array()->size() == 3);
	CHECK(out["sliced"].to<std::string>() == "1,4");
}

static void objects_and_json() {
	auto out = ctjs::run<R"(
		let user = { name: "ada", "logins": 3, tags: ["admin", "dev"] };
		user.logins += 1;
		user["last"] = "today";
		let js = JSON.stringify(user);
		let keys = typeof user;
		console.log(user.name, user.logins, user.tags[1]);
	)">();
	CHECK(out.ok());
	CHECK(out.console() == "ada 4 dev\n");
	CHECK(out["js"].to<std::string>() ==
	      R"({"name":"ada","logins":4,"tags":["admin","dev"],"last":"today"})");
	CHECK(out["keys"].to<std::string>() == "object");
}

static void strings() {
	auto out = ctjs::run<R"(
		let s = "Hello,\tWorld\n";
		let up = s.trim().toUpperCase();
		let parts = "a,b,,c".split(",");
		let idx = "banana".indexOf("nan");
		let pad = String(7).padStart(3, "0");
		let bits = 'single ' + "double" + " " + 65;
	)">();
	CHECK(out.ok());
	CHECK(out["up"].to<std::string>() == "HELLO,\tWORLD");
	CHECK(out["parts"].as_array()->size() == 4);
	CHECK(out["idx"].to<int>() == 2);
	CHECK(out["pad"].to<std::string>() == "007");
	CHECK(out["bits"].to<std::string>() == "single double 65");
}

static void coercions() {
	auto out = ctjs::run<R"(
		let a = 1 == "1";
		let b = 1 === "1";
		let c = null == undefined;
		let d = null === undefined;
		let e = NaN === NaN;
		let f = typeof null;
		let g = "5" * "4";
		let h = "5" + 4;
		let i = !!"";
		let j = 0.1 + 0.2;
		let k = 7 % 3;
		let l = 2 ** 10;
		let m = 1 / 0;
	)">();
	CHECK(out.ok());
	CHECK(out["a"].to<bool>() && !out["b"].to<bool>());
	CHECK(out["c"].to<bool>() && !out["d"].to<bool>());
	CHECK(!out["e"].to<bool>());
	CHECK(out["f"].to<std::string>() == "object");
	CHECK(out["g"].to<int>() == 20);
	CHECK(out["h"].to<std::string>() == "54");
	CHECK(!out["i"].to<bool>());
	CHECK(out["j"].to<std::string>() == "0.30000000000000004");
	CHECK(out["k"].to<int>() == 1);
	CHECK(out["l"].to<int>() == 1024);
	CHECK(out["m"].to<std::string>() == "Infinity");
}

static void control_flow() {
	auto out = ctjs::run<R"(
		let log = [];
		for (const w of ["a", "b", "c"]) { log.push(w); }
		for (const ch of "xy") { log.push(ch); }
		let n = 0;
		do { n++; } while (n < 3);
		let seen = 0;
		for (let i = 0; i < 10; i++) {
			if (i % 2 === 0) { continue; }
			if (i > 6) { break; }
			seen += i;
		}
		let pick = null ?? "fallback";
		let t = seen > 8 ? "big" : "small";
		console.log(log.join(""), n, seen, pick, t);
	)">();
	CHECK(out.ok());
	CHECK(out.console() == "abcxy 3 9 fallback big\n");
}

static void exceptions() {
	auto out = ctjs::run<R"(
		let steps = [];
		function risky(x) {
			if (x < 0) { throw { name: "RangeError", message: "negative" }; }
			return x;
		}
		try {
			steps.push(risky(1));
			steps.push(risky(-1));
			steps.push(risky(2));
		} catch (e) {
			steps.push(e.message);
		} finally {
			steps.push("done");
		}
		let trail = steps.join(",");
	)">();
	CHECK(out.ok());
	CHECK(out["trail"].to<std::string>() == "1,negative,done");

	auto boom = ctjs::run<R"(
		let x = 1;
		undefinedFunction();
	)">();
	CHECK(!boom.ok());
	CHECK(boom.exception_message() == "ReferenceError: undefinedFunction is not defined");

	auto typeerr = ctjs::run<R"(
		let o = null;
		o.field;
	)">();
	CHECK(!typeerr.ok());
	CHECK(typeerr.exception_message() ==
	      "TypeError: Cannot read properties of null (reading 'field')");
}

static void host_bindings() {
	std::string clicked;
	auto out = ctjs::run<R"(
		register("go", (who) => "hi " + who);
		function onClick(target) { setStatus("clicked " + target); return target.length; }
	)">({
	    ctjs::binding{"register",
	                  ctjs::native([](ctjs::context & cx, const std::vector<ctjs::value> & a) {
		                  // host receives a JS closure and may call it back
		                  return ctjs::call_value(cx, a[1], {ctjs::value{"host"}});
	                  })},
	    ctjs::binding{"setStatus", ctjs::native([&](const std::vector<ctjs::value> & a) {
		                  clicked = a[0].to_string();
	                  })},
	});
	CHECK(out.ok());
	CHECK(out.result().to<std::string>() == "hi host"); // register(...) returned it
	CHECK(out.call("onClick", "button1").to<int>() == 7);
	CHECK(clicked == "clicked button1");
}

static void math_and_builtins() {
	auto out = ctjs::run<R"(
		let a = Math.floor(3.7) + Math.ceil(3.2) + Math.abs(-2);
		let b = Math.max(1, 9, 4) + Math.min(5, 2);
		let c = Math.pow(2, 8) + Math.sqrt(81);
		let d = parseInt("42px") + parseFloat("2.5rem");
		let e = parseInt("ff", 16);
		let f = Number("12") + Number(true);
		let g = isNaN(parseInt("nope"));
		let h = Array.isArray([1]) && !Array.isArray("no");
		let i = (255).toString() + "/" + (3.14159).toFixed(2);
		let j = Math.sin(0) + Math.cos(0) + Math.atan2(1, 1);
		let k = Math.hypot(3, 4) + Math.log(Math.exp(2)) + Math.log2(8);
		let l2 = Math.round(Math.tan(Math.PI / 4));
	)">();
	CHECK(out.ok());
	CHECK(out["a"].to<int>() == 9);
	CHECK(out["b"].to<int>() == 11);
	CHECK(out["c"].to<int>() == 265);
	CHECK(out["d"].to<double>() == 44.5);
	CHECK(out["e"].to<int>() == 255);
	CHECK(out["f"].to<int>() == 13);
	CHECK(out["g"].to<bool>());
	CHECK(out["h"].to<bool>());
	CHECK(out["i"].to<std::string>() == "255/3.14");
	CHECK(out["j"].to<std::string>() == "1.7853981633974483"); // 0 + 1 + pi/4
	CHECK(out["k"].to<double>() == 10.0);                      // 5 + 2 + 3
	CHECK(out["l2"].to<int>() == 1);
}

int main() {
	basics();
	closures_and_calls();
	arrays();
	objects_and_json();
	strings();
	coercions();
	control_flow();
	exceptions();
	host_bindings();
	math_and_builtins();
	if (failures == 0) {
		std::printf("runtime suite: all checks passed\n");
	}
	return failures;
}
