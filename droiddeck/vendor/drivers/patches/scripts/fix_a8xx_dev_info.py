import sys
from pathlib import Path

from mesa_edit import replace

TU_CMD = 'src/freedreno/vulkan/tu_cmd_buffer.cc'
ANCHOR = "   /* can't fit attachments into gmem */"

replace(
    'src/freedreno/common/freedreno_dev_info.h',
    '      bool has_image_processing;\n',
    '      bool has_image_processing;\n      bool disable_gmem;\n',
    'per-GPU disable_gmem property',
)

text = Path(TU_CMD).read_text()
field = next((f for f in ('force_render_mode_reason', 'gmem_disable_reason') if f'cmd->state.rp.{f}' in text), None)
if field is None:
    sys.exit(f'{TU_CMD}: no known render-mode reason field')

replace(
    TU_CMD,
    ANCHOR,
    f"""   if (cmd->device->physical_device->dev_info.props.disable_gmem) {{
      cmd->state.rp.{field} = "Unsupported GPU";
      return true;
   }}

{ANCHOR}""",
    'sysmem fallback for GPUs with disable_gmem',
)
