#!/usr/bin/env bash
set -euo pipefail
sysroot=$(realpath -m "$1")
apt_root="$sysroot.apt"
suite=noble
mirror=http://ports.ubuntu.com/ubuntu-ports
keyring=/usr/share/keyrings/ubuntu-archive-keyring.gpg
packages=(
  libc6-dev libstdc++-13-dev linux-libc-dev libdrm-dev libwayland-dev wayland-protocols
  libx11-xcb-dev libxcb-dri3-dev libxcb-present-dev libxcb-randr0-dev libxcb-sync-dev
  libxcb-xfixes0-dev libxcb-keysyms1-dev libxcb-shm0-dev libxshmfence-dev libxrandr-dev
  libzstd-dev zlib1g-dev libexpat1-dev
)
rm -rf "$sysroot" "$apt_root"
mkdir -p "$sysroot" "$apt_root"/etc/apt/{apt.conf.d,preferences.d,sources.list.d,trusted.gpg.d} \
  "$apt_root"/var/lib/apt/lists/partial "$apt_root"/var/cache/apt/archives/partial "$apt_root"/var/lib/dpkg
touch "$apt_root/var/lib/dpkg/status"
for pocket in "$suite" "$suite-updates" "$suite-security"; do
  echo "deb [arch=arm64 signed-by=$keyring] $mirror $pocket main universe"
done > "$apt_root/etc/apt/sources.list"
apt_opts=(-o "Dir=$apt_root" -o "Dir::State::status=$apt_root/var/lib/dpkg/status"
  -o APT::Architecture=arm64 -o APT::Architectures=arm64 -o Debug::NoLocking=1 -qq)
apt-get "${apt_opts[@]}" update
apt-get "${apt_opts[@]}" install -y --download-only --no-install-recommends "${packages[@]}"
for deb in "$apt_root"/var/cache/apt/archives/*.deb; do
  dpkg-deb -x "$deb" "$sysroot"
done
for dir in bin sbin lib; do
  [[ -e "$sysroot/$dir" ]] || ln -s "usr/$dir" "$sysroot/$dir"
done
find "$sysroot" -type l -lname '/*' | while read -r link; do
  ln -sfn "$(realpath -m --relative-to="$(dirname "$link")" "$sysroot$(readlink "$link")")" "$link"
done
rm -rf "$apt_root"
