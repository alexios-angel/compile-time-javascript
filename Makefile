.PHONY: default all clean pch single-header single-header/ctjs.hpp

default: all

CXX_STANDARD := 23

PYTHON := python3

# THE compiler: the stack's std::embed clang (see compile-time-browser
# tools/clang-std-embed or the embed repo's release); plain clang++ as
# a fallback for standalone checkouts. gcc paths are gone (clang-only).
ifeq ($(origin CXX),default)
CTB_CLANG := $(wildcard ../../tools/clang-std-embed/bin/clang++)
CXX := $(if $(CTB_CLANG),$(CTB_CLANG),clang++)
endif
CXX_IS_CLANG := yes

# the constexpr value parser folding whole scripts in static_asserts
# wants a raised step budget; nothing else is special anymore
CONSTEXPR_FLAGS := -fconstexpr-steps=500000000 -fconstexpr-depth=1024

# ctlark and ctll come from a git submodule (run `git submodule update --init`
# once after cloning). The extra <sub>/include/ctlark and <sub>/include/ctll
# entries let the headers' relative `"../ctlark.hpp"`-style quoted includes
# resolve through the quoted-include -I fallback (the compiler appends the
# literal "../ctlark.hpp" to each -I dir).
SUBMODULE_INCLUDES := \
	-Iexternal/compile-time-lark/include \
	-Iexternal/compile-time-lark/include/ctlark \
	-Iexternal/compile-time-lark/include/ctll

override CXXFLAGS := $(CXXFLAGS) -std=c++$(CXX_STANDARD) -Iinclude $(SUBMODULE_INCLUDES) $(CONSTEXPR_FLAGS) -O2 -pedantic -Wall -Wextra -Werror -Wconversion

# precompiled header: the umbrella compiles once (seconds - the
# grammar bake died with the type path)
ifeq ($(CXX_IS_CLANG),yes)
PCH := ctjs.pch
PCH_USE = -include-pch $(PCH)
else
PCH := include/ctjs.hpp.gch
PCH_USE =
endif

# scripts PARSE during each test's compilation; the tests then RUN -
# every tests/*.cpp is an executable with checks, and `make` executes
# them all (a failing check is a non-zero exit)
TESTS := $(wildcard tests/*.cpp)
BINARIES := $(TESTS:%.cpp=%)
DEPENDENCY_FILES := $(TESTS:%.cpp=%.d)

all: run-tests

$(BINARIES): %: %.cpp $(PCH)
	@$(CXX) $(CXXFLAGS) $(PCH_USE) -MMD $< -o $@

run-tests: $(BINARIES)
	@for t in $(BINARIES); do printf '== %s\n' "$$t"; ./$$t || exit 1; done

pch: $(PCH)

$(PCH): include/ctjs.hpp $(wildcard include/ctjs/*.hpp)
	@$(CXX) $(CXXFLAGS) -x c++-header $< -o $@

-include $(DEPENDENCY_FILES)

clean:
	rm -f $(BINARIES) $(DEPENDENCY_FILES) ctjs.pch include/ctjs.hpp.gch

# needs python3 with the quom package
single-header: single-header/ctjs.hpp

single-header/ctjs.hpp:
	$(PYTHON) -m quom include/ctjs.hpp ctjs.hpp.tmp \
		-I external/compile-time-lark/include \
		-I external/compile-time-lark/include/ctlark \
		-I external/compile-time-lark/include/ctll
	echo "/*" > single-header/ctjs.hpp
	cat LICENSE >> single-header/ctjs.hpp
	echo "*/" >> single-header/ctjs.hpp
	cat ctjs.hpp.tmp >> single-header/ctjs.hpp
	rm ctjs.hpp.tmp
