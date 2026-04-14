# Fedora ships Catch2 v3 as the catch-devel package, which provides
# pkgconfig(catch2-with-main).
catch2_dep = dependency('catch2-with-main')
gio_dep = dependency('gio-2.0')

dnf_ui_tests = executable(
  'dnf_ui_tests',
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
)

test('dnf_ui_tests', dnf_ui_tests)
