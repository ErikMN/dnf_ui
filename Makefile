CXX = g++
CXXFLAGS += -std=c++20 -Werror -MMD -MP -pipe -fdiagnostics-color=always
PROGS = dnf_ui dnf_ui_transaction_service
LDLIBS = -lm

PKGS = libdnf5 gtk4 polkit-gobject-1
TEST_PKGS = libdnf5 gio-2.0 catch2-with-main

include utils/transaction_service_paths.conf

ifneq ($(filter test,$(MAKECMDGOALS)),)
  PKG_OK := $(shell pkg-config --exists $(TEST_PKGS) && echo yes)
  ifeq ($(PKG_OK),yes)
    PKG_LIBS := $(shell pkg-config --libs $(TEST_PKGS))
    PKG_CFLAGS := $(shell pkg-config --cflags $(TEST_PKGS))
  else
    $(error "Missing test dependencies: please install development packages for $(TEST_PKGS)")
  endif
else
  ifeq ($(filter dockerrun dockertest dockerservicetest dockerserviceapplytest dockerservicecanceltest dockerservicesystemtest dockerservicesystemapplytest dockerservicesystemdisconnecttest dockersetup cppcheck indent clean serviceuninstall servicesystemtest servicesystemapplytest servicesystemdisconnecttest,$(MAKECMDGOALS)),)
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

