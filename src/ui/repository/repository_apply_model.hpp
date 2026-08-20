// -----------------------------------------------------------------------------
// src/ui/repository/repository_apply_model.hpp
// Repository apply planning helpers
// -----------------------------------------------------------------------------
#pragma once

#include "dnf5daemon_client/repository_service_client.hpp"
#include "dnf_backend/base_manager.hpp"

#include <string>
#include <vector>

struct RepositoryChangePlan {
  std::vector<std::string> enable_ids;
  std::vector<std::string> disable_ids;
  std::string error;
  bool valid = true;
};

enum class RepositoryBackendSyncResult {
  LIVE_METADATA,
  CACHED_METADATA,
  INSTALLED_ONLY,
  FAILED,
};

// -----------------------------------------------------------------------------
// Compare requested repository states with a fresh daemon list before writing.
// -----------------------------------------------------------------------------
RepositoryChangePlan repository_apply_plan_changes(const std::vector<RepositoryInfo> &repositories,
                                                   const std::vector<std::string> &requested_enable_ids,
                                                   const std::vector<std::string> &requested_disable_ids);

// -----------------------------------------------------------------------------
// Return true when the final daemon list confirms all requested repository states.
// -----------------------------------------------------------------------------
bool repository_apply_requested_states_match(const std::vector<RepositoryInfo> &repositories,
                                             const std::vector<std::string> &repo_ids,
                                             bool enabled);

// -----------------------------------------------------------------------------
// Return true when the final daemon list confirms all requested repository states.
// -----------------------------------------------------------------------------
bool repository_apply_requested_states_match(const std::vector<RepositoryInfo> &repositories,
                                             const std::vector<std::string> &enable_ids,
                                             const std::vector<std::string> &disable_ids);

// -----------------------------------------------------------------------------
// Convert the BaseManager rebuild result into the repository Apply result model.
// -----------------------------------------------------------------------------
RepositoryBackendSyncResult repository_apply_backend_sync_result(bool backend_rebuilt, BaseRepoState repo_state);

// -----------------------------------------------------------------------------
// EOF
// -----------------------------------------------------------------------------
