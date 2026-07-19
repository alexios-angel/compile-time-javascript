// The runtime suite: scripts are PARSED AT COMPILE TIME (each
// ctjs::script<...> below went through the grammar during this file's
// compilation) and executed here, with their behavior checked against
// what node does. Run the binary; a non-zero exit is a failure.
#include <ctjs.hpp>
#include <cstdio>
#include <string>

// --- the interpreter is constexpr: whole programs evaluate AT COMPILE
// TIME. These static_asserts run the interpreter during this file's
// compilation - variables, loops, recursion, strings, objects, arrays
// and closures, all evaluated with no runtime at all.
static_assert(ctjs::eval<"2 + 3 * 4;">().to<int>() == 14);
static_assert(ctjs::eval<"let a = 2, b = 3; a * b + 1;">().to<int>() == 7);
static_assert(ctjs::eval<"let s = 0; for (let i = 1; i <= 10; i++) { s += i; } s;">().to<int>() == 55);
static_assert(ctjs::eval<"function fact(n){ return n <= 1 ? 1 : n * fact(n - 1); } fact(5);">().to<int>() == 120);
static_assert(ctjs::eval<"'a' + 'b' + 'c';">().to<std::string>() == "abc");
static_assert(ctjs::eval<"let o = { x: 41 }; o.x + 1;">().to<int>() == 42);
static_assert(ctjs::eval<"let xs = [1, 2, 3, 4]; let t = 0; for (const v of xs) { t += v; } t;">().to<int>() == 10);
static_assert(ctjs::eval<"let add = (a, b) => a + b; add(20, 22);">().to<int>() == 42);

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

static void async_and_promises() {
	auto out = ctjs::run<R"(
		async function double_it(x) { return x * 2; }
		let direct = double_it(21);          // a promise object
		let awaited = await double_it(21);   // top-level await unwraps
		let chained = -1;
		double_it(10).then((v) => { chained = v; });
		let flat = await Promise.resolve(Promise.resolve(7));
		let all = await Promise.all([double_it(1), Promise.resolve(4), 5]);
		let caught = "";
		let recovered = await Promise.reject("boom").catch((e) => "caught " + e);
		try { await Promise.reject("bang"); } catch (e) { caught = e; }
		let ran_finally = false;
		let kept = await Promise.resolve("kept").finally(() => { ran_finally = true; });
		async function inner() {
			const v = await Promise.resolve(30);
			return v + 3;
		}
		let nested = await inner();
	)">();
	CHECK(out.ok());
	CHECK(out["direct"].is_object());
	CHECK(out["awaited"].to<int>() == 42);
	CHECK(out["chained"].to<int>() == 20);
	CHECK(out["flat"].to<int>() == 7);
	CHECK(out["all"].is_array());
	CHECK(out["all"][0].to<int>() == 2);
	CHECK(out["all"][1].to<int>() == 4);
	CHECK(out["all"][2].to<int>() == 5);
	CHECK(out["recovered"].to<std::string>() == "caught boom");
	CHECK(out["caught"].to<std::string>() == "bang");
	CHECK(out["ran_finally"].to<bool>());
	CHECK(out["kept"].to<std::string>() == "kept");
	CHECK(out["nested"].to<int>() == 33);

	// a host binding can hand a settled promise straight to the script
	auto host = ctjs::run<R"(
		const response = await fetchish("hello");
		const upper = response.toUpperCase();
	)">({{"fetchish", ctjs::native([](const std::vector<ctjs::value> & a) {
		     return ctjs::make_promise(ctjs::value{a[0].to_string() + " world"}, false);
	     },
	     "fetchish")}});
	CHECK(host.ok());
	CHECK(host["upper"].to<std::string>() == "HELLO WORLD");
}

