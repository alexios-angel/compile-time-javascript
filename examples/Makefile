CXX ?= c++

CXX_IS_CLANG := $(shell $(CXX) --version 2>/dev/null | grep -qi clang && echo yes)
ifeq ($(CXX_IS_CLANG),yes)
CONSTEXPR_FLAGS := -fconstexpr-steps=500000000 -fconstexpr-depth=1024 -fbracket-depth=2048
else
CONSTEXPR_FLAGS := -fconstexpr-ops-limit=3000000000 -fconstexpr-loop-limit=10000000 -fconstexpr-depth=1024
endif
SUBMODULE_INCLUDES := \
	-I../external/compile-time-lark/include \
	-I../external/compile-time-lark/include/ctlark \
	-I../external/compile-time-lark/include/ctll

CXXFLAGS := -std=c++20 -I../include $(SUBMODULE_INCLUDES) $(CONSTEXPR_FLAGS) -O2 -Wall -Wextra -pedantic

SOURCES := $(wildcard *.cpp)
TARGETS := $(SOURCES:%.cpp=%)

# reuse (or build) the grammar PCH - the one-time slow step; gcc picks
# up ../include/ctjs.hpp.gch automatically, clang needs -include-pch
ifeq ($(CXX_IS_CLANG),yes)
PCH := ../ctjs.pch
PCH_USE = -include-pch $(PCH)
else
PCH := ../include/ctjs.hpp.gch
PCH_USE =
endif

all: $(TARGETS)

$(PCH): ../include/ctjs.hpp
	$(CXX) $(CXXFLAGS) -x c++-header $< -o $@

%: %.cpp $(PCH)
	$(CXX) $(CXXFLAGS) $(PCH_USE) $< -o $@

run: all
	@for t in $(TARGETS); do printf '== %s ==\n' "$$t"; ./$$t; echo; done

clean:
	rm -f $(TARGETS)

.PHONY: all run clean
