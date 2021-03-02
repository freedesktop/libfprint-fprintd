project('fprintd', 'c',
    version: '1.90.9',
    license: 'GPLv2+',
    default_options: [
        'buildtype=debugoptimized',
        'warning_level=3',
        'c_std=gnu99',
    ],
    meson_version: '>= 0.50.0')

gnome = import('gnome')
i18n = import('i18n')

cc = meson.get_compiler('c')
common_cflags = cc.get_supported_arguments([
    '-fno-strict-aliasing',
    '-Wcast-align',
    '-Werror=address',
    '-Werror=array-bounds',
    '-Werror=empty-body',
    '-Werror=implicit',
    '-Werror=init-self',
    '-Werror=int-to-pointer-cast',
    '-Werror=main',
    '-Werror=missing-braces',
    '-Werror=nonnull',
    '-Werror=pointer-to-int-cast',
    '-Werror=redundant-decls',
    '-Werror=return-type',
    '-Werror=sequence-point',
    '-Werror=trigraphs',
    '-Werror=write-strings',
    '-Wformat-nonliteral',
    '-Wformat-security',
    '-Wformat=2',
    '-Wignored-qualifiers',
    '-Wimplicit-function-declaration',
    '-Wlogical-op',
    '-Wmissing-declarations',
    '-Wmissing-format-attribute',
    '-Wmissing-include-dirs',
    '-Wmissing-noreturn',
    '-Wmissing-prototypes',
    '-Wnested-externs',
    '-Wold-style-definition',
    '-Wpointer-arith',
    '-Wshadow',
    '-Wstrict-prototypes',
    '-Wtype-limits',
    '-Wundef',
    '-Wunused',
])
add_project_arguments(common_cflags, language: 'c')

common_cflags = cc.get_supported_arguments([
    # The stub passes a lot of params that we do not use, maybe a good idea to
    # mark it appropriately, but this works well for now.
    '-Wno-unused-parameter',
    # We use g_signal_handlers_disconnect_* which is not compatible with -Wpedantic
    '-Wno-pedantic',
])
add_project_arguments(common_cflags, language: 'c')

host_system = host_machine.system()
# NOTE: Bump gdbus-codegen min version once we can depend on 2.64!
glib_min_version = '2.56'
libfprint_min_version = '1.90.1'

fprintd_installdir = get_option('prefix') / get_option('libexecdir')
fprintd_plugindir = get_option('prefix') / get_option('libdir') / meson.project_name() / 'modules'
storage_path = get_option('prefix') / get_option('localstatedir') / 'lib/fprint'
localedir = get_option('prefix') / get_option('localedir')
datadir = get_option('prefix') / get_option('datadir')
sysconfdir = get_option('sysconfdir')
if get_option('prefix') != '/usr'
    sysconfdir = get_option('prefix') / sysconfdir
endif

# Dependencies
glib_dep = dependency('glib-2.0', version: '>=' + glib_min_version)
gio_dep = dependency('gio-2.0', version: '>=' + glib_min_version)
gio_unix_dep = dependency('gio-unix-2.0', version: '>=' + glib_min_version)
gmodule_dep = dependency('gmodule-2.0', version: '>=' + glib_min_version)
libfprint_dep = dependency('libfprint-2', version: '>=' + libfprint_min_version)
polkit_gobject_dep = dependency('polkit-gobject-1', version: '>= 0.91')
dbus_dep = dependency('dbus-1', required: false)
libsystemd_dep = dependency('libsystemd', required: get_option('pam'))
pam_dep = cc.find_library('pam',
    required: get_option('pam'),
    has_headers: 'security/pam_modules.h',
)

pod2man = find_program('pod2man', required: get_option('man'))
xsltproc = find_program('xsltproc', required: get_option('gtk_doc'))

# StateDirectory was introduced in systemd 235
systemd_dep = dependency('systemd', version: '>= 235', required: false)
systemd_unit_dir = get_option('systemd_system_unit_dir')