static void json_parse() {
	auto out = ctjs::run<R"(
		let doc = JSON.parse('{"name":"pong","scores":[3,1,4],"live":true,"pi":3.5,"nothing":null,"nested":{"deep":"yes!"}}');
		let round = JSON.stringify(doc);
		let arr = JSON.parse("[1, -2.5, 1e3]");
		let bad = "";
		try { JSON.parse("{nope}"); } catch (e) { bad = e.name; }
	)">();
	CHECK(out.ok());
	CHECK(out["doc"]["name"].to<std::string>() == "pong");
	CHECK(out["doc"]["scores"][2].to<int>() == 4);
	CHECK(out["doc"]["live"].to<bool>());
	CHECK(out["doc"]["pi"].to<double>() == 3.5);
	CHECK(out["doc"]["nothing"].is_null());
	CHECK(out["doc"]["nested"]["deep"].to<std::string>() == "yes!");
	CHECK(out["round"].to<std::string>() ==
	      R"({"name":"pong","scores":[3,1,4],"live":true,"pi":3.5,"nothing":null,"nested":{"deep":"yes!"}})");
	CHECK(out["arr"][2].to<double>() == 1000.0);
	CHECK(out["bad"].to<std::string>() == "SyntaxError");
}

static void optional_chaining_and_object_sugar() {
	auto out = ctjs::run<R"(
		let user = { name: "ada", pet: { name: "rex", speak() { return "woof, " + this.name; } } };
		let ghost = null;

		let pet_name = user?.pet?.name;
		let no_pet = ghost?.pet;
		let no_deep = ghost?.pet?.name;
		let idx = user?.pet?.["name"];
		let bark = user.pet?.speak?.();
		let no_call = ghost?.speak?.();
		let missing_method = user?.dance?.();

		let x = 1, y = 2;
		let shorthand = { x, y };
		let methods = {
			base: 10,
			add(n) { return this.base + n; }
		};
		let merged = { ...shorthand, z: 3, ...{ y: 20 } };
		let from_array = { ...["a", "b"] };
	)">();
	CHECK(out.ok());
	CHECK(out["pet_name"].to<std::string>() == "rex");
	CHECK(out["no_pet"].is_undefined());
	CHECK(out["no_deep"].is_undefined());
	CHECK(out["idx"].to<std::string>() == "rex");
	CHECK(out["bark"].to<std::string>() == "woof, rex");
	CHECK(out["no_call"].is_undefined());
	CHECK(out["missing_method"].is_undefined());
	CHECK(out["shorthand"]["x"].to<int>() == 1);
	CHECK(out["shorthand"]["y"].to<int>() == 2);
	CHECK(out["methods"].is_object());
	CHECK(out["merged"]["x"].to<int>() == 1);
	CHECK(out["merged"]["y"].to<int>() == 20); // later spread wins
	CHECK(out["merged"]["z"].to<int>() == 3);
	CHECK(out["from_array"]["0"].to<std::string>() == "a");
	CHECK(out["from_array"]["1"].to<std::string>() == "b");

	// method shorthand binds this through a normal method call
	auto m = ctjs::run<R"(
		let counter = { n: 5, bump(by) { this.n += by; return this.n; } };
		let after = counter.bump(3);
	)">();
	CHECK(m.ok());
	CHECK(m["after"].to<int>() == 8);
}

