import sys
from pathlib import Path

from mesa_edit import replace

KGSL = 'src/freedreno/vulkan/tu_knl_kgsl.cc'
DEVICE = 'src/freedreno/vulkan/tu_device.cc'
TIMESTAMP = 'static int\nkgsl_device_get_gpu_timestamp(struct tu_device *dev, uint64_t *ts)\n{\n   UNREACHABLE("");\n   return 0;\n}\n\n'

replace(KGSL, '#include <sys/mman.h>', '#include <sys/mman.h>\n#include <sys/stat.h>\n#include <sys/sysmacros.h>', 'sys/stat includes')
replace(KGSL, '   result = tu_physical_device_init(device, instance);', '''   struct stat kgsl_st;
   if (fstat(fd, &kgsl_st) == 0 && S_ISCHR(kgsl_st.st_mode)) {
      device->has_local = device->has_master = true;
      device->local_major = device->master_major = major(kgsl_st.st_rdev);
      device->local_minor = device->master_minor = minor(kgsl_st.st_rdev);
   }

   result = tu_physical_device_init(device, instance);''', 'KGSL device numbers for dma-buf feedback')
replace(DEVICE, '.EXT_physical_device_drm = !is_kgsl(device->instance),', '.EXT_physical_device_drm = !is_kgsl(device->instance) || device->has_local,', 'EXT_physical_device_drm on KGSL')

text = Path(KGSL).read_text()
if TIMESTAMP not in text:
    sys.exit(f'{KGSL}: KGSL timestamp stub changed')
Path(KGSL).write_text(text.replace(TIMESTAMP, '').replace('      .device_get_gpu_timestamp = kgsl_device_get_gpu_timestamp,\n', ''))
print(f'{KGSL}: removed unreachable GPU timestamp stub')

replace('src/freedreno/vulkan/tu_knl.cc', '   return dev->instance->knl->device_get_gpu_timestamp(dev, ts);', '''   if (dev->instance->knl->device_get_gpu_timestamp == NULL)
      return -1;
   return dev->instance->knl->device_get_gpu_timestamp(dev, ts);''', 'GPU timestamp null check')
for extension in ('KHR_calibrated_timestamps', 'EXT_calibrated_timestamps', 'EXT_present_timing'):
    replace(DEVICE, f'.{extension} = device->info->props.has_persistent_counter,', f'.{extension} = device->info->props.has_persistent_counter && device->instance->knl->device_get_gpu_timestamp != NULL,', f'{extension} gated on GPU timestamps')
for feature in ('presentTiming', 'presentAtRelativeTime', 'presentAtAbsoluteTime'):
    replace(DEVICE, f'features->{feature} = true;', f'features->{feature} = pdevice->vk.supported_extensions.EXT_present_timing;', f'{feature} gated on EXT_present_timing')
