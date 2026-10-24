#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")" && pwd)
mesa=$(cd "${1:?source directory required}" && pwd)
work=$(realpath -m "${2:?build directory required}")
sysroot="$work/sysroot"
mkdir -p "$work"
bash "$root/linux_sysroot.sh" "$sysroot"

cat > "$work/cross.ini" <<INI
[binaries]
c = 'aarch64-linux-gnu-gcc'
cpp = 'aarch64-linux-gnu-g++'
ar = 'aarch64-linux-gnu-ar'
strip = 'aarch64-linux-gnu-strip'
pkg-config = 'pkg-config'
wayland-scanner = '$(command -v wayland-scanner)'

[properties]
sys_root = '$sysroot'
pkg_config_libdir = '$sysroot/usr/lib/aarch64-linux-gnu/pkgconfig:$sysroot/usr/share/pkgconfig'

[built-in options]
c_args = ['--sysroot=$sysroot']
cpp_args = ['--sysroot=$sysroot']
c_link_args = ['--sysroot=$sysroot']
cpp_link_args = ['--sysroot=$sysroot']

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
INI

meson setup "$work/build" "$mesa" --cross-file "$work/cross.ini" --buildtype release \
  -Dvulkan-drivers=freedreno -Dfreedreno-kmds=msm,kgsl -Dgallium-drivers= -Dplatforms=wayland,x11 \
  -Dopengl=false -Dgbm=disabled -Dglx=disabled -Degl=disabled -Dllvm=disabled -Dvulkan-layers= \
  -Dtools= -Dvideo-codecs= -Dxmlconfig=disabled
ninja -C "$work/build" -j "${BUILD_JOBS:-$(nproc)}" src/freedreno/vulkan/libvulkan_freedreno.so
mkdir -p "$work/out"
aarch64-linux-gnu-strip -o "$work/out/libvulkan_freedreno.so" \
  "$work/build/src/freedreno/vulkan/libvulkan_freedreno.so"