static void generators_instanceof_regex() {
	auto out = ctjs::run<R"(
		function* count(n) {
			for (let i = 1; i <= n; i++) { yield i; }
			return "spent";
		}
		let squares = [];
		for (const v of count(4)) { squares.push(v * v); }
		let g = count(2);
		let first = g.next();
		let second = g.next();
		let third = g.next();

		class Dog {
			constructor(name) { this.name = name; }
			speak() { return this.name + " woofs"; }
		}
		class Cat { }
		let rex = new Dog("rex");
		let is_dog = rex instanceof Dog;
		let is_cat = rex instanceof Cat;
		let plain_is = ({}) instanceof Dog;

		let re = /(\d+)-(\d+)/;
		let hit = re.test("range 10-25 ok");
		let miss = re.test("no numbers here");
		let parts = re.exec("range 10-25 ok");
		let nums = "a1b22c333".match(/\d+/g);
		let swapped = "10-25".replace(/(\d+)-(\d+)/, "$2-$1");
		let stripped = "a1b22c3".replace(/\d+/g, "#");
		let words = "one, two,three".split(/[,]\s*/);
		let ic = /HELLO/i.test("say hello please");
		let anch = /^ab+c$/.test("abbbc");
		let division_still_works = 10 / 2 / 5;
	)">();
	CHECK(out.ok());
	CHECK(out["squares"][0].to<int>() == 1);
	CHECK(out["squares"][3].to<int>() == 16);
	CHECK(out["first"]["value"].to<int>() == 1);
	CHECK(!out["first"]["done"].to<bool>());
	CHECK(out["second"]["value"].to<int>() == 2);
	CHECK(out["third"]["done"].to<bool>());
	CHECK(out["third"]["value"].to<std::string>() == "spent");
	CHECK(out["is_dog"].to<bool>());
	CHECK(!out["is_cat"].to<bool>());
	CHECK(!out["plain_is"].to<bool>());
	CHECK(out["hit"].to<bool>());
	CHECK(!out["miss"].to<bool>());
	CHECK(out["parts"][0].to<std::string>() == "10-25");
	CHECK(out["parts"][1].to<std::string>() == "10");
	CHECK(out["parts"][2].to<std::string>() == "25");
	CHECK(out["nums"][0].to<std::string>() == "1");
	CHECK(out["nums"][1].to<std::string>() == "22");
	CHECK(out["nums"][2].to<std::string>() == "333");
	CHECK(out["swapped"].to<std::string>() == "25-10");
	CHECK(out["stripped"].to<std::string>() == "a#b#c#");
	CHECK(out["words"][0].to<std::string>() == "one");
	CHECK(out["words"][1].to<std::string>() == "two");
	CHECK(out["words"][2].to<std::string>() == "three");
	CHECK(out["ic"].to<bool>());
	CHECK(out["anch"].to<bool>());
	CHECK(out["division_still_works"].to<double>() == 1.0);

	// yield outside a generator is an error, not a silent no-op
	auto bad = ctjs::run<R"(
		function plain() { yield 1; }
		let boom = "";
		try { plain(); } catch (e) { boom = e.name; }
	)">();
	CHECK(bad.ok());
	CHECK(bad["boom"].to<std::string>() == "SyntaxError");
}

static void labels_date_accessors_extends() {
	auto out = ctjs::run<R"(
		// labeled break/continue across nested loops
		let pairs = [];
		outer:
		for (let i = 0; i < 3; i++) {
			for (let j = 0; j < 3; j++) {
				if (j === 2) { continue outer; }
				if (i === 2) { break outer; }
				pairs.push(i * 10 + j);
			}
		}

		// computed keys + object accessors
		let k = "dyn";
		let obj = {
			[k]: 1,
			["a" + "b"]: 2,
			_n: 5,
			get n() { return this._n; },
			set n(v) { this._n = v * 2; }
		};
		let dyn_read = obj.dyn;
		let ab_read = obj.ab;
		let got = obj.n;
		obj.n = 10;
		let after_set = obj._n;

		// class getters/setters
		class Temp {
			constructor(c) { this._c = c; }
			get celsius() { return this._c; }
			set celsius(v) { this._c = v; }
			get fahrenheit() { return this._c * 9 / 5 + 32; }
		}
		let t = new Temp(100);
		let f = t.fahrenheit;
		t.celsius = 0;
		let f2 = t.fahrenheit;

		// extends: base ctor + method inheritance + override + instanceof chain
		class Animal {
			constructor(name) { this.name = name; }
			speak() { return this.name + " makes a sound"; }
			kind() { return "animal"; }
		}
		class Dog extends Animal {
			speak() { return this.name + " barks"; }
		}
		let d = new Dog("rex");
		let d_speak = d.speak();
		let d_kind = d.kind();
		let d_name = d.name;
		let is_dog = d instanceof Dog;
		let is_animal = d instanceof Animal;

		// Date, UTC subset
		let epoch = new Date(0);
		let y = epoch.getUTCFullYear();
		let iso = new Date(1609459200000).toISOString(); // 2021-01-01T00:00:00Z
		let is_date = epoch instanceof Date;
		let now_positive = Date.now() > 0;
	)">();
	CHECK(out.ok());
	CHECK(out["pairs"][0].to<int>() == 0);   // i0 j0
	CHECK(out["pairs"][1].to<int>() == 1);   // i0 j1
	CHECK(out["pairs"][2].to<int>() == 10);  // i1 j0
	CHECK(out["pairs"][3].to<int>() == 11);  // i1 j1
	CHECK(out["pairs"].to_string().size() > 0);
	CHECK(out["dyn_read"].to<int>() == 1);
	CHECK(out["ab_read"].to<int>() == 2);
	CHECK(out["got"].to<int>() == 5);
	CHECK(out["after_set"].to<int>() == 20); // setter doubled
	CHECK(out["f"].to<double>() == 212.0);
	CHECK(out["f2"].to<double>() == 32.0);
	CHECK(out["d_speak"].to<std::string>() == "rex barks");
	CHECK(out["d_kind"].to<std::string>() == "animal");
	CHECK(out["d_name"].to<std::string>() == "rex");
	CHECK(out["is_dog"].to<bool>());
	CHECK(out["is_animal"].to<bool>());
	CHECK(out["y"].to<int>() == 1970);
	CHECK(out["iso"].to<std::string>() == "2021-01-01T00:00:00.000Z");
	CHECK(out["is_date"].to<bool>());
	CHECK(out["now_positive"].to<bool>());

	// exactly 4 pairs pushed (break outer fired at i2)
	CHECK(out["pairs"].to<std::string>() == "0,1,10,11");
}

