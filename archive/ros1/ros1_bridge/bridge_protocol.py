import json
import time
from typing import Any, Dict


PROTOCOL_VERSION = 1
MAX_FRAME_BYTES = 1024 * 1024
DEFAULT_WATCHDOG_MS = 500


def now_ms() -> int:
    return int(time.time() * 1000)


def clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, value))


def json_line(payload: Dict[str, Any]) -> bytes:
    return (json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
