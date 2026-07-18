# Examples

Self-contained programs, each compilable against `../include` (or the
single header). Build with `make` (the grammar PCH from the repo root
is reused, or built here if missing - that first bake is the slow
step), build and run with `make run`; they also build and run as tests
through CMake/CTest.

| File | Shows |
|------|-------|
| [`hello.cpp`](hello.cpp) | the hero demo: a script parsed at compile time, run at runtime - console capture, reading globals back, calling script functions (with closures) from C++ |
| [`host.cpp`](host.cpp) | the host seam a compile-time browser will use: native C++ functions injected as script globals, the script wiring its logic at startup, the host's event loop calling script handlers back |
