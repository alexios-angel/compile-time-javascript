# THE compiler: the stack's std::embed clang (see compile-time-browser
# tools/clang-std-embed or the embed repo's release); plain clang++ as
# a fallback for standalone checkouts. gcc paths are gone (clang-only).
ifeq ($(origin CXX),default)
CTB_CLANG := $(wildcard ../../../tools/clang-std-embed/bin/clang++)
CXX := $(if $(CTB_CLANG),$(CTB_CLANG),clang++)
endif

CONSTEXPR_FLAGS := -fconstexpr-steps=500000000 -fconstexpr-depth=1024
SUBMODULE_INCLUDES := \
	-I../external/compile-time-lark/include \
	-I../external/compile-time-lark/include/ctlark \
	-I../external/compile-time-lark/include/ctll

CXXFLAGS := -std=c++23 -I../include $(SUBMODULE_INCLUDES) $(CONSTEXPR_FLAGS) -O2 -pedantic -Wall -Wextra -Werror -Wconversion

SOURCES := $(wildcard *.cpp)
TARGETS := $(SOURCES:%.cpp=%)

# reuse (or build) the umbrella PCH - compiles in seconds now that the
# grammar bake is gone with the type path
PCH := ../ctjs.pch
PCH_USE = -include-pch $(PCH)

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
