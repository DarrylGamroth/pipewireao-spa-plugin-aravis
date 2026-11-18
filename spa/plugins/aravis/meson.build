aravis_lib = shared_library(
  'spa-aravis',
  ['plugin.c', 'source.c', 'camera.c', 'params.c'],
  dependencies: [spa_dep, aravis_dep],
  include_directories: plugin_include,
  c_args: ['-Wno-pedantic'],
  install: true,
  install_dir: spa_pkg_dep.get_variable(pkgconfig: 'plugindir') / 'aravis',
  install_tag: 'aravis',
)

if get_option('tests').allowed()
  aravis_factory_test = executable(
    'spa-aravis-factory-test',
    'test-factory.c',
    dependencies: [spa_dep, dl_dep],
    c_args: ['-Wno-pedantic'],
  )
  test(
    'spa-aravis-factory',
    aravis_factory_test,
    args: [aravis_lib.full_path()],
  )

  capture_test = executable(
    'spa-aravis-capture-test',
    'test-capture.c',
    dependencies: [spa_dep, dl_dep],
    c_args: ['-Wno-pedantic'],
  )

  camera_test = executable(
    'spa-aravis-camera-test',
    ['test-camera.c', 'camera.c'],
    dependencies: [spa_dep, aravis_dep],
  )

  if get_option('aravis-gige-test')
    fake_camera = find_program('arv-fake-gv-camera-0.10')
    arv_tool = find_program('arv-tool-0.10')
    python = import('python').find_installation('python3')
    wrapped = [
      'arv_camera_create_stream', 'arv_camera_get_payload', 'arv_camera_get_region',
      'arv_camera_get_pixel_format_as_string', 'arv_camera_get_model_name',
      'arv_camera_get_frame_rate', 'arv_camera_start_acquisition',
      'arv_camera_stop_acquisition', 'aravis_camera_announce', 'aravis_camera_queue',
      'aravis_camera_refresh_info', 'arv_buffer_get_status', 'arv_buffer_get_image_data',
      'aravis_camera_revoke', 'arv_camera_get_device_serial_number', 'arv_gc_integer_get_min',
    ]
    wrap_args = []
    foreach function : wrapped
      wrap_args += ['-Wl,--wrap=' + function]
    endforeach
    aravis_faults = shared_library(
      'spa-aravis-faults',
      ['plugin.c', 'source.c', 'camera.c', 'params.c', 'test-faults.c'],
      dependencies: [spa_dep, aravis_dep],
      include_directories: plugin_include,
      c_args: ['-Wno-pedantic'],
      link_args: wrap_args,
    )
    test(
      'spa-aravis-gige-loopback',
      python,
      args: [files('test-gige-loopback.py'), fake_camera.full_path(), arv_tool.full_path(),
        camera_test.full_path(), capture_test.full_path(), aravis_lib.full_path(),
        aravis_faults.full_path()],
      depends: [camera_test, capture_test, aravis_lib, aravis_faults],
      is_parallel: false,
      timeout: 90,
    )
  endif

  executable(
    'spa-aravis-completion-benchmark',
    ['benchmark-completion.c', 'camera.c'],
    dependencies: [spa_dep, aravis_dep],
  )
endif
