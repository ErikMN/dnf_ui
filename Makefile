MESON ?= meson

include utils/transaction_service_paths.conf

APP_BIN_NAME = dnf_ui
APP_BIN_DEST = /usr/bin/dnf_ui
TEST_BIN_NAME = dnf_ui_tests

ifeq ($(FINAL),y)
  MESON_BUILD_NAME = final
  MESON_BUILD_TYPE = release
  MESON_FINAL_BUILD = true
  MESON_WARNING_LEVEL = 3
else
  MESON_BUILD_NAME = debug
  MESON_BUILD_TYPE = debug
  MESON_FINAL_BUILD = false
  MESON_WARNING_LEVEL = 0
endif

ifeq ($(ASAN),y)
  MESON_SANITIZE = address
else
  MESON_SANITIZE = none
endif

ifeq ($(DEBUG_TRACE),y)
  MESON_DEBUG_TRACE = true
else
  MESON_DEBUG_TRACE = false
endif

ifneq ($(filter test $(TEST_BIN_NAME),$(MAKECMDGOALS)),)
  MESON_BUILD_TESTS = true
else
  MESON_BUILD_TESTS = false
endif

MESON_BUILD_ROOT = build
MESON_BUILD_DIR = $(MESON_BUILD_ROOT)/$(MESON_BUILD_NAME)
MESON_SETUP_ARGS = \
	--prefix /usr \
	--libexecdir libexec \
	--buildtype $(MESON_BUILD_TYPE) \
	-Dwarning_level=$(MESON_WARNING_LEVEL) \
	-Dbuild_tests=$(MESON_BUILD_TESTS) \
	-Ddebug_trace=$(MESON_DEBUG_TRACE) \
	-Dfinal_build=$(MESON_FINAL_BUILD) \
	-Db_sanitize=$(MESON_SANITIZE)

APP_BUILD_PATH = $(CURDIR)/$(MESON_BUILD_DIR)/src/$(APP_BIN_NAME)
SERVICE_BUILD_PATH = $(CURDIR)/$(MESON_BUILD_DIR)/src/service/$(TRANSACTION_SERVICE_BIN_NAME)
TEST_BUILD_PATH = $(CURDIR)/$(MESON_BUILD_DIR)/test/$(TEST_BIN_NAME)

SERVICE_INSTALL_FILES = \
	"$(TRANSACTION_SERVICE_BIN_DEST)" \
	"$(TRANSACTION_SERVICE_POLICY_DEST)" \
	"$(TRANSACTION_SERVICE_DBUS_SERVICE_DEST)" \
	"$(TRANSACTION_SERVICE_DBUS_POLICY_DEST)" \
	"$(TRANSACTION_SERVICE_SYSTEMD_UNIT_DEST)"

APP_INSTALL_FILES = \
	"$(APP_BIN_DEST)" \
	$(SERVICE_INSTALL_FILES)

# Require root for targets that change the live system:
define require_root
	@test "$$(id -u)" -eq 0 || { echo "*** $(1) must run as root ***" >&2; exit 1; }
endef

# Require a built artifact before install with no rebuild:
define require_built_file
	@test -e "$(1)" || { echo "*** Build target missing: $(1). Build as normal user first. ***" >&2; exit 1; }
endef

# Stop the transaction service if it is currently running:
define stop_transaction_service
	systemctl stop "$(TRANSACTION_SERVICE_SYSTEMD_UNIT_NAME)" >/dev/null 2>&1 || true
endef

# Reload systemd and D-Bus state after service file changes:
define refresh_transaction_service_state
	systemctl daemon-reload
	systemctl reset-failed "$(TRANSACTION_SERVICE_SYSTEMD_UNIT_NAME)" >/dev/null 2>&1 || true
	gdbus call --system --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.ReloadConfig >/dev/null
endef

.DEFAULT_GOAL := all

