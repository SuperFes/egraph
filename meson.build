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

version_hpp = configure_file(
  input: 'src/version.hpp.in',
  output: 'version.hpp',
  configuration: {'VERSION': meson.project_version()},
)

egraph_lib = static_library(
  'egraph',
  'src/cli.cpp',
  'src/freshness.cpp',
  'src/json.cpp',
  'src/os.cpp',
  'src/store.cpp',
  version_hpp,
  dependencies: cli11,
)
egraph_dep = declare_dependency(
  link_with: egraph_lib,
  include_directories: include_directories('src', '.'),
  dependencies: cli11,
)

egraph_exe = executable('egraph', 'src/main.cpp', dependencies: egraph_dep, install: true)

unit_tests = executable(
  'unit-tests',
  'tests/test_cli.cpp',
  'tests/test_freshness.cpp',
  'tests/test_json.cpp',
  'tests/test_store.cpp',
  dependencies: [egraph_dep, catch2],
)
test('unit', unit_tests, protocol: 'exitcode')

# The builder imports portage, so its tests run under whichever portage is on PYTHONPATH:
# the fork by default, since ResolverPlayground needs portage's test GPG keys from a checkout.
pytest = find_program('pytest', required: false)
if pytest.found()
  builder_env = environment()
  # The golden tests run the C++ reader on stores the Python writer produced.
  builder_env.set('EGRAPH', egraph_exe.full_path())
  if get_option('portage_lib') != ''
    builder_env.set('PYTHONPATH', get_option('portage_lib'))
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
