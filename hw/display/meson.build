hw_display_modules = {}

libyuv = subproject('libyuv')
libyuv_dep = libyuv.get_variable('yuv_dep')
if get_option('system_shared_lib')
  # libyuv's 'yuv' archive only bundles its object libraries, which CMake builds
  # as PIC, but meson's CMake conversion marks the archive itself non-PIC and
  # refuses to put it in a shared library. Link the object libraries directly.
  libyuv_parts = [libyuv_dep.partial_dependency(compile_args: true, includes: true)]
  foreach part : ['yuv_common_objects', 'yuv_neon64', 'yuv_sve', 'yuv_sme']
    libyuv_parts += libyuv.get_variable(part + '_dep', [])
  endforeach
  libyuv_dep = declare_dependency(dependencies: libyuv_parts)
endif
system_ss.add(when: 'CONFIG_APPLE_SOC', if_true: [libyuv_dep, files('apple_displaypipe_v4.c', 'apple_displaypipe_v2.c', 'synopsys_mipi_dsim.c', 'apple_scaler.c')])
system_ss.add(when: 'CONFIG_DDC', if_true: files('i2c-ddc.c'))
system_ss.add(when: 'CONFIG_EDID', if_true: files('edid-generate.c', 'edid-region.c'))

system_ss.add(when: 'CONFIG_FRAMEBUFFER', if_true: files('framebuffer.c'))

system_ss.add(when: 'CONFIG_DPCD', if_true: files('dpcd.c'))

modules += { 'hw-display': hw_display_modules }
