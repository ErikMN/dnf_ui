CXX = g++
CXXFLAGS += -std=c++20 -Werror -MMD -MP -pipe -fdiagnostics-color=always
PROGS = dnf_ui
LDLIBS = -lm

PKGS = libdnf5 gtk4

ifeq ($(filter clean,$(MAKECMDGOALS)),)
  PKG_OK := $(shell pkg-config --exists $(PKGS) && echo yes)
  ifeq ($(PKG_OK),yes)
    PKG_LIBS := $(shell pkg-config --libs $(PKGS))
    PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
    LDLIBS += $(PKG_LIBS)
    CPPFLAGS += $(PKG_CFLAGS)
  else
    $(error "Missing dependencies: please install development packages for $(PKGS)")
  endif
endif

SRCS = $(wildcard src/*.cpp)
OBJS = $(SRCS:.cpp=.o)
DEPS = $(SRCS:.cpp=.d)

CPPFLAGS += -Iinclude -Isrc

-include $(DEPS)

# FINAL=y
ifeq ($(FINAL), y)
  LDFLAGS += -s
  CXXFLAGS += -DNDEBUG_BUILD -g0 -O2
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
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

.PHONY: run
run: $(PROGS)
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
