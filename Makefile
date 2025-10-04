CC = g++
CXXFLAGS += -std=c++20
PROGS = dnf_ui
LDLIBS = -lm

PKGS += libdnf5 gtk4
ifdef PKGS
  LDLIBS += $(shell pkg-config --libs $(PKGS))
  CPPFLAGS += $(shell pkg-config --cflags $(PKGS))
endif

SRCS = $(wildcard *.cpp)
OBJS = $(SRCS:.cpp=.o)
DEPS = $(SRCS:.cpp=.d)
CPPFLAGS += -Werror -MMD -MP

-include $(DEPS)

# FINAL=y
ifeq ($(FINAL), y)
  LDFLAGS += -s
  CFLAGS += -DNDEBUG -g0 -O2
  CPPFLAGS += -Wpedantic -Wextra -Wmaybe-uninitialized
  CPPFLAGS += -W -Wformat=2 -Wpointer-arith -Winline
  CPPFLAGS += -Wdisabled-optimization -Wfloat-equal -Wall
else
  # ASAN=y
  CPPFLAGS += -g3 -DDEBUG_BUILD
  ifeq ($(ASAN), y)
    CPPFLAGS += -fsanitize=address -O1 -fno-omit-frame-pointer
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
	@echo "Compiler-flags:" $(CPPFLAGS)
	@echo "Linker-flags:" $(LDFLAGS)
	@echo "Linker-libs:" $(LDLIBS)

$(PROGS): $(OBJS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

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