if systemd_unit_dir == '' and systemd_dep.found()
    systemd_unit_dir = systemd_dep.get_pkgconfig_variable('systemdsystemunitdir')
endif

if get_option('systemd') and systemd_unit_dir == ''
    error('systemd development files or systemd_system_unit_dir is needed for systemd support.')
endif

dbus_service_dir = get_option('dbus_service_dir')
dbus_data_dir = datadir
dbus_interfaces_dir = ''

if dbus_dep.found()
    if dbus_service_dir == ''
        dbus_service_dir = dbus_dep.get_pkgconfig_variable('system_bus_services_dir')
    endif
    dbus_interfaces_dir = dbus_dep.get_pkgconfig_variable('interfaces_dir')
    dbus_data_dir = dbus_dep.get_pkgconfig_variable('datadir')
endif

dbus_conf_dir = dbus_data_dir / 'dbus-1/system.d'

if dbus_service_dir == ''
    dbus_service_dir = datadir / 'dbus-1/system-services'
endif
if dbus_interfaces_dir == ''
    dbus_interfaces_dir = datadir / 'dbus-1/interfaces'
endif

polkit_policy_directory = polkit_gobject_dep.get_pkgconfig_variable('policydir')

# Tests dependencies
pam_wrapper_dep = dependency('pam_wrapper', required: get_option('pam'))

xmllint = find_program('xmllint', required: false)
python3 = find_program('python3') # No meson without it!
python3_test_modules = {
    'cairo': true,
    'dbus': true,
    'dbusmock': true,
    'gi': true,
    'gi.repository.FPrint': true,
    'pypamtest': get_option('pam'),
}
python3_available_modules = []

foreach module, required : python3_test_modules
    if required and run_command(python3, '-c', 'import @0@'.format(module)).returncode() != 0
        error('Python3 module \'' + module + '\' required by test suite not found')
    endif
endforeach

cdata = configuration_data()
cdata.set_quoted('GETTEXT_PACKAGE', meson.project_name())
cdata.set_quoted('PACKAGE_VERSION', meson.project_version())
cdata.set_quoted('VERSION', meson.project_version())
cdata.set_quoted('SYSCONFDIR', sysconfdir)
cdata.set('POLKIT_HAS_AUTOPOINTERS', polkit_gobject_dep.version().version_compare('>= 0.114'))

config_h = configure_file(
  input: 'config.h.in',
  output: 'config.h',
  configuration: cdata
)

subdir('src')
subdir('data')
subdir('utils')
if get_option('pam')
    subdir('pam')
endif
if get_option('gtk_doc')
    subdir('doc')
endif
subdir('tests')
subdir('po')

output = []
output += 'System paths:'
output += '  prefix: ' + get_option('prefix')
output += '  fprintd daemon directory: ' + fprintd_installdir
output += '  fprintd modules directory: ' + fprintd_plugindir
output += '  fprintd prints storage directory: ' + storage_path
output += '  DBus configuration directory: ' + dbus_conf_dir
output += '  DBus service directory: ' + dbus_service_dir
output += '  DBus interfaces directory: ' + dbus_interfaces_dir
output += '  Polkit policy directory: ' + polkit_policy_directory
output += '  Systemd service directory: ' + systemd_unit_dir
if get_option('pam')
    output += '  PAM module directory: ' + pam_modules_dir
endif
output += '\nOptional features:\n'
output += '  PAM module: ' + (pam_dep.found() and get_option('pam')).to_string()
output += '  Manuals: ' + get_option('man').to_string()
output += '  GTK Doc: ' + get_option('gtk_doc').to_string()
output += '  XML Linter ' + xmllint.found().to_string()
output += '\nTest setup:\n'
output += '  With address sanitizer: ' + address_sanitizer.to_string()

message('\n'+'\n'.join(output)+'\n')
