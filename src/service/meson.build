service_sources = files(
  'transaction_service.cpp',
  'transaction_service_preview_formatter.cpp',
  'transaction_service_main.cpp',
)

dnfui_service = executable(
  'dnfui-service',
  backend_sources + service_sources,
  include_directories: src_inc,
  dependencies: [
    libdnf5_dep,
    polkit_dep,
  ],
  install: true,
  install_dir: get_option('libexecdir'),
  install_tag: 'transaction-service',
)
