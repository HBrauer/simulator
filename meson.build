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
liquid_resampler_opt = get_option('liquid_resampler')
volk_accel_opt = get_option('volk_accel')
liquid_dep = cc.find_library('liquid', required: liquid_resampler_opt)
volk_dep = dependency('volk', required: volk_accel_opt)

config_data = configuration_data()
config_data.set10('SIM_USE_LIQUID_RESAMPLER', liquid_dep.found() and not liquid_resampler_opt.disabled())
config_data.set10('SIM_HAVE_VOLK', volk_dep.found() and not volk_accel_opt.disabled())
configure_file(output: 'sim_config.h', configuration: config_data)

if get_option('native_optimizations')
  if cc.has_argument('-march=native')
    add_project_arguments('-march=native', language: 'c')
    add_project_link_arguments('-march=native', language: 'c')
  endif
endif

if get_option('fast_math')
  if cc.has_argument('-ffast-math')
    add_project_arguments('-ffast-math', language: 'c')
  endif
endif

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
if liquid_dep.found() and not liquid_resampler_opt.disabled()
  sim_deps += [liquid_dep]
endif
if volk_dep.found() and not volk_accel_opt.disabled()
  sim_deps += [volk_dep]
endif

sim_includes = include_directories('src', '.')

sim_lib = static_library('simcore', sim_sources, dependencies: sim_deps, include_directories: sim_includes)

simulator = executable(
  'sdr-simulator',
  'src/main.c',
  link_with: sim_lib,
  dependencies: sim_deps,
  include_directories: sim_includes,
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
  include_directories: sim_includes
)

test('unit', unit_tests, workdir: meson.project_source_root(), suite: 'unit')

renderer_benchmark = executable(
  'renderer_benchmark',
  'tests/benchmarks/renderer_benchmark.c',
  link_with: sim_lib,
  dependencies: sim_deps,
  include_directories: sim_includes
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
