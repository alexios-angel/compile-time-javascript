# Examples

Self-contained programs, each compilable against `../include` (or the
single header). Build with `make`, build and run with `make run`; they
also build and run as tests through CMake/CTest.

| File | Shows |
|------|-------|
| [`page.cpp`](page.cpp) | a web page baked into the binary: compile-time requirement checks, attribute values as constants, iteration over the nav links — written with void elements, unquoted attributes and `<li>` run-ons, landing in a browser-shaped DOM |
| [`wellformed.cpp`](wellformed.cpp) | `is_valid` as a bool: the HTML5 conveniences that parse silently, and the author mistakes (crossing close tags, duplicate attributes, `<div/>`) that stay compile errors |
| [`introspection.cpp`](introspection.cpp) | a generic recursive visitor over any document: kind dispatch, attribute/child iteration, compile-time re-serialization |
| [`iteration.cpp`](iteration.cpp) | brackets and iteration: `doc["tag"]`/`doc[1]` lookups with `operator[]` (case-insensitive), children and attributes as uniform views with range-for and `<algorithm>` |
