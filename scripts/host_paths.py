"""Output boundaries shared by host release/validation tools."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def external_output(path):
    """Resolve links before any mkdir/tool call; artifacts stay outside source."""
    result = Path(path).expanduser().resolve()
    if result == ROOT or ROOT in result.parents or result in ROOT.parents:
        raise ValueError('build/release/evidence output must be outside the source tree and its ancestors')
    reference = (ROOT / 'ros2_ws_car').resolve()
    if result == reference or reference in result.parents or result in reference.parents:
        raise ValueError('output intersects the read-only reference tree')
    return result
