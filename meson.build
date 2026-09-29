project(
  'egraph',
  'cpp',
  version: '0.0.0',
  license: 'GPL-2.0-only',
  meson_version: '>=1.3',
  default_options: [
    'cpp_std=c++23',
    'warning_level=3',
    'werror=true',
    'buildtype=debugoptimized',
  ],
)

cxx = meson.get_compiler('cpp')

add_project_arguments('-D_GLIBCXX_ASSERTIONS', language: 'cpp')
add_project_arguments(
  cxx.get_supported_arguments(
    '-Wconversion',
    '-Wsign-conversion',
    '-Wshadow',
    '-fstack-protector-strong',
    '-fstack-clash-protection',
    '-fcf-protection',
  ),
  language: 'cpp',
)
# FORTIFY needs optimization and fights the sanitizers' interceptors.
if get_option('optimization') not in ['0', 'plain'] and get_option('b_sanitize') == 'none'
  add_project_arguments('-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=3', language: 'cpp')
endif
if cxx.get_id() == 'clang'
  add_project_arguments('-Werror=unsafe-buffer-usage', language: 'cpp')
endif

cli11 = dependency('CLI11', version: '>=2.4')
catch2 = dependency('catch2-with-main', version: '>=3.4')
json = dependency('nlohmann_json', version: '>=3.10', include_type: 'system')
# System headers, so -Wunsafe-buffer-usage stays out of Notcurses' inline functions.
notcurses = dependency(
  'notcurses-core',
  version: '>=3',
  required: get_option('tui'),
  include_type: 'system',
)

build_info_hpp = configure_file(
  input: 'src/build_info.hpp.in',
  output: 'build_info.hpp',
  configuration: {
    'VERSION': meson.project_version(),
    'HAVE_TUI': notcurses.found() ? 1 : 0,
  },
)

egraph_sources = []
if notcurses.found()
  egraph_sources += 'src/screen.cpp'
endif

egraph_lib = static_library(
  'egraph',
  egraph_sources,
  'src/affected.cpp',
  'src/atom.cpp',
  'src/check.cpp',
  'src/cli.cpp',
  'src/depclean.cpp',
  'src/emerge.cpp',
  'src/encoding.cpp',
  'src/evaluated.cpp',
  'src/freshness.cpp',
  'src/graph.cpp',
  'src/human.cpp',
  'src/json.cpp',
  'src/os.cpp',
  'src/pressure.cpp',
  'src/query.cpp',
  'src/session.cpp',
  'src/steve.cpp',
  'src/store.cpp',
  'src/tui.cpp',
  'src/version.cpp',
  build_info_hpp,
  dependencies: [cli11, json, notcurses],
)
egraph_dep = declare_dependency(
  link_with: egraph_lib,
  include_directories: include_directories('src', '.'),
  dependencies: [cli11, notcurses],
)

egraph_exe = executable('egraph', 'src/main.cpp', dependencies: egraph_dep, install: true)

# egraph looks for egraph-build next to itself, then in PATH. In the build directory that is a
# wrapper running the builder from the source tree, under the portage in portage_lib (the
# installed one when empty); installed, it is a script over the installed package.
python = import('python').find_installation('python3')
builder_path = [meson.project_source_root() / 'builder']
if get_option('portage_lib') != ''
  builder_path += get_option('portage_lib')
endif
configure_file(
  input: 'builder/egraph-build.in',
  output: 'egraph-build',
  configuration: {'PYTHON': python.full_path(), 'PYTHONPATH': ':'.join(builder_path)},
)
builder_script = configure_file(
  input: 'builder/egraph-build.py.in',
  output: 'egraph-build.py',
  configuration: {'PYTHON': python.full_path()},
)
install_data(
  builder_script,
  install_dir: get_option('bindir'),
  rename: 'egraph-build',
  install_mode: 'rwxr-xr-x',
)
python.install_sources(
  files(
    'builder/egraph_build/__init__.py',
    'builder/egraph_build/__main__.py',
    'builder/egraph_build/build.py',
    'builder/egraph_build/cli.py',
    'builder/egraph_build/dynamic.py',
    'builder/egraph_build/evaluated.py',
    'builder/egraph_build/installed.py',
    'builder/egraph_build/masks.py',
    'builder/egraph_build/model.py',
    'builder/egraph_build/oracle.py',
    'builder/egraph_build/pending.py',
    'builder/egraph_build/profile.py',
    'builder/egraph_build/roots.py',
    'builder/egraph_build/store.py',
  ),
  subdir: 'egraph_build',
)

# portage's post_emerge hook, as a dispatcher over /etc/portage/post_emerge.d, and egraph's entry
# there and in postsync.d, which refreshes the stores after every emerge that changed the
# installed packages and after every sync.
egraph_hook = configure_file(
  input: 'hooks/egraph.in',
  output: 'egraph-hook',
  configuration: {'BINDIR': get_option('prefix') / get_option('bindir')},
)
if get_option('portage_hooks')
  install_data(
    'hooks/post_emerge',
    install_dir: get_option('portage_config') / 'bin',
    install_mode: 'rwxr-xr-x',
  )
  foreach hooks : ['post_emerge.d', 'postsync.d']
    install_data(
      egraph_hook,
      install_dir: get_option('portage_config') / hooks,
      rename: 'egraph',
      install_mode: 'rwxr-xr-x',
    )
  endforeach
endif

unit_tests = executable(
  'unit-tests',
  'tests/test_affected.cpp',
  'tests/test_atom.cpp',
  'tests/test_check.cpp',
  'tests/test_cli.cpp',
  'tests/test_depclean.cpp',
  'tests/test_emerge.cpp',
  'tests/test_evaluated.cpp',
  'tests/test_freshness.cpp',
  'tests/test_graph.cpp',
  'tests/test_human.cpp',
  'tests/test_json.cpp',
  'tests/test_pressure.cpp',
  'tests/test_session.cpp',
  'tests/test_shell.cpp',
  'tests/test_steve.cpp',
  'tests/test_store.cpp',
  'tests/test_tui.cpp',
  dependencies: [egraph_dep, catch2],
)
# Tests set egraph's options themselves, whatever the user's environment says.
egraph_variables = ['EGRAPH_STORE', 'EGRAPH_BUILD', 'EGRAPH_LAYOUT', 'EGRAPH_GLYPHS', 'ROOT',
                    'PORTAGE_CONFIGROOT', 'PORTAGE_OVERRIDE_EPREFIX']
unit_env = environment()
foreach variable : egraph_variables
  unit_env.unset(variable)
endforeach
test('unit', unit_tests, protocol: 'exitcode', env: unit_env)

# The builder imports portage, so its tests run under whichever portage is on PYTHONPATH:
# the fork by default, since ResolverPlayground needs portage's test GPG keys from a checkout.
pytest = find_program('pytest', required: false)
if pytest.found()
  builder_env = environment()
  foreach variable : egraph_variables
    builder_env.unset(variable)
  endforeach
  # The golden tests run the C++ reader on stores the Python writer produced.
  builder_env.set('EGRAPH', egraph_exe.full_path())
  if get_option('portage_lib') != ''
    builder_env.set('PYTHONPATH', get_option('portage_lib'))
  endif
  if get_option('portage_test_keys') != ''
    builder_env.set('EGRAPH_TEST_KEYS', get_option('portage_test_keys'))
  endif
  test(
    'builder',
    pytest,
    args: ['-p', 'no:cacheprovider', '-q', meson.project_source_root() / 'builder' / 'tests'],
    env: builder_env,
    depends: egraph_exe,
    timeout: 600,
  )
else
  warning('pytest not found: builder tests are not registered')
endif
