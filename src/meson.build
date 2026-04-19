backend_sources = files(
  'base_manager.cpp',
  'dnf_backend/dnf_common.cpp',
  'dnf_backend/dnf_details.cpp',
  'dnf_backend/dnf_query.cpp',
  'dnf_backend/dnf_state.cpp',
  'dnf_backend/dnf_transaction.cpp',
)

app_sources = files(
  'app.cpp',
  'config.cpp',
  'main.cpp',
  'package_info_controller.cpp',
  'package_query_controller.cpp',
  'package_table_view.cpp',
  'pending_transaction_controller.cpp',
  'transaction_progress.cpp',
  'transaction_service_client.cpp',
  'ui_helpers.cpp',
  'widgets.cpp',
)

dnfui_app = executable(
  'dnfui',
  backend_sources + app_sources,
  include_directories: src_inc,
  dependencies: [
    libdnf5_dep,
    gtk4_dep,
    polkit_dep,
  ],
  install: true,
  install_tag: 'runtime',
)

subdir('service')
