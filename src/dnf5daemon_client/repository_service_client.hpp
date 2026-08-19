// -----------------------------------------------------------------------------
// src/dnf5daemon_client/repository_service_client.hpp
// GUI client helpers for dnf5daemon repository configuration
// Exposes a small plain-value API for listing and changing configured repositories.
// -----------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

typedef struct _GCancellable GCancellable;

#ifdef DNFUI_BUILD_TESTS
typedef struct _GVariant GVariant;
#endif

struct RepositoryInfo {
  std::string id;
  std::string name;
  bool enabled = false;
};

struct RepositoryWriteResult {
  bool enable_attempted = false;
  bool enable_succeeded = true;
  bool disable_attempted = false;
  bool disable_succeeded = true;
  std::string error;
};

// -----------------------------------------------------------------------------
// List configured repositories through dnf5daemon.
// -----------------------------------------------------------------------------
bool repository_service_client_list(std::vector<RepositoryInfo> &repositories_out,
                                    std::string &error_out,
                                    GCancellable *cancellable = nullptr);

// -----------------------------------------------------------------------------
// Apply repository enable and disable requests through dnf5daemon.
// -----------------------------------------------------------------------------
RepositoryWriteResult repository_service_client_apply_changes(const std::vector<std::string> &enable_ids,
                                                              const std::vector<std::string> &disable_ids);

#ifdef DNFUI_BUILD_TESTS
// -----------------------------------------------------------------------------
// Feed daemon repository-list data through the strict parser for tests.
// -----------------------------------------------------------------------------
bool repository_service_client_testonly_parse_repository_list(GVariant *repositories,
                                                              std::vector<RepositoryInfo> &repositories_out,
                                                              std::string &error_out);
#endif

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
