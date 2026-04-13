MESON ?= meson
NPROC ?= $(shell nproc)

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

.DEFAULT_GOAL := all

.PHONY: all
all: dnf_ui dnf_ui_transaction_service

.PHONY: _meson_setup
_meson_setup:
	if [ -d "$(MESON_BUILD_DIR)" ]; then \
		$(MESON) setup "$(MESON_BUILD_DIR)" --reconfigure $(MESON_SETUP_ARGS); \
	else \
		$(MESON) setup "$(MESON_BUILD_DIR)" $(MESON_SETUP_ARGS); \
	fi

.PHONY: debug
debug:
	@echo "*** Meson build directory: $(MESON_BUILD_DIR)"
	@echo "*** Build type: $(MESON_BUILD_TYPE)"
	@echo "*** Final build: $(MESON_FINAL_BUILD)"
	@echo "*** Address sanitizer: $(MESON_SANITIZE)"
	@echo "*** Debug trace: $(MESON_DEBUG_TRACE)"
	@echo "*** Build tests: $(MESON_BUILD_TESTS)"

.PHONY: dnf_ui
dnf_ui: _meson_setup
	$(MESON) compile -C "$(MESON_BUILD_DIR)" dnf_ui
	ln -sfn "$(APP_BUILD_PATH)" "$(APP_BIN_NAME)"

.PHONY: dnf_ui_transaction_service
dnf_ui_transaction_service: _meson_setup
	$(MESON) compile -C "$(MESON_BUILD_DIR)" dnf_ui_transaction_service
	ln -sfn "$(SERVICE_BUILD_PATH)" "$(TRANSACTION_SERVICE_BIN_NAME)"

.PHONY: dnf_ui_tests
dnf_ui_tests: _meson_setup
	$(MESON) compile -C "$(MESON_BUILD_DIR)" dnf_ui_tests
	ln -sfn "$(TEST_BUILD_PATH)" "$(TEST_BIN_NAME)"

.PHONY: run
run: dnf_ui
	@./$(APP_BIN_NAME)

.PHONY: test
test: dnf_ui_tests
	@echo "*** Running backend test suite ***"
	@./$(TEST_BIN_NAME)

.PHONY: install
install: all
	@if [ -z "$$DESTDIR" ] && [ "$$(id -u)" -ne 0 ]; then \
		echo "*** install must run as root unless DESTDIR is set ***" >&2; \
		exit 1; \
	fi
	$(MESON) install -C "$(MESON_BUILD_DIR)" --only-changed
	@if [ -z "$$DESTDIR" ]; then \
		systemctl daemon-reload; \
		systemctl reset-failed "$(TRANSACTION_SERVICE_SYSTEMD_UNIT_NAME)" >/dev/null 2>&1 || true; \
		gdbus call --system --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.ReloadConfig >/dev/null; \
	fi

.PHONY: uninstall
uninstall:
	@test "$$(id -u)" -eq 0 || { echo "*** uninstall must run as root ***" >&2; exit 1; }
	-systemctl stop "$(TRANSACTION_SERVICE_SYSTEMD_UNIT_NAME)" >/dev/null 2>&1 || true
	rm -f "$(APP_BIN_DEST)" \
	      "$(TRANSACTION_SERVICE_BIN_DEST)" \
	      "$(TRANSACTION_SERVICE_POLICY_DEST)" \
	      "$(TRANSACTION_SERVICE_DBUS_SERVICE_DEST)" \
	      "$(TRANSACTION_SERVICE_DBUS_POLICY_DEST)" \
	      "$(TRANSACTION_SERVICE_SYSTEMD_UNIT_DEST)"
	systemctl daemon-reload
	-systemctl reset-failed "$(TRANSACTION_SERVICE_SYSTEMD_UNIT_NAME)" >/dev/null 2>&1 || true
	gdbus call --system --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.ReloadConfig >/dev/null
	@echo "*** Removed installed DNF UI runtime files. ***"

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
	-systemctl stop "$(TRANSACTION_SERVICE_SYSTEMD_UNIT_NAME)" >/dev/null 2>&1 || true
	$(MESON) install -C "$(MESON_BUILD_DIR)" --only-changed --tags transaction-service
	systemctl daemon-reload
	-systemctl reset-failed "$(TRANSACTION_SERVICE_SYSTEMD_UNIT_NAME)" >/dev/null 2>&1 || true
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
	-systemctl reset-failed "$(TRANSACTION_SERVICE_SYSTEMD_UNIT_NAME)" >/dev/null 2>&1 || true
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
valgrind: dnf_ui
	@valgrind \
		--tool=memcheck \
		--leak-check=yes \
		--show-reachable=yes \
		--num-callers=20 \
		--track-fds=yes \
		./$(APP_BIN_NAME)

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
	rm -rf "$(MESON_BUILD_ROOT)"
	rm -f "$(APP_BIN_NAME)" "$(TRANSACTION_SERVICE_BIN_NAME)" "$(TEST_BIN_NAME)"
