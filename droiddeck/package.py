import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import zipfile
from pathlib import Path

from pipeline import ROOT, git, validate_provenance

LIBRARY = 'libvulkan_freedreno.so'


def verify_elf(data, dynamic, sysroot):
    if len(data) < 64 or data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('Expected a little-endian ELF64 library')
    if struct.unpack_from('<HH', data, 16) != (3, 183):
        raise ValueError('Expected an AArch64 shared library')
    needed = sorted(set(re.findall(r'\(NEEDED\).*?\[(.+?)\]', dynamic)))
    if 'libc.so.6' not in needed or {'libc.so', 'liblog.so', 'libandroid.so'} & set(needed):
        raise ValueError(f'Expected glibc Linux dependencies, got {needed}')
    for name in needed:
        if '/' in name or not any((sysroot / directory / name).is_file() for directory in
                                 ('usr/lib/aarch64-linux-gnu', 'lib/aarch64-linux-gnu', 'usr/lib', 'lib')):
            raise ValueError(f'Dependency missing from Linux sysroot: {name}')
    for marker in (b'vk_icdGetInstanceProcAddr', b'vkCreateWaylandSurfaceKHR',
                   b'vkCreateXcbSurfaceKHR', b'Failed to set initial PWR_MAX constraint'):
        if marker not in data:
            raise ValueError(f'Missing expected driver marker: {marker!r}')
    if b'drirc.d' in data:
        raise ValueError('Driver must use compiled defaults, without runtime drirc files')
    return needed


def min_glibc(symbols):
    versions = {tuple(map(int, text.split('.'))) for text in re.findall(r'@GLIBC_([0-9.]+)', symbols)}
    if not versions or max(versions) > (2, 39):
        raise ValueError(f'Expected a glibc requirement at most 2.39, got {sorted(versions)}')
    return '.'.join(map(str, max(versions)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mesa', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--sysroot', type=Path, required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--dist', type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', args.version):
        raise ValueError('Version must be X.Y.Z')
    mesa = args.mesa.resolve()
    provenance = validate_provenance(mesa)
    if 'linux' not in provenance or git(mesa, 'status', '--porcelain', '--untracked-files=all'):
        raise ValueError('Packaging requires clean, committed Linux source')
    kgsl = (mesa / 'src/freedreno/vulkan/tu_knl_kgsl.cc').read_text()
    if ('device->local_major = device->master_major' not in kgsl or
            'kgsl_device_get_gpu_timestamp' in kgsl):
        raise ValueError('Required Linux KGSL adaptations are missing')
    data = args.library.read_bytes()
    dynamic = subprocess.check_output(['aarch64-linux-gnu-readelf', '-d', str(args.library)], text=True)
    symbols = subprocess.check_output(['aarch64-linux-gnu-readelf', '--dyn-syms', '--wide', str(args.library)], text=True)
    needed = verify_elf(data, dynamic, args.sysroot)
    glibc = min_glibc(symbols)
    digest = hashlib.sha256(data).hexdigest()
    variant = provenance['variant']
    source_commit = git(mesa, 'rev-parse', 'HEAD')
    name = f'DroidDeck-Turnip-v{args.version}-{variant}-Linux-{source_commit[:10]}'
    args.dist.mkdir(parents=True, exist_ok=True)
    header = (mesa / 'include/vulkan/vulkan_core.h').read_text()
    version = re.search(r'#define VK_HEADER_VERSION_COMPLETE VK_MAKE_API_VERSION\(0, (\d+), (\d+), VK_HEADER_VERSION\)', header)
    patch = re.search(r'^#define VK_HEADER_VERSION (\d+)$', header, re.MULTILINE)
    if not version or not patch:
        raise ValueError('Cannot derive Vulkan ICD API version from Mesa headers')
    api_version = f'{version[1]}.{version[2]}.{patch[1]}'
    meta = {
        'schemaVersion': 1, 'kind': 'linux-vulkan-icd',
        'name': f'DroidDeck Turnip {args.version} ({variant}, Linux)',
        'driverVersion': name, 'libraryName': LIBRARY,
        'platform': 'linux', 'architecture': 'aarch64', 'libc': 'glibc',
        'minGlibc': glibc, 'wsi': ['wayland', 'x11'], 'kmds': ['kgsl', 'msm'],
        'mesaVersion': provenance['mesaVersion'], 'mesaCommit': provenance['mesaCommit'],
        'sourceCommit': source_commit, 'librarySha256': digest, 'dependencies': needed,
        'sourceArchive': f'{name}-source.tar.gz',
    }
    notice = (
        'DroidDeck Turnip Drivers\n\n'
        f'Corresponding Source: {name}-source.tar.gz, distributed alongside this ZIP.\n'
        f'Build source commit: {source_commit}\n'
        'Mesa license texts and upstream author notices are preserved in LICENSES/mesa/.\n'
        'Imported Banner and Droid-Deck/Drivers patch/build code is GPL-3.0; see LICENSES/.\n'
        'The patched driver is distributed under GPL-3.0 with original notices preserved.\n'
        'The source archive contains the exact patched Mesa tree and build recipe.\n'
    )
    files = {
        LIBRARY: data, 'meta.json': json.dumps(meta, indent=2).encode(),
        'freedreno_icd.aarch64.json': json.dumps({
            'file_format_version': '1.0.0',
            'ICD': {'library_path': f'./{LIBRARY}', 'api_version': api_version},
        }, indent=2).encode(),
        'build-report.json': json.dumps({'metadata': meta, 'provenance': provenance}, indent=2).encode(),
        'NOTICE': notice.encode(),
        'LICENSES/mesa/license.rst': (mesa / 'docs/license.rst').read_bytes(),
        'LICENSES/banners-GPL-3.0.txt': (ROOT / 'vendor/banners/LICENSE').read_bytes(),
        'LICENSES/drivers-GPL-3.0.txt': (ROOT / 'vendor/drivers/LICENSE').read_bytes(),
    }
    for file in (mesa / 'licenses').rglob('*'):
        if file.is_file():
            files[f'LICENSES/mesa/{file.relative_to(mesa / "licenses").as_posix()}'] = file.read_bytes()
    source = args.dist / f'{name}-source.tar.gz'
    with source.open('wb') as stream:
        subprocess.run(['git', '-C', str(mesa), 'archive', '--format=tar.gz',
                        f'--prefix={name}-source/', 'HEAD'], stdout=stream, check=True)
    archive = args.dist / f'{name}.zip'
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as stream:
        for path, content in sorted(files.items()):
            stream.writestr(path, content)
    with zipfile.ZipFile(archive) as stream:
        if stream.testzip() or hashlib.sha256(stream.read(LIBRARY)).hexdigest() != digest:
            raise ValueError('Packaged ZIP verification failed')
    for path in (archive, source):
        with path.open('rb') as stream:
            sha = hashlib.file_digest(stream, 'sha256').hexdigest()
        path.with_name(path.name + '.sha256').write_text(f'{sha}  {path.name}\n')
    summary = os.environ.get('GITHUB_STEP_SUMMARY')
    if summary:
        with open(summary, 'a', encoding='utf-8') as stream:
            stream.write(f'Package: `{archive.name}`\n\nSource: `{source_commit}`\n\n'
                         f'AArch64 glibc {glibc}, KGSL + MSM, Wayland + X11.\n\n'
                         'Download the artifact, extract it, and import the inner Linux ZIP in DroidDeck.\n')
    print(json.dumps(meta, indent=2))


if __name__ == '__main__':
    main()
