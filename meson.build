project(
  'sdr_receiver_simulator',
  'c',
  version: '0.1.0',
  default_options: ['c_std=c11', 'warning_level=3', 'werror=false']
)

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: true)
thread_dep = dependency('threads')
microhttpd_dep = dependency('libmicrohttpd', required: true)
jansson_dep = dependency('jansson', required: true)
yaml_dep = dependency('yaml-0.1', required: true)
check_dep = dependency('check', required: true)

common_args = [
  '-D_GNU_SOURCE',
  '-Wall',
  '-Wextra',
  '-Wpedantic',
  '-Wshadow',
  '-Wconversion',
  '-Wstrict-prototypes',
  '-Wmissing-prototypes',
]
add_project_arguments(common_args, language: 'c')

sim_sources = files(
  'src/asset_cache.c',
  'src/config.c',
  'src/iq_file_reader.c',
  'src/nco.c',
  'src/receiver.c',
  'src/renderer.c',
  'src/rest_server.c',
  'src/ringbuffer.c',
  'src/scenario.c',
  'src/streamer.c',
  'src/timebase.c',
  'src/udp_output.c'
)

sim_deps = [m_dep, thread_dep, microhttpd_dep, jansson_dep, yaml_dep]

sim_lib = static_library('simcore', sim_sources, dependencies: sim_deps, include_directories: include_directories('src'))

simulator = executable(
  'sdr-simulator',
  'src/main.c',
  link_with: sim_lib,
  dependencies: sim_deps,
  include_directories: include_directories('src'),
  install: false
)

unit_tests = executable(
  'unit_tests',
  files(
    'tests/unit/test_config.c',
    'tests/unit/test_asset_cache.c',
    'tests/unit/test_iq_file_reader.c',
    'tests/unit/test_renderer.c',
    'tests/unit/test_ringbuffer.c',
    'tests/unit/test_scenario.c',
    'tests/unit/test_streamer.c',
    'tests/unit/test_timebase.c',
    'tests/unit/test_main.c'
  ),
  link_with: sim_lib,
  dependencies: sim_deps + [check_dep],
  include_directories: include_directories('src')
)

test('unit', unit_tests, workdir: meson.project_source_root(), suite: 'unit')

renderer_benchmark = executable(
  'renderer_benchmark',
  'tests/benchmarks/renderer_benchmark.c',
  link_with: sim_lib,
  dependencies: sim_deps,
  include_directories: include_directories('src')
)

benchmark('renderer_80mhz_scalar_1_receiver', renderer_benchmark, args: ['1000', '4096', '--receivers', '1'], workdir: meson.project_source_root(), timeout: 60)
benchmark('renderer_80mhz_scalar_4_receivers', renderer_benchmark, args: ['1000', '4096', '--receivers', '4'], workdir: meson.project_source_root(), timeout: 60)
benchmark('renderer_80mhz_scalar_12_receivers', renderer_benchmark, args: ['1000', '4096', '--receivers', '12'], workdir: meson.project_source_root(), timeout: 60)

pytest = find_program('pytest', required: false)
if pytest.found()
  test('integration', pytest, args: ['-q', 'tests/integration'], workdir: meson.project_source_root(), suite: 'integration', timeout: 30)
endif

clang_tidy_runner = find_program('scripts/run_clang_tidy.sh', required: false)
if clang_tidy_runner.found()
  run_target('clang-tidy', command: [clang_tidy_runner, meson.current_build_dir()])
endif
