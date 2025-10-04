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

using namespace std;

int
main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  cout << endl << "Hello World!" << endl;
  cout << __FILE__ << ":" << __LINE__ << endl;

#ifdef DEBUG
  cout << endl << "*** Development build" << endl;
  cout << "Build: " << __DATE__ << " " << __TIME__ << endl;
#endif

  return EXIT_SUCCESS;
}
