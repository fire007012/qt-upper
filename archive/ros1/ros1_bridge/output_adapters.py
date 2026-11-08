import sys
from typing import Any, Dict, List, Optional, Tuple

from bridge_state import FlipperCommand, GripperCommand, ServoCommand, TwistCommand
from debug_events import EventSink, NullEventSink


class OutputAdapter:
    def __init__(self, events: Optional[EventSink] = None) -> None:
        self.events = events or NullEventSink()

    def publish_twist(self, twist: TwistCommand) -> None:
        raise NotImplementedError

    def publish_servo(self, servo: ServoCommand) -> None:
        raise NotImplementedError

    def publish_gripper(self, gripper: GripperCommand) -> None:
        raise NotImplementedError

    def publish_flipper(self, flipper: FlipperCommand) -> None:
        raise NotImplementedError

    def call_flipper_profile(self, profile: str) -> Tuple[bool, int, str, Dict[str, Any]]:
        return False, 2302, "no flipper profile service configured", {}

    def call_lifecycle(self, command: str) -> Tuple[bool, int, str]:
        raise NotImplementedError

    def lifecycle_service_name(self, command: str) -> str:
        return f"lifecycle/{command}"

    def command_service_name(self, command: str, params: Optional[Dict[str, Any]] = None) -> str:
        return ""

    def call_command_service(self, command: str, params: Optional[Dict[str, Any]] = None) -> Tuple[bool, int, str]:
        return False, 2302, f"no ROS service configured for {command}"

    def list_arm_named_targets(self) -> Tuple[bool, int, str, List[Dict[str, str]]]:
        return False, 2302, "no MoveIt arm group configured", []

    def move_arm_named_target(self, target: str) -> Tuple[bool, int, str]:
        return False, 2302, f"no MoveIt arm group configured for target {target}"


class DryRunOutput(OutputAdapter):
    def __init__(
        self,
        events: Optional[EventSink] = None,
        service_commands: Optional[Dict[str, str]] = None,
    ) -> None:
        super().__init__(events)
        self.service_commands = service_commands or {}

    def publish_twist(self, twist: TwistCommand) -> None:
        print(
            f"[bridge] cmd_vel source={twist.source} "
            f"linear_x={twist.linear_x:+.3f} angular_z={twist.angular_z:+.3f}",
            flush=True,
        )
        self.events.emit("output", "dry-run cmd_vel", data={
            "linear_x": twist.linear_x,
            "angular_z": twist.angular_z,
            "source": twist.source,
        })

    def publish_servo(self, servo: ServoCommand) -> None:
        print(
            f"[bridge] servo source={servo.source} frame={servo.frame_id} "
            f"lin=({servo.linear_x:+.3f},{servo.linear_y:+.3f},{servo.linear_z:+.3f}) "
            f"ang=({servo.angular_x:+.3f},{servo.angular_y:+.3f},{servo.angular_z:+.3f})",
            flush=True,
        )
        self.events.emit("output", "dry-run servo", data=servo.to_dict())

    def publish_gripper(self, gripper: GripperCommand) -> None:
        print(
            f"[bridge] gripper source={gripper.source} position={gripper.position:.4f}",
            flush=True,
        )
        self.events.emit("output", "dry-run gripper", data=gripper.to_dict())

    def publish_flipper(self, flipper: FlipperCommand) -> None:
        pairs = ", ".join(
            f"{name}={velocity:+.3f}"
            for name, velocity in zip(flipper.joint_names, flipper.velocities)
        )
        print(
            f"[bridge] flipper source={flipper.source} duration={flipper.duration:.3f} {pairs}",
            flush=True,
        )
        self.events.emit("output", "dry-run flipper", data=flipper.to_dict())

    def call_flipper_profile(self, profile: str) -> Tuple[bool, int, str, Dict[str, Any]]:
        print(f"[bridge] flipper profile dry-run profile={profile}", flush=True)
        detail = {"active_profile": profile, "active_controller": "dry-run"}
        self.events.emit("flipper", "dry-run flipper profile service", data=detail)
        return True, 0, f"dry-run flipper profile {profile} accepted", detail

    def call_lifecycle(self, command: str) -> Tuple[bool, int, str]:
        print(f"[bridge] lifecycle dry-run command={command}", flush=True)
        self.events.emit("lifecycle", "dry-run lifecycle service", data={"command": command})
        return True, 0, f"dry-run lifecycle {command} accepted"

    def lifecycle_service_name(self, command: str) -> str:
        return f"dry-run:lifecycle/{command}"

    def command_service_name(self, command: str, params: Optional[Dict[str, Any]] = None) -> str:
        template = self.service_commands.get(command, "")
        if not template:
            return ""
        return format_service_name(template, command, params)

    def call_command_service(self, command: str, params: Optional[Dict[str, Any]] = None) -> Tuple[bool, int, str]:
        service_name = self.command_service_name(command, params)
        print(f"[bridge] service dry-run command={command} service={service_name}", flush=True)
        self.events.emit("service", "dry-run command service", data={
            "command": command,
            "service": service_name,
            "params": params or {},
        })
        return True, 0, f"dry-run service {service_name} accepted"

    def list_arm_named_targets(self) -> Tuple[bool, int, str, List[Dict[str, str]]]:
        targets = [
            {"name": "home", "description": "dry-run"},
            {"name": "ready", "description": "dry-run"},
            {"name": "inspect", "description": "dry-run"},
        ]
        self.events.emit("moveit", "dry-run named targets listed", data={"count": len(targets)})
        return True, 0, "dry-run MoveIt named targets", targets

    def move_arm_named_target(self, target: str) -> Tuple[bool, int, str]:
        print(f"[bridge] moveit dry-run target={target}", flush=True)
        self.events.emit("moveit", "dry-run named target accepted", data={"target": target})
        return True, 0, f"dry-run MoveIt target {target} accepted"

