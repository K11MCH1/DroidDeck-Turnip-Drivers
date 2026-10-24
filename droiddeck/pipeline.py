import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROVENANCE = '.droiddeck-turnip.json'


def git(mesa, *args):
    return subprocess.check_output(['git', '-C', str(mesa), *args], text=True).strip()


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')


def apply_series(mesa, paths):
    applied = []
    for relative in paths:
        path = (ROOT / relative).resolve()
        if not path.is_relative_to(ROOT) or not path.is_file():
            raise ValueError(f'Invalid patch path: {relative}')
        print(f'Applying {relative}', flush=True)
        if path.suffix == '.py':
            command = [sys.executable, str(path)]
        elif path.suffix == '.patch':
            command = ['git', 'apply', '--whitespace=nowarn', str(path)]
        else:
            raise ValueError(f'Expected .py or .patch: {relative}')
        subprocess.run(command, cwd=mesa, check=True, env={**os.environ, 'PYTHONUTF8': '1'})
        applied.append({'file': relative, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    return applied


def assert_upstream(mesa):
    text = (mesa / 'src/freedreno/vulkan/tu_knl_kgsl.cc').read_text(encoding='utf-8')
    for marker in ('kgsl_syncobj_merge(struct tu_device *device',
                   'return kgsl_poll_timestamp(device, context_id, timestamp);'):
        if marker not in text:
            raise ValueError(f'Mesa lacks Banner common fix: {marker}')


def validate_provenance(mesa):
    provenance = json.loads((mesa / PROVENANCE).read_text(encoding='utf-8'))
    if provenance['schemaVersion'] != 1 or not re.fullmatch('[0-9a-f]{40}', provenance['mesaCommit']):
        raise ValueError('Invalid stage 1 provenance')
    subprocess.run(['git', '-C', str(mesa), 'merge-base', '--is-ancestor',
                    provenance['mesaCommit'], 'HEAD'], check=True)
    if provenance['variant'] not in json.loads((ROOT / 'patches.json').read_text())['banner']:
        raise ValueError('Unknown source variant')
    return provenance


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('stage', choices=['banner', 'linux'])
    parser.add_argument('mesa', type=Path)
    parser.add_argument('--variant', default='standard')
    args = parser.parse_args()
    mesa = args.mesa.resolve()
    config = json.loads((ROOT / 'patches.json').read_text(encoding='utf-8'))
    assert_upstream(mesa)
    if args.stage == 'banner':
        if (mesa / PROVENANCE).exists():
            raise ValueError('Stage 1 requires a fresh upstream source tree')
        source = git(mesa, 'rev-parse', 'HEAD')
        (mesa / '.github/workflows/macos.yml').unlink(missing_ok=True)
        paths = config['banner'][args.variant] + config['extra']
        applied = apply_series(mesa, paths)
        data = {
            'schemaVersion': 1, 'mesaCommit': source, 'variant': args.variant,
            'mesaVersion': (mesa / 'VERSION').read_text().strip(),
            'recipeCommit': git(ROOT, 'rev-parse', 'HEAD'),
            'sources': json.loads((ROOT / 'sources.json').read_text()),
            'patches': applied,
        }
        write_json(mesa / PROVENANCE, data)
    else:
        data = validate_provenance(mesa)
        if 'linux' in data:
            raise ValueError('Select a stage 1 source, before Linux adaptation')
        data['linux'] = {
            'sourceCommit': git(mesa, 'rev-parse', 'HEAD'),
            'recipeCommit': git(ROOT, 'rev-parse', 'HEAD'),
            'patches': apply_series(mesa, config['linux']),
        }
        write_json(mesa / PROVENANCE, data)
    subprocess.run(['git', '-C', str(mesa), 'diff', '--check'], check=True)
    for file in ('src/freedreno/common/freedreno_devices.py',):
        compile((mesa / file).read_text(encoding='utf-8'), file, 'exec')
    print(json.dumps(data, indent=2))


if __name__ == '__main__':
    main()
