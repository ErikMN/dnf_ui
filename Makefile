CXX = g++
CXXFLAGS += -std=c++20 -Werror -MMD -MP -pipe -fdiagnostics-color=always
PROGS = dnf_ui
LDLIBS = -lm

PKGS = libdnf5 gtk4
TEST_PKGS = libdnf5 catch2-with-main

ifneq ($(filter test,$(MAKECMDGOALS)),)
  PKG_OK := $(shell pkg-config --exists $(TEST_PKGS) && echo yes)
  ifeq ($(PKG_OK),yes)
    PKG_LIBS := $(shell pkg-config --libs $(TEST_PKGS))
    PKG_CFLAGS := $(shell pkg-config --cflags $(TEST_PKGS))
  else
    $(error "Missing test dependencies: please install development packages for $(TEST_PKGS)")
  endif
else
  ifeq ($(filter dockerrun dockertest dockersetup cppcheck indent clean,$(MAKECMDGOALS)),)
    PKG_OK := $(shell pkg-config --exists $(PKGS) && echo yes)
    ifeq ($(PKG_OK),yes)
      PKG_LIBS := $(shell pkg-config --libs $(PKGS))
      PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
    else
      $(error "Missing dependencies: please install development packages for $(PKGS)")
    endif
  endif
endif

LDLIBS += $(PKG_LIBS)
CPPFLAGS += $(PKG_CFLAGS)

SRCS = $(wildcard src/*.cpp)
OBJS = $(SRCS:.cpp=.o)
DEPS = $(SRCS:.cpp=.d)

TEST_SRCS = \
    test/test_backend.cpp \
    test/test_search.cpp \
    src/base_manager.cpp \
    src/dnf_backend.cpp

TEST_OBJS = $(TEST_SRCS:.cpp=.o)
TEST_DEPS = $(TEST_SRCS:.cpp=.d)

CPPFLAGS += -Iinclude -Isrc

-include $(DEPS)
-include $(TEST_DEPS)

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

dnf_ui_tests: $(TEST_OBJS)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

.PHONY: test
test: dnf_ui_tests
	@echo "*** Running backend test suite ***"
	@./dnf_ui_tests

.PHONY: dockerrun
dockerrun:
	@./docker/docker_build.sh

.PHONY: dockertest
dockertest:
	@./docker/docker_test.sh

.PHONY: dockersetup
dockersetup:
	@./docker/docker_setup.sh

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
	@cppcheck $(shell find src -name "*.cpp" -o -name "*.hpp") \
		--quiet --enable=all -DDEBUG=1 \
		--suppress=missingIncludeSystem \
		--suppress=unknownMacro \
		--suppress=unusedFunction \
		--suppress=variableScope

.PHONY: indent
indent:
	@echo "*** Formatting code"
	@clang-format $(shell find src -name "*.cpp" -o -name "*.hpp") \
		-style=file -i -fallback-style=none

.PHONY: clean
clean:
	$(RM) $(PROGS) dnf_ui_tests $(OBJS) $(TEST_OBJS) $(DEPS) $(TEST_DEPS)