class RosOutput(OutputAdapter):
    def __init__(
        self,
        node_name: str,
        cmd_vel_topic: str,
        servo_topic: str,
        gripper_position_topic: str,
        flipper_jog_topic: str,
        flipper_profile_service: str,
        hybrid_service_ns: str,
        service_commands: Optional[Dict[str, str]] = None,
        moveit_group: str = "manipulator",
        events: Optional[EventSink] = None,
    ) -> None:
        super().__init__(events)
        import rospy
        from control_msgs.msg import JointJog
        from flipper_control.srv import SetControlProfile, SetControlProfileRequest
        from geometry_msgs.msg import Twist, TwistStamped
        from std_msgs.msg import Float64
        from std_srvs.srv import Trigger

        self._twist_type = Twist
        self._twist_stamped_type = TwistStamped
        self._float64_type = Float64
        self._joint_jog_type = JointJog
        self._set_control_profile_type = SetControlProfile
        self._set_control_profile_request_type = SetControlProfileRequest
        self._trigger_type = Trigger
        self._rospy = rospy
        self.hybrid_service_ns = hybrid_service_ns.rstrip("/") or "/hybrid_motor_hw_node"
        self.flipper_profile_service = flipper_profile_service
        self.service_commands = service_commands or {}
        self.moveit_group_name = moveit_group
        self._move_group = None
        self._moveit_error = ""
        self._moveit_commander = None
        self._moveit_roscpp_initialized = False
        rospy.init_node(node_name, anonymous=False)
        self._cmd_vel_pub = rospy.Publisher(cmd_vel_topic, Twist, queue_size=1)
        self._servo_pub = rospy.Publisher(servo_topic, TwistStamped, queue_size=1)
        self._gripper_pub = rospy.Publisher(gripper_position_topic, Float64, queue_size=1)
        self._flipper_pub = rospy.Publisher(flipper_jog_topic, JointJog, queue_size=1)
        self._flipper_profile_client = rospy.ServiceProxy(
            flipper_profile_service, SetControlProfile
        )
        rospy.loginfo("host_bridge_node publishing Twist to %s", cmd_vel_topic)
        rospy.loginfo("host_bridge_node publishing TwistStamped to %s", servo_topic)
        rospy.loginfo("host_bridge_node publishing Float64 to %s", gripper_position_topic)
        rospy.loginfo("host_bridge_node publishing JointJog to %s", flipper_jog_topic)
        rospy.loginfo("host_bridge_node using flipper profile service %s", flipper_profile_service)
        rospy.loginfo("host_bridge_node using hybrid service namespace %s", self.hybrid_service_ns)
        self._init_moveit_group(moveit_group)

    def publish_twist(self, twist: TwistCommand) -> None:
        msg = self._twist_type()
        msg.linear.x = twist.linear_x
        msg.angular.z = twist.angular_z
        self._cmd_vel_pub.publish(msg)
        self.events.emit("output", "ros cmd_vel published", data={
            "linear_x": twist.linear_x,
            "angular_z": twist.angular_z,
            "source": twist.source,
        })

    def publish_servo(self, servo: ServoCommand) -> None:
        msg = self._twist_stamped_type()
        msg.header.stamp = self._rospy.Time.now()
        msg.header.frame_id = servo.frame_id
        msg.twist.linear.x = servo.linear_x
        msg.twist.linear.y = servo.linear_y
        msg.twist.linear.z = servo.linear_z
        msg.twist.angular.x = servo.angular_x
        msg.twist.angular.y = servo.angular_y
        msg.twist.angular.z = servo.angular_z
        self._servo_pub.publish(msg)
        self.events.emit("output", "ros servo published", data=servo.to_dict())

    def publish_gripper(self, gripper: GripperCommand) -> None:
        msg = self._float64_type(data=gripper.position)
        self._gripper_pub.publish(msg)
        self.events.emit("output", "ros gripper published", data=gripper.to_dict())

    def publish_flipper(self, flipper: FlipperCommand) -> None:
        msg = self._joint_jog_type()
        msg.header.stamp = self._rospy.Time.now()
        msg.joint_names = list(flipper.joint_names)
        msg.velocities = list(flipper.velocities)
        msg.duration = flipper.duration
        self._flipper_pub.publish(msg)
        self.events.emit("output", "ros flipper published", data=flipper.to_dict())

    def call_flipper_profile(self, profile: str) -> Tuple[bool, int, str, Dict[str, Any]]:
        try:
            self._rospy.wait_for_service(self.flipper_profile_service, timeout=0.1)
            request = self._set_control_profile_request_type(profile=profile)
            response = self._flipper_profile_client(request)
        except Exception as exc:
            self.events.emit("flipper", "flipper profile service failed", level="warning", data={
                "profile": profile,
                "service": self.flipper_profile_service,
                "error": str(exc),
            })
            return False, 2301, str(exc), {"service_unavailable": True}

        detail = {
            "profile": profile,
            "service": self.flipper_profile_service,
            "active_profile": getattr(response, "active_profile", ""),
            "active_controller": getattr(response, "active_controller", ""),
        }
        ok = bool(response.success)
        message = str(response.message)
        self.events.emit("flipper", "flipper profile service completed", data={
            **detail,
            "success": ok,
            "message": message,
        })
        return ok, 0 if ok else 2301, message, detail

    def call_lifecycle(self, command: str) -> Tuple[bool, int, str]:
        service_name = f"{self.hybrid_service_ns}/{command}"
        try:
            self._rospy.wait_for_service(service_name, timeout=2.0)
            response = self._rospy.ServiceProxy(service_name, self._trigger_type)()
        except Exception as exc:
            self.events.emit("lifecycle", "lifecycle service failed", level="error", data={
                "command": command,
                "service": service_name,
                "error": str(exc),
            })
            return False, 2301, f"{service_name} failed: {exc}"

        self.events.emit("lifecycle", "lifecycle service completed", data={
            "command": command,
            "service": service_name,
            "success": bool(response.success),
            "message": str(response.message),
        })
        return bool(response.success), 0 if response.success else 2301, str(response.message)

    def lifecycle_service_name(self, command: str) -> str:
        return f"{self.hybrid_service_ns}/{command}"

    def command_service_name(self, command: str, params: Optional[Dict[str, Any]] = None) -> str:
        template = self.service_commands.get(command, "")
        if not template:
            return ""
        return format_service_name(template, command, params)

    def call_command_service(self, command: str, params: Optional[Dict[str, Any]] = None) -> Tuple[bool, int, str]:
        service_name = self.command_service_name(command, params)
        try:
            self._rospy.wait_for_service(service_name, timeout=2.0)
            response = self._rospy.ServiceProxy(service_name, self._trigger_type)()
        except Exception as exc:
            self.events.emit("service", "command service failed", level="error", data={
                "command": command,
                "service": service_name,
                "params": params or {},
                "error": str(exc),
            })
            return False, 2301, f"{service_name} failed: {exc}"

        self.events.emit("service", "command service completed", data={
            "command": command,
            "service": service_name,
            "params": params or {},
            "success": bool(response.success),
            "message": str(response.message),
        })
        return bool(response.success), 0 if response.success else 2301, str(response.message)

    def _init_moveit_group(self, moveit_group: str) -> None:
        if not moveit_group:
            self._moveit_error = "MoveIt group is empty"
            self.events.emit("moveit", "moveit group disabled", level="warning")
            return
        try:
            if self._moveit_commander is None:
                import moveit_commander
                self._moveit_commander = moveit_commander
            if not self._moveit_roscpp_initialized:
                self._moveit_commander.roscpp_initialize(sys.argv)
                self._moveit_roscpp_initialized = True
            self._move_group = self._moveit_commander.MoveGroupCommander(moveit_group)
        except Exception as exc:
            self._move_group = None
            self._moveit_error = str(exc)
            self.events.emit("moveit", "moveit group unavailable", level="warning", data={
                "group": moveit_group,
                "error": self._moveit_error,
            })
            return

        self.events.emit("moveit", "moveit group ready", data={"group": moveit_group})
        self._rospy.loginfo("host_bridge_node using MoveIt group %s", moveit_group)

    def _ensure_moveit_group(self) -> bool:
        if self._move_group is not None:
            return True
        self._init_moveit_group(self.moveit_group_name)
        return self._move_group is not None

    def list_arm_named_targets(self) -> Tuple[bool, int, str, List[Dict[str, str]]]:
        if not self._ensure_moveit_group():
            return False, 2303, f"MoveIt unavailable: {self._moveit_error}", []
        try:
            names = list(self._move_group.get_named_targets())
        except Exception as exc:
            self.events.emit("moveit", "list named targets failed", level="error", data={
                "group": self.moveit_group_name,
                "error": str(exc),
            })
            return False, 2304, str(exc), []

        targets = [{"name": str(name), "description": self.moveit_group_name} for name in names]
        self.events.emit("moveit", "named targets listed", data={
            "group": self.moveit_group_name,
            "count": len(targets),
        })
        return True, 0, f"{len(targets)} MoveIt named targets", targets

    def move_arm_named_target(self, target: str) -> Tuple[bool, int, str]:
        if not self._ensure_moveit_group():
            return False, 2303, f"MoveIt unavailable: {self._moveit_error}"
        if not target:
            return False, 2202, "missing MoveIt target"
        try:
            named_targets = set(self._move_group.get_named_targets())
            if target not in named_targets:
                return False, 2203, f"unknown MoveIt target: {target}"
            self._move_group.set_named_target(target)
            ok = bool(self._move_group.go(wait=True))
            self._move_group.stop()
        except Exception as exc:
            self.events.emit("moveit", "move named target failed", level="error", data={
                "group": self.moveit_group_name,
                "target": target,
                "error": str(exc),
            })
            return False, 2305, str(exc)

        message = f"MoveIt target {target} reached" if ok else f"MoveIt target {target} failed"
        self.events.emit("moveit", "move named target completed", data={
            "group": self.moveit_group_name,
            "target": target,
            "ok": ok,
        }, level="info" if ok else "error")
        return ok, 0 if ok else 2306, message


def format_service_name(template: str, command: str, params: Optional[Dict[str, Any]] = None) -> str:
    params = params or {}
    mode = str(params.get("mode", ""))
    source = str(params.get("source", ""))
    target = str(params.get("target", ""))
    return template.format(command=command, mode=mode, source=source, target=target)
