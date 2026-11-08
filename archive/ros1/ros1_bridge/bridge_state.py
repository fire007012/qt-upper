from dataclasses import dataclass, field
from typing import Dict, List, Optional


@dataclass
class TwistCommand:
    linear_x: float = 0.0
    angular_z: float = 0.0
    source: str = "idle"

    def to_dict(self) -> Dict[str, float]:
        return {
            "linear_x": self.linear_x,
            "angular_z": self.angular_z,
            "source": self.source,
        }


@dataclass
class ServoCommand:
    linear_x: float = 0.0
    linear_y: float = 0.0
    linear_z: float = 0.0
    angular_x: float = 0.0
    angular_y: float = 0.0
    angular_z: float = 0.0
    frame_id: str = "catch_camera"
    source: str = "idle"

    def to_dict(self) -> Dict[str, object]:
        return {
            "linear_x": self.linear_x,
            "linear_y": self.linear_y,
            "linear_z": self.linear_z,
            "angular_x": self.angular_x,
            "angular_y": self.angular_y,
            "angular_z": self.angular_z,
            "frame_id": self.frame_id,
            "source": self.source,
        }


@dataclass
class GripperCommand:
    position: float = 0.022
    source: str = "idle"

    def to_dict(self) -> Dict[str, object]:
        return {
            "position": self.position,
            "source": self.source,
        }


@dataclass
class FlipperCommand:
    joint_names: List[str] = field(default_factory=list)
    velocities: List[float] = field(default_factory=list)
    duration: float = 0.15
    source: str = "idle"

    def to_dict(self) -> Dict[str, object]:
        return {
            "joint_names": list(self.joint_names),
            "velocities": list(self.velocities),
            "duration": self.duration,
            "source": self.source,
        }


@dataclass
class BridgeState:
    emergency_active: bool = False
    emergency_source: str = ""
    control_mode: str = "vehicle"
    speed_level: int = 2
    motor_initialized: bool = True
    motor_enabled: bool = True
    last_error_code: int = 0
    last_error_message: str = ""
    last_operator_seq: int = 0
    last_valid_input_ms: int = 0
    watchdog_active: bool = False
    last_pressed_keys: List[str] = field(default_factory=list)
    last_gamepad_buttons: List[str] = field(default_factory=list)
    last_twist: TwistCommand = field(default_factory=TwistCommand)
    last_servo: ServoCommand = field(default_factory=ServoCommand)
    gripper_target: float = 0.022
    last_gripper: Optional[GripperCommand] = None
    last_flipper: FlipperCommand = field(default_factory=FlipperCommand)
    flipper_profile_target: str = "csv_velocity"
    flipper_profile_result: str = "pending"
    flipper_profile_message: str = ""

    def to_dict(self) -> Dict[str, object]:
        return {
            "emergency_active": self.emergency_active,
            "emergency_source": self.emergency_source,
            "control_mode": self.control_mode,
            "speed_level": self.speed_level,
            "motor_initialized": self.motor_initialized,
            "motor_enabled": self.motor_enabled,
            "last_error_code": self.last_error_code,
            "last_error_message": self.last_error_message,
            "last_operator_seq": self.last_operator_seq,
            "last_valid_input_ms": self.last_valid_input_ms,
            "watchdog_active": self.watchdog_active,
            "last_pressed_keys": list(self.last_pressed_keys),
            "last_gamepad_buttons": list(self.last_gamepad_buttons),
            "last_twist": self.last_twist.to_dict(),
            "last_servo": self.last_servo.to_dict(),
            "gripper_target": self.gripper_target,
            "last_gripper": self.last_gripper.to_dict() if self.last_gripper else None,
            "last_flipper": self.last_flipper.to_dict(),
            "flipper_profile_target": self.flipper_profile_target,
            "flipper_profile_result": self.flipper_profile_result,
            "flipper_profile_message": self.flipper_profile_message,
        }