static void classes_prototypes_super_statics() {
	auto out = ctjs::run<R"(
		class Shape {
			constructor(name) { this.name = name; }
			describe() { return "a " + this.name; }
			get label() { return "[" + this.name + "]"; }
			static kinds() { return "shapes"; }
			static origin = "geometry";
		}
		class Circle extends Shape {
			constructor(r) { super("circle"); this.r = r; }
			describe() { return super.describe() + " of radius " + this.r; }
			area() { return 3 * this.r * this.r; }
		}
		let c = new Circle(2);
		let d = c.describe();               // override + super.describe()
		let inherited_label = c.label;      // getter inherited from Shape
		let a = c.area();
		let is_circle = c instanceof Circle;
		let is_shape = c instanceof Shape;
		let base_kind = Shape.kinds();
		let sub_kind = Circle.kinds();      // static inherited through extends
		let origin = Shape.origin;          // static field
		let c2 = new Circle(3);
		let shared = c.describe === c2.describe;                 // one method obj
		let proto_ok = Object.getPrototypeOf(c) === Circle.prototype;

		// instance fields + computed member names (static and instance)
		const key = "dynamic";
		class Widget {
			count = 0;
			[key]() { return "computed!"; }
			static [key + "Static"]() { return "cs"; }
			bump() { this.count += 1; return this.count; }
		}
		let w = new Widget();
		let field0 = w.count;
		let bumped = w.bump();
		let comp = w.dynamic();
		let comp_static = Widget.dynamicStatic();

		// plain-function prototype methods
		function Point(x) { this.x = x; }
		Point.prototype.getX = function() { return this.x; };
		let pt = new Point(7);
		let px = pt.getX();
		let pt_is = pt instanceof Point;

		// Object helpers
		let obj = { a: 1, b: 2 };
		let keys = Object.keys(obj).join(",");
		let merged = Object.assign({}, obj, { b: 3, c: 4 });
		let created = Object.create(obj);
		let inherited_a = created.a;        // resolves up the prototype

		// Date string parsing
		let parsed = new Date("2021-06-15T12:30:00Z");
		let py = parsed.getUTCFullYear();
		let pmo = parsed.getMonth();
		let pday = parsed.getDate();
		let ph = parsed.getHours();
		let epoch1 = Date.parse("1970-01-02");
	)">();
	CHECK(out.ok());
	CHECK(out["d"].to<std::string>() == "a circle of radius 2");
	CHECK(out["inherited_label"].to<std::string>() == "[circle]");
	CHECK(out["a"].to<double>() == 12.0);
	CHECK(out["is_circle"].to<bool>());
	CHECK(out["is_shape"].to<bool>());
	CHECK(out["base_kind"].to<std::string>() == "shapes");
	CHECK(out["sub_kind"].to<std::string>() == "shapes");
	CHECK(out["origin"].to<std::string>() == "geometry");
	CHECK(out["shared"].to<bool>());
	CHECK(out["proto_ok"].to<bool>());
	CHECK(out["field0"].to<int>() == 0);
	CHECK(out["bumped"].to<int>() == 1);
	CHECK(out["comp"].to<std::string>() == "computed!");
	CHECK(out["comp_static"].to<std::string>() == "cs");
	CHECK(out["px"].to<int>() == 7);
	CHECK(out["pt_is"].to<bool>());
	CHECK(out["keys"].to<std::string>() == "a,b");
	CHECK(out["merged"]["a"].to<int>() == 1);
	CHECK(out["merged"]["b"].to<int>() == 3);
	CHECK(out["merged"]["c"].to<int>() == 4);
	CHECK(out["inherited_a"].to<int>() == 1);
	CHECK(out["py"].to<int>() == 2021);
	CHECK(out["pmo"].to<int>() == 5); // June is month 5 (0-based)
	CHECK(out["pday"].to<int>() == 15);
	CHECK(out["ph"].to<int>() == 12);
	CHECK(out["epoch1"].to<double>() == 86400000.0);
}

