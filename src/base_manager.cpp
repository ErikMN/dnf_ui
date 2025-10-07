// -----------------------------------------------------------------------------
// BaseManager: Provides cached access to a libdnf5::Base instance
// - Ensures thread-safe creation and reuse of libdnf5 Base objects
// - Refreshes automatically after a fixed timeout (10 minutes)
// - Supports manual rebuilds when repositories are refreshed
// -----------------------------------------------------------------------------
#include "base_manager.hpp"

#include <libdnf5/conf/option_bool.hpp>
#include <libdnf5/rpm/package_query.hpp>

// -----------------------------------------------------------------------------
// Singleton: BaseManager instance accessor
// -----------------------------------------------------------------------------
BaseManager &
BaseManager::instance()
{
  static BaseManager mgr;
  return mgr;
}

// -----------------------------------------------------------------------------
// Retrieve cached libdnf5::Base instance (per-thread)
// Initializes if missing or older than 10 minutes
// -----------------------------------------------------------------------------
libdnf5::Base &
BaseManager::get_base()
{
  // Thread-local shared_ptr so each thread uses its own copy safely
  thread_local std::shared_ptr<libdnf5::Base> thread_base;

  // Check cache age refresh every 10 minutes
  const auto now = std::chrono::steady_clock::now();
  if (!thread_base || (now - last_refresh) > std::chrono::minutes(10)) {
    // Only one thread rebuilds the global base at a time
    std::scoped_lock lock(rebuild_mutex);

    if (!global_base || (now - last_refresh) > std::chrono::minutes(10)) {
      // Create and fully initialize a new libdnf5::Base
      auto base = std::make_shared<libdnf5::Base>();
      base->load_config();
      base->setup();

      auto repo_sack = base->get_repo_sack();
      repo_sack->create_repos_from_system_configuration();
      repo_sack->load_repos();

      global_base = base;
      last_refresh = now;
    }
    // Reuse the shared copy for this thread
    // Store shared global base reference in this thread's local cache
    thread_base = global_base;
  }

  return *thread_base;
}

// -----------------------------------------------------------------------------
// Force rebuild of cached Base (used when user requests "Refresh Repositories")
// -----------------------------------------------------------------------------
void
BaseManager::rebuild()
{
  // Lock to ensure only one rebuild runs at a time
  std::scoped_lock lock(rebuild_mutex);

  // Clear global cached Base instance to force fresh creation
  global_base.reset();

  // Build a new Base and reload all repository data
  auto base = std::make_shared<libdnf5::Base>();
  base->load_config();
  base->setup();

  auto repo_sack = base->get_repo_sack();
  repo_sack->create_repos_from_system_configuration();
  repo_sack->load_repos();

  global_base = base;

  // Update timestamp to mark last successful rebuild
  last_refresh = std::chrono::steady_clock::now();
}