APP_SRCS = $(wildcard src/*.cpp)
APP_OBJS = $(APP_SRCS:.cpp=.o)
APP_DEPS = $(APP_SRCS:.cpp=.d)

SERVICE_BACKEND_SRCS = \
	src/base_manager.cpp \
	src/dnf_backend.cpp
SERVICE_BACKEND_OBJS = $(SERVICE_BACKEND_SRCS:.cpp=.o)
SERVICE_BACKEND_DEPS = $(SERVICE_BACKEND_SRCS:.cpp=.d)

SERVICE_SRCS = $(wildcard src/service/*.cpp)
SERVICE_OBJS = $(SERVICE_SRCS:.cpp=.o)
SERVICE_DEPS = $(SERVICE_SRCS:.cpp=.d)

TEST_SRCS = \
    test/test_backend.cpp \
    test/test_search.cpp \
    test/test_transaction_preview.cpp \
    test/test_transaction_request.cpp \
    src/base_manager.cpp \
    src/dnf_backend.cpp

TEST_OBJS = $(TEST_SRCS:.cpp=.o)
TEST_DEPS = $(TEST_SRCS:.cpp=.d)

CPPFLAGS += -Iinclude -Isrc

TRANSACTION_SERVICE_BIN_SRC = $(CURDIR)/$(TRANSACTION_SERVICE_BIN_NAME)
TRANSACTION_SERVICE_POLICY_SRC = $(CURDIR)/$(TRANSACTION_SERVICE_POLICY_FILE)
TRANSACTION_SERVICE_DBUS_SERVICE_SRC = $(CURDIR)/$(TRANSACTION_SERVICE_DBUS_SERVICE_FILE)
TRANSACTION_SERVICE_DBUS_POLICY_SRC = $(CURDIR)/$(TRANSACTION_SERVICE_DBUS_POLICY_FILE)
TRANSACTION_SERVICE_SYSTEMD_UNIT_SRC = $(CURDIR)/$(TRANSACTION_SERVICE_SYSTEMD_UNIT_FILE)

-include $(APP_DEPS)
-include $(SERVICE_BACKEND_DEPS)
-include $(SERVICE_DEPS)
-include $(TEST_DEPS)

# FINAL=y
ifeq ($(FINAL), y)
  LDFLAGS += -s
  CXXFLAGS += -DNDEBUG_BUILD -g0 -O2
  CXXFLAGS += -Wpedantic -Wextra -Wmaybe-uninitialized
  CXXFLAGS += -W -Wformat=2 -Wpointer-arith
  CXXFLAGS += -Wdisabled-optimization -Wfloat-equal -Wall
else
  # ASAN=y
  CXXFLAGS += -g3 -DDEBUG_BUILD
  ifeq ($(ASAN), y)
    CXXFLAGS += -fsanitize=address -O1 -fno-omit-frame-pointer
    LDLIBS += -fsanitize=address
  endif
endif

# DEBUG_TRACE=y
ifeq ($(DEBUG_TRACE), y)
  CXXFLAGS += -DDNF_UI_DEBUG_TRACE
endif

.PHONY: all
all: $(PROGS)

.PHONY: debug
debug:
	@echo "*** Debug info:"
	@echo "App source-files:" $(APP_SRCS)
	@echo "App object-files:" $(APP_OBJS)
	@echo "Service backend source-files:" $(SERVICE_BACKEND_SRCS)
	@echo "Service backend object-files:" $(SERVICE_BACKEND_OBJS)
	@echo "Service source-files:" $(SERVICE_SRCS)
	@echo "Service object-files:" $(SERVICE_OBJS)
	@echo "Compiler-flags:" $(CXXFLAGS)
	@echo "Linker-flags:" $(LDFLAGS)
	@echo "Linker-libs:" $(LDLIBS)

dnf_ui: $(APP_OBJS)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

dnf_ui_transaction_service: $(SERVICE_BACKEND_OBJS) $(SERVICE_OBJS)
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

# Native transaction service development helpers:
.PHONY: servicetest
servicetest: dnf_ui_transaction_service
	@./utils/test_transaction_service_preview.sh

.PHONY: servicecanceltest
servicecanceltest: dnf_ui_transaction_service
	@./utils/test_transaction_service_cancel.sh

.PHONY: serviceapplytest
serviceapplytest: dnf_ui_transaction_service
	@./utils/test_transaction_service_apply.sh

.PHONY: serviceinstall
serviceinstall: dnf_ui_transaction_service
	@test "$$(id -u)" -eq 0 || { echo "*** serviceinstall must run as root ***" >&2; exit 1; }
	@test -x "$(TRANSACTION_SERVICE_BIN_SRC)" || { echo "*** Missing service binary: $(TRANSACTION_SERVICE_BIN_SRC) ***" >&2; echo "*** Build it first with: make dnf_ui_transaction_service ***" >&2; exit 1; }
	@test -f "$(TRANSACTION_SERVICE_POLICY_SRC)" || { echo "*** Missing packaging file: $(TRANSACTION_SERVICE_POLICY_SRC) ***" >&2; exit 1; }
	@test -f "$(TRANSACTION_SERVICE_DBUS_SERVICE_SRC)" || { echo "*** Missing packaging file: $(TRANSACTION_SERVICE_DBUS_SERVICE_SRC) ***" >&2; exit 1; }
	@test -f "$(TRANSACTION_SERVICE_DBUS_POLICY_SRC)" || { echo "*** Missing packaging file: $(TRANSACTION_SERVICE_DBUS_POLICY_SRC) ***" >&2; exit 1; }
	@test -f "$(TRANSACTION_SERVICE_SYSTEMD_UNIT_SRC)" || { echo "*** Missing packaging file: $(TRANSACTION_SERVICE_SYSTEMD_UNIT_SRC) ***" >&2; exit 1; }
	install -D -m 0755 "$(TRANSACTION_SERVICE_BIN_SRC)" "$(TRANSACTION_SERVICE_BIN_DEST)"
	install -D -m 0644 "$(TRANSACTION_SERVICE_POLICY_SRC)" "$(TRANSACTION_SERVICE_POLICY_DEST)"
	install -D -m 0644 "$(TRANSACTION_SERVICE_DBUS_SERVICE_SRC)" "$(TRANSACTION_SERVICE_DBUS_SERVICE_DEST)"
	install -D -m 0644 "$(TRANSACTION_SERVICE_DBUS_POLICY_SRC)" "$(TRANSACTION_SERVICE_DBUS_POLICY_DEST)"
	install -D -m 0644 "$(TRANSACTION_SERVICE_SYSTEMD_UNIT_SRC)" "$(TRANSACTION_SERVICE_SYSTEMD_UNIT_DEST)"
	systemctl daemon-reload
	gdbus call --system --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.ReloadConfig >/dev/null
	@echo "*** Installed $(TRANSACTION_SERVICE_NAME) service files for native testing. ***"
	@echo "*** Run dnf_ui as a regular desktop user and apply a transaction to trigger the Polkit prompt. ***"

.PHONY: serviceuninstall
serviceuninstall:
	@test "$$(id -u)" -eq 0 || { echo "*** serviceuninstall must run as root ***" >&2; exit 1; }
	-systemctl stop "$(TRANSACTION_SERVICE_SYSTEMD_UNIT_NAME)" >/dev/null 2>&1 || true
	rm -f "$(TRANSACTION_SERVICE_BIN_DEST)" \
	      "$(TRANSACTION_SERVICE_POLICY_DEST)" \
	      "$(TRANSACTION_SERVICE_DBUS_SERVICE_DEST)" \
	      "$(TRANSACTION_SERVICE_DBUS_POLICY_DEST)" \
	      "$(TRANSACTION_SERVICE_SYSTEMD_UNIT_DEST)"
	systemctl daemon-reload
	gdbus call --system --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.ReloadConfig >/dev/null
	@echo "*** Removed native transaction service files. ***"

.PHONY: servicesystemtest
servicesystemtest:
	@./utils/test_transaction_service_system_bus.sh

.PHONY: servicesystemapplytest
servicesystemapplytest:
	@SERVICE_SYSTEM_APPLY=yes ./utils/test_transaction_service_system_bus.sh

.PHONY: servicesystemdisconnecttest
servicesystemdisconnecttest:
	@SERVICE_SYSTEM_DISCONNECT=yes ./utils/test_transaction_service_system_bus.sh

# Docker app and transaction service helpers:
# To test dark or light themes in Docker:
# make dockerrun THEME=dark
# make dockerrun THEME=light
.PHONY: dockerrun
dockerrun:
	@THEME="$(THEME)" DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_build.sh

.PHONY: dockertest
dockertest:
	@./docker/docker_test.sh

.PHONY: dockerservicetest
dockerservicetest:
	@DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_service_test.sh

.PHONY: dockerserviceapplytest
dockerserviceapplytest:
	@DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_service_apply_test.sh

.PHONY: dockerservicecanceltest
dockerservicecanceltest:
	@DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_service_cancel_test.sh

.PHONY: dockerservicesystemtest
dockerservicesystemtest:
	@DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_service_system_bus_test.sh

.PHONY: dockerservicesystemapplytest
dockerservicesystemapplytest:
	@DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_service_system_bus_apply_test.sh

.PHONY: dockerservicesystemdisconnecttest
dockerservicesystemdisconnecttest:
	@DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_service_system_bus_disconnect_test.sh

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
		--suppress=unusedStructMember \
		--suppress=knownConditionTrueFalse

.PHONY: indent
indent:
	@echo "*** Formatting code"
	@./utils/docker-clang-format.sh

.PHONY: clean
clean:
	$(RM) $(PROGS) dnf_ui_tests $(APP_OBJS) $(SERVICE_BACKEND_OBJS) $(SERVICE_OBJS) $(TEST_OBJS) $(APP_DEPS) $(SERVICE_BACKEND_DEPS) $(SERVICE_DEPS) $(TEST_DEPS)
