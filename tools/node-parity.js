// Runs the expectations hard-coded in tests/runtime.cpp under REAL node
// (a dev tool, not part of the build):  node tools/node-parity.js
const checks = [];
function eq(label, actual, expected) {
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  checks.push([label, a === e, a, e]);
}

{ let total = 0; for (let i = 1; i <= 10; i++) { total += i; } eq("total", total, 55); }
{
  let xs = [3, 1, 4, 1, 5, 9];
  xs.push(2);
  let doubled = xs.map((x) => x * 2);
  let big = xs.filter((x) => x >= 4);
  let sum = xs.reduce((a, b) => a + b, 0);
  let text = xs.join("-");
  let sliced = xs.slice(1, 3);
  eq("doubled", doubled, [6,2,8,2,10,18,4]);
  eq("len-sum-text", [xs.length, sum, text], [7, 25, "3-1-4-1-5-9-2"]);
  eq("big-count", big.length, 3);
  eq("sliced-tostring", String(sliced), "1,4");
}
{
  let user = { name: "ada", "logins": 3, tags: ["admin", "dev"] };
  user.logins += 1;
  user["last"] = "today";
  eq("json", JSON.stringify(user), '{"name":"ada","logins":4,"tags":["admin","dev"],"last":"today"}');
}
{
  let s = "Hello,\tWorld\n";
  eq("up", s.trim().toUpperCase(), "HELLO,\tWORLD");
  eq("parts", "a,b,,c".split(",").length, 4);
  eq("idx", "banana".indexOf("nan"), 2);
  eq("pad", String(7).padStart(3, "0"), "007");
  eq("bits", 'single ' + "double" + " " + 65, "single double 65");
}
{
  eq("looseeq", [1 == "1", 1 === "1", null == undefined, null === undefined], [true,false,true,false]);
  eq("typeofnull", typeof null, "object");
  eq("mul", "5" * "4", 20);
  eq("plus", "5" + 4, "54");
  eq("floats", String(0.1 + 0.2), "0.30000000000000004");
  eq("pow", 2 ** 10, 1024);
  eq("inf", String(1 / 0), "Infinity");
}
{
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
  eq("control", log.join("") + " " + n + " " + seen + " " + pick + " " + t, "abcxy 3 9 fallback big");
}
{
  let steps = [];
  function risky(x) { if (x < 0) { throw { name: "RangeError", message: "negative" }; } return x; }
  try { steps.push(risky(1)); steps.push(risky(-1)); steps.push(risky(2)); }
  catch (e) { steps.push(e.message); }
  finally { steps.push("done"); }
  eq("trail", steps.join(","), "1,negative,done");
}
{
  eq("math", [Math.floor(3.7)+Math.ceil(3.2)+Math.abs(-2), Math.max(1,9,4)+Math.min(5,2),
              Math.pow(2,8)+Math.sqrt(81), parseInt("42px")+parseFloat("2.5rem"),
              parseInt("ff",16), Number("12")+Number(true)],
     [9, 11, 265, 44.5, 255, 13]);
  eq("nan-arr", [isNaN(parseInt("nope")), Array.isArray([1]) && !Array.isArray("no")], [true, true]);
  eq("tostr", (255).toString() + "/" + (3.14159).toFixed(2), "255/3.14");
}
// console.log rendering of arrays (node inspect style)
console.log([6, 2, 8, 2, 10, 18, 4]);
console.log({ k: 'v' });
console.log("total", 55);

let bad = 0;
for (const [label, ok, a, e] of checks) {
  if (!ok) { console.error(`MISMATCH ${label}: got ${a}, expected ${e}`); bad++; }
}
console.log(bad === 0 ? "node parity: all expectations match" : `${bad} mismatches`);
process.exit(bad);
