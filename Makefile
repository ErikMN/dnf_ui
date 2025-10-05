CXX = g++
CXXFLAGS += -std=c++20 -Werror -MMD -MP -pipe -fdiagnostics-color=always
PROGS = dnf_ui
LDLIBS = -lm

PKGS += libdnf5 gtk4
PKG_OK := $(shell pkg-config --exists $(PKGS) && echo yes)
ifeq ($(PKG_OK),yes)
  LDLIBS += $(shell pkg-config --libs $(PKGS))
  CPPFLAGS += $(shell pkg-config --cflags $(PKGS))
else
  $(error "Missing dependencies: please install development packages for $(PKGS)")
endif

SRCS = $(wildcard *.cpp)
OBJS = $(SRCS:.cpp=.o)
DEPS = $(SRCS:.cpp=.d)

-include $(DEPS)

# FINAL=y
ifeq ($(FINAL), y)
  LDFLAGS += -s
  CXXFLAGS += -DNDEBUG -g0 -O2
  CXXFLAGS += -Wpedantic -Wextra -Wmaybe-uninitialized
  CXXFLAGS += -W -Wformat=2 -Wpointer-arith -Winline
  CXXFLAGS += -Wdisabled-optimization -Wfloat-equal -Wall
else
  # ASAN=y
  CXXFLAGS += -g3 -DDEBUG_BUILD
  ifeq ($(ASAN), y)
    CXXFLAGS += -fsanitize=address -O1 -fno-omit-frame-pointer
    LDLIBS += -fsanitize=address
  endif
endif

.PHONY: all
all: $(PROGS)

.PHONY: debug
debug:
	@echo "*** Debug info:"
	@echo "Source-files:" $(SRCS)
	@echo "Object-files:" $(OBJS)
	@echo "Compiler-flags:" $(CXXFLAGS)
	@echo "Linker-flags:" $(LDFLAGS)
	@echo "Linker-libs:" $(LDLIBS)

$(PROGS): $(OBJS)
	@echo "Linking $@..."
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

%.o: %.cpp
	@echo "Compiling $<..."
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

.PHONY: run
run: clean $(PROGS)
	@./$(PROGS)

.PHONY: valgrind
valgrind: $(PROGS)
	@valgrind \
		--tool=memcheck \
		--leak-check=yes \
		--show-reachable=yes \
		--num-callers=20 \
		--track-fds=yes \
		./$(PROGS)

.PHONY: cppcheck
cppcheck:
	@echo "*** Static code analysis"
	@cppcheck $(shell find . -name "*.cpp" -o -name "*.h") \
		--verbose --enable=all -DDEBUG=1 \
		--suppress=missingIncludeSystem \
		--suppress=unknownMacro \
		--suppress=unusedFunction

.PHONY: indent
indent:
	@echo "*** Formatting code"
	@clang-format $(shell find . -name "*.cpp" -o -name "*.h") \
		-style=file -i -fallback-style=none

.PHONY: clean
clean:
	$(RM) $(PROGS) $(OBJS) $(DEPS)
