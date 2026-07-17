.PHONY: default all clean pch single-header single-header/ctjs.hpp

default: all

CXX_STANDARD := 20

PYTHON := python3

# Earley at compile time needs more constexpr budget than the defaults
CXX_IS_CLANG := $(shell $(CXX) --version 2>/dev/null | grep -qi clang && echo yes)
ifeq ($(CXX_IS_CLANG),yes)
CONSTEXPR_FLAGS := -fconstexpr-steps=500000000 -fconstexpr-depth=1024 -fbracket-depth=2048
else
CONSTEXPR_FLAGS := -fconstexpr-ops-limit=3000000000 -fconstexpr-loop-limit=10000000 -fconstexpr-depth=1024
endif

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

# precompiled header: parsing the XML grammar text and compiling its
# tables happens once here instead of once per translation unit
ifeq ($(CXX_IS_CLANG),yes)
PCH := ctjs.pch
PCH_USE = -include-pch $(PCH)
else
PCH := include/ctjs.hpp.gch
PCH_USE =
endif

TESTS := $(wildcard tests/*.cpp)
OBJECTS := $(TESTS:%.cpp=%.o)
DEPENDENCY_FILES := $(OBJECTS:%.o=%.d)

all: $(OBJECTS)

$(OBJECTS): %.o: %.cpp $(PCH)
	$(CXX) $(CXXFLAGS) $(PCH_USE) -MMD -c $< -o $@

pch: $(PCH)

$(PCH): include/ctjs.hpp
	$(CXX) $(CXXFLAGS) -x c++-header $< -o $@

-include $(DEPENDENCY_FILES)

clean:
	rm -f $(OBJECTS) $(DEPENDENCY_FILES) ctjs.pch include/ctjs.hpp.gch

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
