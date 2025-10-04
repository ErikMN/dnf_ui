/**
 * https://en.cppreference.com/w/cpp/header
 */
#include <array>
#include <cassert>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <getopt.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#include <libdnf5/base/base.hpp>
#include <libdnf5/rpm/package_query.hpp>

using namespace std;

int
main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  cout << endl << "Hello World!" << endl;
  cout << __FILE__ << ":" << __LINE__ << endl;

#ifdef DEBUG_BUILD
  cout << endl << "*** Development build" << endl;
  cout << "Build: " << __DATE__ << " " << __TIME__ << endl;
#endif

  try {
    libdnf5::Base base;
    base.load_config();
    base.setup();

    // Load system and configured repositories
    auto repo_sack = base.get_repo_sack();
    repo_sack->create_repos_from_system_configuration();
    repo_sack->load_repos();

    // Query installed packages
    libdnf5::rpm::PackageQuery query(base);
    query.filter_installed();

    cout << endl << "Installed DNF packages:" << endl;
    for (auto pkg : query) {
      cout << "  " << pkg.get_name() << "-" << pkg.get_evr() << endl;
    }
  } catch (const std::exception &e) {
    cerr << "Error listing packages: " << e.what() << endl;
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