# Build the app and transaction service:
.PHONY: all
all: dnf_ui dnf_ui_transaction_service

# Configure the active Meson build directory:
$(MESON_BUILD_DIR)/build.ninja:
	if [ -d "$(MESON_BUILD_DIR)" ]; then \
		$(MESON) setup "$(MESON_BUILD_DIR)" --reconfigure $(MESON_SETUP_ARGS); \
	else \
		$(MESON) setup "$(MESON_BUILD_DIR)" $(MESON_SETUP_ARGS); \
	fi

# Build the app binary:
.PHONY: dnf_ui
dnf_ui: $(MESON_BUILD_DIR)/build.ninja
	$(MESON) compile -C "$(MESON_BUILD_DIR)" dnf_ui
	ln -sfn "$(APP_BUILD_PATH)" "$(APP_BIN_NAME)"

# Build the transaction service binary:
.PHONY: dnf_ui_transaction_service
dnf_ui_transaction_service: $(MESON_BUILD_DIR)/build.ninja
	$(MESON) compile -C "$(MESON_BUILD_DIR)" dnf_ui_transaction_service
	ln -sfn "$(SERVICE_BUILD_PATH)" "$(TRANSACTION_SERVICE_BIN_NAME)"

# Build the backend test binary:
.PHONY: dnf_ui_tests
dnf_ui_tests: $(MESON_BUILD_DIR)/build.ninja
	$(MESON) compile -C "$(MESON_BUILD_DIR)" dnf_ui_tests
	ln -sfn "$(TEST_BUILD_PATH)" "$(TEST_BIN_NAME)"

# Run the app from the current build:
.PHONY: run
run: dnf_ui
	@./$(APP_BIN_NAME)

# Run the backend test suite:
.PHONY: test
test: dnf_ui_tests
	@echo "*** Running backend test suite ***"
	@./$(TEST_BIN_NAME)

# Install the app and service files from the current build:
.PHONY: install
install:
	@if [ -z "$$DESTDIR" ] && [ "$$(id -u)" -ne 0 ]; then \
		echo "*** install must run as root unless DESTDIR is set ***" >&2; \
		exit 1; \
	fi
	$(call require_built_file,$(APP_BUILD_PATH))
	$(call require_built_file,$(SERVICE_BUILD_PATH))
	@if [ -z "$$DESTDIR" ]; then \
		$(call stop_transaction_service) \
	fi
	$(MESON) install -C "$(MESON_BUILD_DIR)" --no-rebuild --only-changed
	@if [ -z "$$DESTDIR" ]; then \
		systemctl daemon-reload; \
		systemctl reset-failed "$(TRANSACTION_SERVICE_SYSTEMD_UNIT_NAME)" >/dev/null 2>&1 || true; \
		gdbus call --system --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.ReloadConfig >/dev/null; \
	fi

# Remove the installed app and service files:
.PHONY: uninstall
uninstall:
	$(call require_root,uninstall)
	$(call stop_transaction_service)
	rm -f $(APP_INSTALL_FILES)
	$(call refresh_transaction_service_state)
	@echo "*** Removed installed DNF UI runtime files. ***"

# Run the session bus preview smoke test:
.PHONY: servicetest
servicetest: dnf_ui_transaction_service
	@./utils/test_transaction_service_preview.sh

# Run the session bus cancel smoke test:
.PHONY: servicecanceltest
servicecanceltest: dnf_ui_transaction_service
	@./utils/test_transaction_service_cancel.sh

# Run the session bus apply smoke test:
.PHONY: serviceapplytest
serviceapplytest: dnf_ui_transaction_service
	@./utils/test_transaction_service_apply.sh

