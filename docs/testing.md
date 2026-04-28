# Testing

This document explains the test layout and what each group protects.

## Test Types

The project uses:

- Catch2 tests under [test](../test)
- shell smoke tests under [utils](../utils)
- Docker helpers under [docker](../docker)

The Catch2 tests are the fastest place to check backend and client behavior.
The shell smoke tests exercise the transaction service through D-Bus.

## Catch2 Tests

Key files:

- [test/test_backend.cpp](../test/test_backend.cpp)
- [test/test_search.cpp](../test/test_search.cpp)
- [test/test_transaction_preview.cpp](../test/test_transaction_preview.cpp)
- [test/test_transaction_request.cpp](../test/test_transaction_request.cpp)
- [test/test_transaction_service_client.cpp](../test/test_transaction_service_client.cpp)
- [test/test_offline.cpp](../test/test_offline.cpp)

These tests protect:

- package search and merge behavior
- installed snapshot behavior
- transaction preview behavior
- transaction request validation
- service client error handling
- offline and cached metadata behavior

## Service Smoke Tests

The service tests run the transaction service through D-Bus.

Important scripts:

- [utils/test_transaction_service_preview.sh](../utils/test_transaction_service_preview.sh)
- [utils/test_transaction_service_cancel.sh](../utils/test_transaction_service_cancel.sh)
- [utils/test_transaction_service_apply.sh](../utils/test_transaction_service_apply.sh)
- [utils/test_transaction_service_preview_failure.sh](../utils/test_transaction_service_preview_failure.sh)
- [utils/test_transaction_service_system_bus.sh](../utils/test_transaction_service_system_bus.sh)

These tests protect:

- preview success
- preview cancellation
- preview failure handling
- apply flow
- system bus authorization path
- disconnect cleanup

## Common Commands

Run the normal Docker Catch2 test set:

```sh
make dockertest
```

Run every Docker-backed test target:

```sh
make dockertests
```

Run the session bus service preview smoke test:

```sh
make dockerservicetest
```

Run the other session bus service smoke tests:

```sh
make dockerservicepreviewfailuretest
make dockerservicecanceltest
make dockerserviceapplytest
```

Run the system bus service smoke tests:

```sh
make dockerservicesystemtest
make dockerservicesystemdisconnecttest
make dockerservicesystemapplytest
```

The Makefile is a task runner. Meson owns build configuration and test
definitions.

## What To Test After Changes

For documentation-only changes, run `git diff --check`.

For comments inside C++ source files, also run a target that compiles the changed
files.

For backend query changes, run the Catch2 tests through `make dockertest`.

For transaction service changes, run `make dockertests` when possible. For a
focused check, choose the specific service target that matches the changed flow.
If the change touches system bus, Polkit, or client disconnect behavior, include
the matching system bus target.

For package apply behavior, use test packages that are safe to install and
remove in the test environment.
