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
sdl2_dep = dependency('sdl2', required: true)
fftw3f_dep = dependency('fftw3f', required: true)
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
  # Safe everywhere (unlike -ffast-math) and lets the compiler inline lrintf/sqrtf on the
  # hot render path instead of emitting errno-setting library calls.
  '-fno-math-errno',
]
add_project_arguments(common_args, language: 'c')

# Hot DSP path: always build at -O3 regardless of the buildtype so the render loop stays fast
# even in the default (unoptimised) developer build the benchmark runs against.
sim_render_sources = files(
  'simulator/src/ddc.c',
  'simulator/src/ddc_cache.c',
  'simulator/src/nco.c',
  'simulator/src/renderer.c',
)

sim_sources = files(
  'simulator/src/asset_cache.c',
  'simulator/src/config.c',
  'simulator/src/iq_file_reader.c',
  'simulator/src/receiver.c',
  'simulator/src/rest_server.c',
  'simulator/src/ringbuffer.c',
  'simulator/src/scenario.c',
  'simulator/src/streamer.c',
  'simulator/src/timebase.c',
  'simulator/src/udp_output.c',
  'simulator/src/vita49_packet.c',
  'simulator/src/wav_reader.c',
  'simulator/src/yaml_tree.c'
)

sim_deps = [m_dep, thread_dep, microhttpd_dep, jansson_dep, yaml_dep]
if liquid_dep.found() and not liquid_resampler_opt.disabled()
  sim_deps += [liquid_dep]
endif
if volk_dep.found() and not volk_accel_opt.disabled()
  sim_deps += [volk_dep]
endif

sim_includes = include_directories('simulator/src', '.')
test_includes = include_directories('simulator/tests/unit')

sim_render_lib = static_library('simrender', sim_render_sources, dependencies: sim_deps, include_directories: sim_includes, override_options: ['optimization=3'])
sim_lib = static_library('simcore', sim_sources, dependencies: sim_deps, include_directories: sim_includes, link_with: sim_render_lib)

receiver_sources = files(
  'receiver/src/control_client.c',
  'receiver/src/history.c',
  'receiver/src/vita49_rx.c',
  'receiver/src/waterfall.c'
)

receiver_includes = include_directories('receiver/src')
receiver_deps = [m_dep, fftw3f_dep, jansson_dep]
receiver_lib = static_library('receivercore', receiver_sources, dependencies: receiver_deps, include_directories: receiver_includes)

# Starter files, embedded: written by `sdr-simulator --init` and run as-is when no setup is given.
setup_template_files = files('simulator/setup_template/receiver.yaml', 'simulator/setup_template/scenario.yaml')
setup_template_c = custom_target(
  'setup_template',
  input: setup_template_files,
  output: 'setup_template.c',
  command: [find_program('python3'), files('simulator/scripts/embed_files.py'), '@OUTPUT@', '@INPUT@']
)

simulator = executable(
  'sdr-simulator',
  ['simulator/src/main.c', 'simulator/src/setup_dir.c', setup_template_c],
  link_with: sim_lib,
  dependencies: sim_deps,
  include_directories: sim_includes,
  install: true
)

waterfall_receiver = executable(
  'sdr-waterfall-receiver',
  ['receiver/src/main.c', 'receiver/src/ui_text.c'],
  link_with: receiver_lib,
  dependencies: receiver_deps + [sdl2_dep],
  include_directories: receiver_includes,
  install: false
)

unit_tests = executable(
  'unit_tests',
  files(
    'simulator/tests/unit/test_config.c',
    'simulator/tests/unit/test_asset_cache.c',
    'simulator/tests/unit/test_ddc.c',
    'simulator/tests/unit/test_ddc_cache.c',
    'simulator/tests/unit/test_iq_file_reader.c',
    'simulator/tests/unit/test_renderer.c',
    'simulator/tests/unit/test_ringbuffer.c',
    'simulator/tests/unit/test_scenario.c',
    'simulator/tests/unit/test_streamer.c',
    'simulator/tests/unit/test_timebase.c',
    'simulator/tests/unit/test_vita49_packet.c',
    'simulator/tests/unit/test_wav_reader.c',
    'receiver/tests/test_receiver_c.c',
    'simulator/tests/unit/test_main.c'
  ),
  link_with: [sim_lib, receiver_lib],
  dependencies: sim_deps + receiver_deps + [check_dep],
  include_directories: [sim_includes, receiver_includes, test_includes]
)

test('unit', unit_tests, workdir: meson.project_source_root(), suite: 'unit')

renderer_benchmark = executable(
  'renderer_benchmark',
  'simulator/tests/benchmarks/renderer_benchmark.c',
  link_with: sim_lib,
  dependencies: sim_deps,
  include_directories: sim_includes
)

benchmark('renderer_80mhz_scalar_1_receiver', renderer_benchmark, args: ['1000', '4096', '--receivers', '1'], workdir: meson.project_source_root(), timeout: 60)
benchmark('renderer_80mhz_scalar_4_receivers', renderer_benchmark, args: ['1000', '4096', '--receivers', '4'], workdir: meson.project_source_root(), timeout: 60)
benchmark('renderer_80mhz_scalar_12_receivers', renderer_benchmark, args: ['1000', '4096', '--receivers', '12'], workdir: meson.project_source_root(), timeout: 60)
# Realistic P9 load: one receiver, all channels (wideband + narrowband), noise plus mixed IQ
# and audio signals. The realtime_ratio it prints is the headroom on a single core; run against
# a -O3/native build.
benchmark('renderer_realistic_load', renderer_benchmark, args: ['500', '4096', '--receivers', '1', '--all-channels', '--scenario', 'simulator/tests/benchmarks/benchmark_load.yaml'], workdir: meson.project_source_root(), timeout: 120)
# DDC load: one 80 MHz track-tuner channel plus 20 narrow DDC channels (500 kHz .. 1 kHz)
# extracting from the same shift-mode 80 MHz replay. First blocks include the per-channel
# intermediate cache builds; steady state is dominated by the wideband channel.
benchmark('renderer_ddc_channels', renderer_benchmark, args: ['200', '4096', '--receivers', '1', '--all-channels', '--sum-rates', '--config', 'simulator/configs/receiver_wideband_ddc.yaml', '--scenario', 'simulator/tests/benchmarks/benchmark_ddc.yaml'], workdir: meson.project_source_root(), timeout: 180)

pytest = find_program('pytest', required: false)
if pytest.found()
  test('integration', pytest, args: ['-q', 'simulator/tests/integration'], workdir: meson.project_source_root(), suite: 'integration', timeout: 30)
endif

clang_tidy_runner = find_program('simulator/scripts/run_clang_tidy.sh', required: false)
if clang_tidy_runner.found()
  run_target('clang-tidy', command: [clang_tidy_runner, meson.current_build_dir()])
endif
