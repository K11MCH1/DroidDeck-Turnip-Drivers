import sys
from pathlib import Path

from mesa_edit import replace

DEVICES = 'src/freedreno/common/freedreno_devices.py'

A810_PROPS = (
    '[a7xx_base, a7xx_gen3, a8xx_base, a8xx_gen1, GPUProps(\n'
    '            gmem_vpc_attr_buf_size = 16384,\n'
    '            gmem_vpc_pos_buf_size = 12288,\n'
    '            gmem_vpc_bv_pos_buf_size = 20480,\n'
    '            # This is possibly also needed for a830 (and all of a8xx),\n'
    '            # move to a8xx_base if confirmed needed for a830.\n'
    '            has_fs_tex_prefetch = False,\n'
)
A829_PROPS = (
    '[a7xx_base, a7xx_gen3, a8xx_base, a8xx_gen2,\n'
    '         GPUProps(\n'
    '            shading_rate_matches_vk = True,  # TODO confirm this\n'
    '            sysmem_vpc_bv_pos_buf_size = 24576,\n'
)
A810_END = (
    '        raw_magic_regs = a8xx_base_raw_magic_regs,\n'
    '    ))\n'
)
A825 = '''
add_gpus([
        GPUId(chip_id=0x44030000, name="Adreno (TM) 825"),
    ], A6xxGPUInfo(
        CHIP.A8XX,
        [a7xx_base, a7xx_gen3, a8xx_base, a8xx_gen1, GPUProps(
            sysmem_ccu_depth_cache_fraction = CCUColorCacheFraction.THREE_QUARTER.value,
            sysmem_per_ccu_depth_cache_size = 96 * 1024,
        )],
        num_ccu = 4,
        num_slices = 2,
        tile_align_w = 96,
        tile_align_h = 32,
        tile_max_w = 16416,
        tile_max_h = 16384,
        num_vsc_pipes = 32,
        cs_shared_mem_size = 32 * 1024,
        wave_granularity = 2,
        fibers_per_sp = 128 * 2 * 16,
        raw_magic_regs = a8xx_base_raw_magic_regs,
    ))
'''

replace(
    DEVICES,
    '       GPUId(chip_id=0xffff44010000, name="Adreno (TM) 810"),\n',
    '       GPUId(chip_id=0x44010000, name="Adreno (TM) 810"),\n'
    '       GPUId(chip_id=0xffff44010000, name="Adreno (TM) 810"),\n',
    'A810 speedbin chip_id 0x44010000',
)
replace(DEVICES, A810_PROPS, A810_PROPS + '            disable_gmem = True,\n', 'A810 disable_gmem')
replace(
    DEVICES,
    '        GPUId(chip_id=0x44030a20, name="Adreno (TM) 829"), # KGSL\n',
    '        GPUId(chip_id=0x44030A00, name="Adreno (TM) 829"),\n'
    '        GPUId(chip_id=0xffff44030A00, name="Adreno (TM) 829"),\n'
    '        GPUId(chip_id=0x44030a20, name="Adreno (TM) 829"), # KGSL\n',
    'A829 KGSL chip_ids 0x44030A00 / 0xffff44030A00',
)
replace(DEVICES, A829_PROPS, A829_PROPS + '            disable_gmem = True,\n', 'A829 disable_gmem')
replace(
    DEVICES,
    '        GPUId(chip_id=0xffff44050A31, name="Adreno (TM) 840"),\n',
    '        GPUId(chip_id=0xffff44050A31, name="Adreno (TM) 840"),\n'
    '        GPUId(chip_id=0xffff44050A21, name="Adreno (TM) 840v2"),\n'
    '        GPUId(chip_id=0x44050A21, name="Adreno (TM) 840v2"), # KGSL\n',
    'A840v2 chip_ids 0x44050A21 / 0xffff44050A21',
)

content = Path(DEVICES).read_text()
if 'name="Adreno (TM) 825"' in content or 'name="FD825"' in content:
    print(f'{DEVICES}: A825 already present upstream')
else:
    end = content.find(A810_END, content.find('GPUId(chip_id=0xffff44010000'))
    if end == -1:
        sys.exit(f'{DEVICES}: end of the A810 entry not found')
    end += len(A810_END)
    content = content[:end] + A825 + content[end:]
    compile(content, DEVICES, 'exec')
    Path(DEVICES).write_text(content)
    print(f'{DEVICES}: A825 entry')
