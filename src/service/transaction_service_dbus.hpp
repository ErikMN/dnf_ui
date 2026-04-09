// -----------------------------------------------------------------------------
// src/service/transaction_service_dbus.hpp
// Shared D-Bus contract names for the transaction service
// These constants are used by both the GUI client and the service. Keep them
// stable once the service is installed and packaged.
// -----------------------------------------------------------------------------
#pragma once

// -----------------------------------------------------------------------------
// Transaction service D-Bus names shared by the GUI client and service
// -----------------------------------------------------------------------------
// Well-known bus name owned by the transaction service.
inline constexpr const char *kTransactionServiceName = "com.fedora.Dnfui.Transaction1";
// Object path for the manager object that creates new transaction requests.
inline constexpr const char *kTransactionServiceManagerPath = "/com/fedora/Dnfui/Transaction1";
// Interface implemented by the manager object.
inline constexpr const char *kTransactionServiceManagerInterface = "com.fedora.Dnfui.Transaction1";
// Interface implemented by each per-request transaction object.
inline constexpr const char *kTransactionServiceRequestInterface = "com.fedora.Dnfui.TransactionRequest1";
