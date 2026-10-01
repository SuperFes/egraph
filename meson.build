project(
  'egraph',
  'cpp',
  version: '0.2.1',
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
  'src/action.cpp',
  'src/affected.cpp',
  'src/atom.cpp',
  'src/blockers.cpp',
  'src/check.cpp',
  'src/cli.cpp',
  'src/completion.cpp',
  'src/depclean.cpp',
  'src/emerge.cpp',
  'src/encoding.cpp',
  'src/evaluated.cpp',
  'src/freshness.cpp',
  'src/graph.cpp',
  'src/human.cpp',
  'src/json.cpp',
  'src/notices.cpp',
  'src/os.cpp',
  'src/package_use.cpp',
  'src/plan.cpp',
  'src/pressure.cpp',
  'src/query.cpp',
  'src/remedy.cpp',
  'src/remove.cpp',
  'src/request.cpp',
  'src/required_use.cpp',
  'src/selection.cpp',
  'src/session.cpp',
  'src/steve.cpp',
  'src/store.cpp',
  'src/tui.cpp',
  'src/use_changes.cpp',
  'src/use_reduce.cpp',
  'src/verify.cpp',
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

# Completion scripts, generated from the command line's definition where each shell looks.
completions_exe = executable(
  'egraph-completions',
  'src/completions_main.cpp',
  dependencies: egraph_dep,
)
completion_scripts = []
foreach shell : [
  ['bash', 'egraph.bash', get_option('datadir') / 'bash-completion' / 'completions'],
  ['zsh', '_egraph', get_option('datadir') / 'zsh' / 'site-functions'],
  ['fish', 'egraph.fish', get_option('datadir') / 'fish' / 'vendor_completions.d'],
]
  completion_scripts += custom_target(
    'completion-' + shell[0],
    output: shell[1],
    command: [completions_exe, shell[0]],
    capture: true,
    install: true,
    install_dir: shell[2],
  )
endforeach

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
    'builder/egraph_build/notices.py',
    'builder/egraph_build/oracle.py',
    'builder/egraph_build/pending.py',
    'builder/egraph_build/profile.py',
    'builder/egraph_build/roots.py',
    'builder/egraph_build/store.py',
  ),
  subdir: 'egraph_build',
)

foreach page : ['egraph.1', 'egraph-build.1']
  install_man(
    configure_file(
      input: 'docs' / page,
      output: page,
      configuration: {'VERSION': meson.project_version()},
    ),
  )
endforeach

# egraph's entry in /etc/portage/post_emerge.d and postsync.d, which refreshes the stores after
# every emerge that changed the installed packages and after every sync. portage runs postsync.d
# itself, but only /etc/portage/bin/post_emerge after an emerge, which is the user's own file: the
# dispatcher running post_emerge.d is installed for the user to link there.
egraph_hook = configure_file(
  input: 'hooks/egraph.in',
  output: 'egraph-hook',
  configuration: {'BINDIR': get_option('prefix') / get_option('bindir')},
)
if get_option('portage_hooks')
  install_data(
    'hooks/post_emerge',
    install_dir: get_option('datadir') / 'egraph',
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
  'tests/test_action.cpp',
  'tests/test_affected.cpp',
  'tests/test_atom.cpp',
  'tests/test_blockers.cpp',
  'tests/test_check.cpp',
  'tests/test_cli.cpp',
  'tests/test_completion.cpp',
  'tests/test_depclean.cpp',
  'tests/test_emerge.cpp',
  'tests/test_evaluated.cpp',
  'tests/test_freshness.cpp',
  'tests/test_graph.cpp',
  'tests/test_human.cpp',
  'tests/test_json.cpp',
  'tests/test_man.cpp',
  'tests/test_notices.cpp',
  'tests/test_package_use.cpp',
  'tests/test_plan.cpp',
  'tests/test_pressure.cpp',
  'tests/test_remedy.cpp',
  'tests/test_remove.cpp',
  'tests/test_request.cpp',
  'tests/test_required_use.cpp',
  'tests/test_selection.cpp',
  'tests/test_session.cpp',
  'tests/test_shell.cpp',
  'tests/test_steve.cpp',
  'tests/test_store.cpp',
  'tests/test_tui.cpp',
  'tests/test_use_changes.cpp',
  'tests/test_use_reduce.cpp',
  'tests/test_verify.cpp',
  dependencies: [egraph_dep, catch2],
  cpp_args: '-DEGRAPH_SOURCE_DIR="' + meson.project_source_root() + '"',
)
# Answers REQUIRED_USE checks and dependency reductions for the builder's tests, which hold them
# to portage's.
shadow_exe = executable(
  'shadow',
  'tests/shadow.cpp',
  dependencies: [egraph_dep, json],
)
# Tests set egraph's options themselves, whatever the user's environment says.
egraph_variables = ['EGRAPH_STORE', 'EGRAPH_BUILD', 'EGRAPH_EMERGE', 'EGRAPH_LAYOUT', 'EGRAPH_GLYPHS', 'ROOT',
                    'PORTAGE_CONFIGROOT', 'PORTAGE_OVERRIDE_EPREFIX']
# Empty rather than unset: environment().unset() leaves an inherited variable in place (portage
# exports PORTAGE_CONFIGROOT to a package's tests), and CLI11 takes an empty one as absent.
unit_env = environment()
foreach variable : egraph_variables
  unit_env.set(variable, '')
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
  builder_env.set('EGRAPH_COMPLETIONS', meson.current_build_dir())
  builder_env.set('EGRAPH_SHADOW', shadow_exe.full_path())
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
    depends: [egraph_exe, completion_scripts, shadow_exe],
    timeout: 600,
  )
else
  warning('pytest not found: builder tests are not registered')
endif