static void constant_folding() {
	auto out = ctjs::run<R"(
		let a = 2 + 3 * 4;               // -> 14 at compile time
		let b = (10 - 2) ** 2;           // -> 64
		let c = 100 % 7 + 0x10;          // -> 2 + 16 = 18
		let f = 5 > 3 && 2 < 1;          // -> false
		let ran = 0;
		let d = true ? 42 : (ran = 99);  // dead branch dropped: ran untouched
		let e = false && (ran = 88);     // short-circuited away: ran untouched
		let g = 1 + 2 + 3 + x;           // partial: 6 folded, + dynamic x
	)">({{"x", ctjs::value{10.0}}});
	CHECK(out.ok());
	CHECK(out["a"].to<int>() == 14);
	CHECK(out["b"].to<int>() == 64);
	CHECK(out["c"].to<int>() == 18);
	CHECK(!out["f"].to<bool>());
	CHECK(out["d"].to<int>() == 42);
	CHECK(!out["e"].to<bool>());
	CHECK(out["ran"].to<int>() == 0); // proves the folded-out branches never ran
	CHECK(out["g"].to<int>() == 16);  // 6 (folded) + 10 (dynamic)

	// folded results still match the interpreter exactly for the tricky
	// cases (negative zero, chained comparison result, hex)
	auto out2 = ctjs::run<R"(
		let h = 10 / 4;                  // 2.5 (division of ints is exact)
		let i = 2 ** 8 - 1;              // 255
		let j = (1 + 1 === 2);           // true
	)">();
	CHECK(out2["h"].to<double>() == 2.5);
	CHECK(out2["i"].to<int>() == 255);
	CHECK(out2["j"].to<bool>());
}

static void string_folding() {
	auto out = ctjs::run<R"(
		let s = "a" + "b" + "c";        // -> "abc" at compile time
		let u = "id-" + 42;             // -> "id-42" (int coercion)
		let v = 7 + "up";               // -> "7up"
		let w = "x" + true + null;      // -> "xtruenull"
		let esc = "a\tb" + "!";         // cook preserved: -> "a\tb!"
		let neg = "n" + -5;             // -> "n-5"
		let dyn = "hi " + name;         // partial: name is dynamic
		let mixed = "sum=" + (2 + 3);   // inner folds to 5 -> "sum=5"
	)">({{"name", ctjs::value{std::string{"there"}}}});
	CHECK(out.ok());
	CHECK(out["s"].to<std::string>() == "abc");
	CHECK(out["u"].to<std::string>() == "id-42");
	CHECK(out["v"].to<std::string>() == "7up");
	CHECK(out["w"].to<std::string>() == "xtruenull");
	CHECK(out["esc"].to<std::string>() == "a\tb!");
	CHECK(out["neg"].to<std::string>() == "n-5");
	CHECK(out["dyn"].to<std::string>() == "hi there");
	CHECK(out["mixed"].to<std::string>() == "sum=5");

	// folded strings behave like any other string downstream
	auto out2 = ctjs::run<R"(
		let g = ("Hello, " + "World").toUpperCase();
		let n = ("abc" + "de").length;
	)">();
	CHECK(out2["g"].to<std::string>() == "HELLO, WORLD");
	CHECK(out2["n"].to<int>() == 5);
}

