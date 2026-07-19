// V8-differential corpus: each `// === name` block is one snippet.
// tools/gen-v8diff.py runs every snippet under node (V8) to capture the
// expected output, then emits tests/v8diff.cpp where ctjs runs the same
// snippet and the outputs are compared byte-for-byte. Snippets must be
// fully semicolon-terminated (ctjs has no ASI - documented) and use only
// console.log for output.

// === const-reassignment-throws
const a = 1;
a = 2;
console.log("unreached");

// === const-object-mutation-allowed
const o = {x: 1};
o.x = 2;
console.log(o.x);

// === var-hoisting-reads-undefined
console.log(x);
var x = 1;
console.log(x);

// === var-escapes-block
if (true) { var y = 5; }
console.log(y);

// === var-escapes-for-body
for (var i = 0; i < 3; i++) { var w = i; }
console.log(i, w);

// === let-tdz-throws
console.log(z);
let z = 1;

// === let-block-scoped
let b = 1;
{ let b = 2; console.log(b); }
console.log(b);

// === for-let-closures-per-iteration
let fs = [];
for (let i = 0; i < 3; i++) { fs.push(function() { return i; }); }
console.log(fs[0](), fs[1](), fs[2]());

// === for-var-closures-shared
let gs = [];
for (var j = 0; j < 3; j++) { gs.push(function() { return j; }); }
console.log(gs[0](), gs[1](), gs[2]());

// === function-hoisting
console.log(f());
function f() { return 42; }

// === var-shadows-in-function
var v = "outer";
function g() { console.log(v); var v = "inner"; console.log(v); }
g();

// === this-in-method
var obj = {n: 5};
obj.get = function() { return this.n; };
console.log(obj.get());

// === this-lost-on-extraction
var obj2 = {n: 7};
obj2.get = function() { return this; };
var f2 = obj2.get;
console.log(f2() === undefined || f2() === globalThis);

// === plus-coercions
console.log([] + [], [] + 1, 1 + "2", "3" * "4", null + 1, true + true, undefined + 1);

// === loose-equality-table
console.log(null == undefined, null === undefined, NaN == NaN, 0 == "", "0" == false, "" == false, [] == false);

// === relational-null
console.log(null >= 0, null > 0, null == 0);

// === typeof-table
console.log(typeof undefined, typeof null, typeof 1, typeof "s", typeof true, typeof {}, typeof [], typeof function(){});

// === array-sort-is-lexicographic
console.log([10, 1, 2].sort());

// === array-holes-and-length
var arr = [];
arr[3] = 1;
console.log(arr.length, arr[0]);

// === string-methods-edges
console.log("abc".slice(-2), "abc".indexOf("z"), " a ".trim(), "aaa".replace("a", "b"));

// === number-printing
console.log(0.1 + 0.2, 1e21, 1e-7, -0, 1 / 0, -1 / 0, 0 / 0);

// === ternary-and-comma
var t = (1, 2, 3);
console.log(t, t > 2 ? "big" : "small");

// === logical-value-returns
console.log(0 || "fallback", 1 && "yes", null ?? "coalesced");

// === increment-semantics
var c = 5;
console.log(c++, c, ++c, c);

// === delete-and-in
var d = {k: 1};
console.log("k" in d);
delete d.k;
console.log("k" in d, d.k);

// === switch-fallthrough
switch (2) {
  case 1: console.log("one");
  case 2: console.log("two");
  case 3: console.log("three"); break;
  case 4: console.log("four");
}

// === do-while
var n = 0;
do { n++; } while (n < 3);
console.log(n);

// === string-length-ascii
console.log("hello".length, "hello"[1], "hello".charCodeAt(0));

// === shadowed-builtin-parseInt
console.log(parseInt("08"), parseInt("0x10"), parseInt("12px"), parseInt(""));

// === tostring-conversions
console.log(String(null), String(undefined), String([1, 2]), String({}));

// === nested-closure-counter
function make() { var count = 0; return function() { count++; return count; }; }
var m = make();
m();
console.log(m(), m());
