from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional
import threading

from bridge_protocol import now_ms


@dataclass
class DebugEvent:
    id: int
    timestamp_ms: int
    level: str
    category: str
    message: str
    data: Dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, Any]:
        return {
            "id": self.id,
            "timestamp_ms": self.timestamp_ms,
            "level": self.level,
            "category": self.category,
            "message": self.message,
            "data": self.data,
        }


class EventSink:
    def emit(self, category: str, message: str, level: str = "info",
             data: Optional[Dict[str, Any]] = None) -> None:
        raise NotImplementedError


class NullEventSink(EventSink):
    def emit(self, category: str, message: str, level: str = "info",
             data: Optional[Dict[str, Any]] = None) -> None:
        return


class RingBufferEventSink(EventSink):
    def __init__(self, capacity: int = 500) -> None:
        self.capacity = capacity
        self._events: List[DebugEvent] = []
        self._next_id = 1
        self._lock = threading.Lock()

    def emit(self, category: str, message: str, level: str = "info",
             data: Optional[Dict[str, Any]] = None) -> None:
        with self._lock:
            event = DebugEvent(
                id=self._next_id,
                timestamp_ms=now_ms(),
                level=level,
                category=category,
                message=message,
                data=data or {},
            )
            self._next_id += 1
            self._events.append(event)
            if len(self._events) > self.capacity:
                self._events = self._events[-self.capacity:]

    def recent(self, limit: int = 100) -> List[Dict[str, Any]]:
        limit = max(0, min(limit, self.capacity))
        with self._lock:
            return [event.to_dict() for event in self._events[-limit:]]