static void function_folding() {
	auto out = ctjs::run<R"(
		let sq = (x => x * x)(5);                            // -> 25
		let sum = (function (a, b) { return a + b; })(2, 3); // -> 5
		let clamp = (n => n > 10 ? 10 : n)(42);              // -> 10
		let greet = (name => "hi " + name)("bob");           // -> "hi bob"
		let id = (x => x)(7);                                // -> 7
		let mixed = (k => k * 2 + offset)(5);                // partial: 10 + offset
		let dynArg = (x => x + 1)(n);                        // not folded: n dynamic
	)">({{"offset", ctjs::value{3.0}}, {"n", ctjs::value{40.0}}});
	CHECK(out.ok());
	CHECK(out["sq"].to<int>() == 25);
	CHECK(out["sum"].to<int>() == 5);
	CHECK(out["clamp"].to<int>() == 10);
	CHECK(out["greet"].to<std::string>() == "hi bob");
	CHECK(out["id"].to<int>() == 7);
	CHECK(out["mixed"].to<int>() == 13);   // body has a free var: runs, still correct
	CHECK(out["dynArg"].to<int>() == 41);  // dynamic arg: runs, still correct
}

static void named_function_folding() {
	auto out = ctjs::run<R"(
		function sq(x) { return x * x; }
		function cube(x) { return x * sq(x); }                     // calls sq
		function fact(n) { return n <= 1 ? 1 : n * fact(n - 1); }  // recursion
		function label(name) { return "id-" + name; }

		let a = sq(6);            // -> 36 at compile time
		let b = cube(3);          // -> 27 (folds the nested sq call)
		let c = fact(6);          // -> 720 (bounded recursion)
		let d = label(42);        // -> "id-42"
		let e = sq(4) + fact(4);  // -> 16 + 24 = 40
		let dyn = sq(k);          // not folded: k is dynamic - still runs
	)">({{"k", ctjs::value{5.0}}});
	CHECK(out.ok());
	CHECK(out["a"].to<int>() == 36);
	CHECK(out["b"].to<int>() == 27);
	CHECK(out["c"].to<int>() == 720);
	CHECK(out["d"].to<std::string>() == "id-42");
	CHECK(out["e"].to<int>() == 40);
	CHECK(out["dyn"].to<int>() == 25); // runs at runtime, still correct

	// a function called with constants AND dynamically both work: the
	// constant call folds, the dynamic one runs against the real def
	auto out2 = ctjs::run<R"(
		function dbl(x) { return x * 2; }
		let folded = dbl(21);   // -> 42
		let live = dbl(n);      // runs: n * 2
	)">({{"n", ctjs::value{10.0}}});
	CHECK(out2["folded"].to<int>() == 42);
	CHECK(out2["live"].to<int>() == 20);
}

static void constexpr_interpreter() {
	// the SAME constexpr eval, called at runtime, agrees with itself and
	// with run() - one interpreter, two evaluation times
	CHECK(ctjs::eval<"let s = 0; for (let i = 1; i <= 100; i++) { s += i; } s;">().to<int>() == 5050);
	CHECK(ctjs::eval<"function fib(n){ return n < 2 ? n : fib(n-1) + fib(n-2); } fib(15);">().to<int>() == 610);
	CHECK(ctjs::eval<"let o = {}; o.a = 1; o.b = 2; o.a + o.b;">().to<int>() == 3);
	CHECK(ctjs::eval<"'result: ' + (6 * 7);">().to<std::string>() == "result: 42");
	// and run() gives the same globals
	auto out = ctjs::run<"let answer = 6 * 7;">();
	CHECK(out["answer"].to<int>() == 42);
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
	async_and_promises();
	json_parse();
	optional_chaining_and_object_sugar();
	generators_instanceof_regex();
	labels_date_accessors_extends();
	classes_prototypes_super_statics();
	constant_folding();
	string_folding();
	function_folding();
	named_function_folding();
	constexpr_interpreter();
	if (failures == 0) {
		std::printf("runtime suite: all checks passed\n");
	}
	return failures;
}
