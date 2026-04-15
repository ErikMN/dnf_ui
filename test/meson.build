# Fedora ships Catch2 v3 as the catch-devel package, which provides
# pkgconfig(catch2-with-main).
catch2_dep = dependency('catch2-with-main')
gio_dep = dependency('gio-2.0')

dnfui_tests = executable(
  'dnfui-tests',
  files(
    'test_backend.cpp',
    'test_search.cpp',
    'test_transaction_preview.cpp',
    'test_transaction_request.cpp',
  ) + backend_sources,
  include_directories: src_inc,
  dependencies: [
    libdnf5_dep,
    gio_dep,
    catch2_dep,
  ],
  cpp_args: ['-DDNFUI_BUILD_TESTS=1'],
)

test('dnfui-tests', dnfui_tests)
