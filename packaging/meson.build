i18n = import('i18n')

install_data(
  'com.fedora.dnfui.policy',
  install_dir: polkit_action_dir,
  install_tag: 'transaction-service',
)

i18n.merge_file(
  input: 'com.fedora.dnfui.desktop.in',
  output: 'com.fedora.dnfui.desktop',
  type: 'desktop',
  po_dir: '../po',
  install: true,
  install_dir: desktop_file_dir,
  install_tag: 'runtime',
)

install_data(
  'com.fedora.dnfui.metainfo.xml',
  install_dir: metainfo_dir,
  install_tag: 'runtime',
)

install_data(
  'com.fedora.Dnfui.Transaction1.service',
  install_dir: dbus_system_service_dir,
  install_tag: 'transaction-service',
)

install_data(
  'com.fedora.Dnfui.Transaction1.conf',
  install_dir: dbus_system_policy_dir,
  install_tag: 'transaction-service',
)

install_data(
  'dnfui-service.service',
  install_dir: systemd_system_unit_dir,
  install_tag: 'transaction-service',
)

app_icon_sizes = ['48', '64', '128', '256', '512', '1024']

foreach app_icon_size : app_icon_sizes
  app_icon_size_dir = app_icon_size + 'x' + app_icon_size

  install_data(
    join_paths('icons', 'hicolor', app_icon_size_dir, 'apps', 'com.fedora.dnfui.png'),
    install_dir: join_paths(app_icon_root_dir, app_icon_size_dir, 'apps'),
    install_tag: 'runtime',
  )
endforeach
