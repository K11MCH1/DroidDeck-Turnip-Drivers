import sys
from pathlib import Path

EDITS = (
    ('include/android_stub/cutils/native_handle.h',
     'typedef const native_handle_t* buffer_handle_t;', 'typedef void* buffer_handle_t;'),
    ('src/util/u_gralloc/u_gralloc_fallback.c', ', hnd->handle', ', (void *)hnd->handle'),
    ('src/vulkan/runtime/vk_android.c', 'anb->handle->', '((const native_handle_t *)anb->handle)->'),
    ('meson.build', "    '-Werror=gnu-empty-initializer',\n", ''),
)

for path, old, new in EDITS:
    file = Path(path)
    text = file.read_text()
    count = text.count(old)
    if not count:
        sys.exit(f'{path}: anchor missing for NDK compatibility edit {old!r}')
    file.write_text(text.replace(old, new))
    print(f'{path}: {count} NDK compatibility edit(s)')
