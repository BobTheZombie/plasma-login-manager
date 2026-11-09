project('plasma-login-manager', 'cpp',
  version: '0.1.0',
  default_options: ['cpp_std=c++20', 'warning_level=2', 'buildtype=release'])

cc = meson.get_compiler('cpp')
qt6 = import('qt6')
qtcore = dependency('qt6', modules: ['Core'])
qtnetwork = dependency('qt6', modules: ['Core', 'Network'])
qtdbus = dependency('qt6', modules: ['Core', 'DBus'])
qtquick = dependency('qt6', modules: ['Core', 'Gui', 'Qml', 'Quick', 'QuickControls2', 'Network'])
systemd = dependency('libsystemd', required: true)
pam = cc.find_library('pam', required: true)
if not cc.has_header('security/pam_appl.h')
  error('Missing PAM development headers')
endif

executable('plmd', 'src/daemon.cpp',
  dependencies: [qtcore, qtnetwork, qtdbus, systemd],
  install: true, install_dir: get_option('libexecdir'))
executable('plm-session', 'src/session.cpp',
  dependencies: [pam, systemd],
  install: true, install_dir: get_option('libexecdir'))
greeter_moc = qt6.preprocess(moc_headers: ['src/backend.h'])
executable('plm-greeter', ['src/greeter.cpp', greeter_moc],
  dependencies: [qtquick],
  install: true, install_dir: get_option('libexecdir'))

install_data('qml/Main.qml', install_dir: get_option('datadir') / 'plm')
install_data('data/plmd.service', install_dir: get_option('prefix') / 'lib/systemd/system')
install_data('data/plm.sysusers', install_dir: get_option('prefix') / 'lib/sysusers.d', rename: 'plm.conf')
install_data('data/plm.tmpfiles', install_dir: get_option('prefix') / 'lib/tmpfiles.d', rename: 'plm.conf')
install_data('data/pam/plm', 'data/pam/plm-greeter',
  install_dir: get_option('sysconfdir') / 'pam.d')