# Install the native transaction service files for development:
.PHONY: serviceinstall
serviceinstall:
	$(call require_root,serviceinstall)
	$(call require_built_file,$(SERVICE_BUILD_PATH))
	$(call stop_transaction_service)
	$(MESON) install -C "$(MESON_BUILD_DIR)" --no-rebuild --only-changed --tags transaction-service
	$(call refresh_transaction_service_state)
	@echo "*** Installed $(TRANSACTION_SERVICE_NAME) service files for native testing. ***"
	@echo "*** Run dnf_ui as a regular desktop user and apply a transaction to trigger the Polkit prompt. ***"

# Remove the native transaction service files:
.PHONY: serviceuninstall
serviceuninstall:
	$(call require_root,serviceuninstall)
	$(call stop_transaction_service)
	rm -f $(SERVICE_INSTALL_FILES)
	$(call refresh_transaction_service_state)
	@echo "*** Removed native transaction service files. ***"

# Run the native system bus preview smoke test:
.PHONY: servicesystemtest
servicesystemtest:
	@./utils/test_transaction_service_system_bus.sh

# Run the native system bus apply smoke test:
.PHONY: servicesystemapplytest
servicesystemapplytest:
	@SERVICE_SYSTEM_APPLY=yes ./utils/test_transaction_service_system_bus.sh

# Run the native system bus disconnect smoke test:
.PHONY: servicesystemdisconnecttest
servicesystemdisconnecttest:
	@SERVICE_SYSTEM_DISCONNECT=yes ./utils/test_transaction_service_system_bus.sh

# Run the app in Docker:
.PHONY: dockerrun
dockerrun:
	@THEME="$(THEME)" DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_build.sh

# Run the backend test suite in Docker:
.PHONY: dockertest
dockertest:
	@./docker/docker_test.sh

# Run the session bus preview smoke test in Docker:
.PHONY: dockerservicetest
dockerservicetest:
	@DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_service_test.sh

# Run the session bus apply smoke test in Docker:
.PHONY: dockerserviceapplytest
dockerserviceapplytest:
	@DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_service_apply_test.sh

# Run the session bus cancel smoke test in Docker:
.PHONY: dockerservicecanceltest
dockerservicecanceltest:
	@DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_service_cancel_test.sh

# Run the system bus preview smoke test in Docker:
.PHONY: dockerservicesystemtest
dockerservicesystemtest:
	@DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_service_system_bus_test.sh

# Run the system bus apply smoke test in Docker:
.PHONY: dockerservicesystemapplytest
dockerservicesystemapplytest:
	@DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_service_system_bus_apply_test.sh

# Run the system bus disconnect smoke test in Docker:
.PHONY: dockerservicesystemdisconnecttest
dockerservicesystemdisconnecttest:
	@DEBUG_TRACE="$(DEBUG_TRACE)" ./docker/docker_service_system_bus_disconnect_test.sh

# Build the Docker development image:
.PHONY: dockersetup
dockersetup:
	@./docker/docker_setup.sh

# Run valgrind on the app binary:
.PHONY: valgrind
valgrind: dnf_ui
	@valgrind \
		--tool=memcheck \
		--leak-check=yes \
		--show-reachable=yes \
		--num-callers=20 \
		--track-fds=yes \
		./$(APP_BIN_NAME)

# Run cppcheck on the source tree:
.PHONY: cppcheck
cppcheck:
	@echo "*** Static code analysis"
	@cppcheck $(shell find src -name "*.cpp" -o -name "*.hpp") \
		--quiet --enable=all -DDEBUG=1 \
		--suppress=missingIncludeSystem \
		--suppress=unusedStructMember \
		--suppress=knownConditionTrueFalse

# Run clang format through the Docker helper:
.PHONY: indent
indent:
	@echo "*** Formatting code"
	@./utils/docker-clang-format.sh

# Remove generated build output and symlinks:
.PHONY: clean
clean:
	rm -rf "$(MESON_BUILD_ROOT)"
	rm -f "$(APP_BIN_NAME)" "$(TRANSACTION_SERVICE_BIN_NAME)" "$(TEST_BIN_NAME)"
