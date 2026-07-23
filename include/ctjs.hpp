#ifndef CTJS__HPP
#define CTJS__HPP

#include "ctjs/value.hpp"
#include "ctjs/builtins.hpp"
#include "ctjs/script.hpp"

// ctjs: JavaScript parsed while your code compiles, executed when it
// runs.
//
//   constexpr auto & app = ctjs::script<R"(
//       function greet(name) { return "hi " + name + "!"; }
//       let total = 0;
//       for (let i = 1; i <= 10; i++) { total += i; }
//       console.log("total", total);
//   )">;
//
//   auto out = app.run();
//   assert(out.ok());
//   assert(out.console() == "total 55\n");
//   assert(out["total"].to<int>() == 55);
//   assert(out.call("greet", "ctjs").to<std::string>() == "hi ctjs!");
//
//   static_assert(ctjs::script<"let x = 1;">.valid);
//   static_assert(!ctjs::is_valid<"let o = { a: };">); // structural breaks fail
//
// The SOURCE can ride as a template argument - its validity is proven
// during compilation by the CONSTEXPR value parser (vparse.hpp), so a
// syntax error in script<...> fails the build - or arrive as an
// ordinary runtime string through run_value (an embedded asset, a
// network payload). Both execute through the same value tree-walking
// interpreter (vinterp.hpp): a dynamic value model with real closures,
// JS coercions, prototypes, classes and try/catch.
//
// Hosts inject native functions as globals (ctjs::binding +
// ctjs::native), and call script-defined functions from C++ through
// run_result::call - the seam a compile-time browser hangs DOM APIs on.

#endif
