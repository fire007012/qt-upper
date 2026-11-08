#!/usr/bin/env python3
import argparse
import json
import threading
import time
try:
    import tkinter as tk
    from tkinter import ttk
except ImportError:  # Python 2 fallback, harmless under Noetic/Python 3.
    import Tkinter as tk
    import ttk

import rospy
from control_msgs.msg import JointJog
from geometry_msgs.msg import Twist, TwistStamped


class TopicState(object):
    def __init__(self):
        self.payload = {}
        self.count = 0
        self.last_wall_time = 0.0
        self.last_ros_stamp = None
        self.hz = 0.0

    def update(self, payload, ros_stamp=None):
        now = time.time()
        if self.last_wall_time > 0.0:
            dt = now - self.last_wall_time
            if dt > 0.0:
                instant_hz = 1.0 / dt
                self.hz = instant_hz if self.hz <= 0.0 else self.hz * 0.8 + instant_hz * 0.2
        self.payload = payload
        self.count += 1
        self.last_wall_time = now
        self.last_ros_stamp = ros_stamp


class SignalPanel(ttk.LabelFrame):
    def __init__(self, parent, title):
        ttk.LabelFrame.__init__(self, parent, text=title)
        self.status_var = tk.StringVar(value="waiting")
        self.text = tk.Text(self, height=12, width=58, wrap="none")
        self.text.configure(font=("Monospace", 10))
        status = ttk.Label(self, textvariable=self.status_var)
        status.pack(fill="x", padx=6, pady=(4, 2))
        self.text.pack(fill="both", expand=True, padx=6, pady=(0, 6))
        self.text.configure(state="disabled")

    def set_content(self, status, payload):
        self.status_var.set(status)
        body = json.dumps(payload, indent=2, sort_keys=True)
        self.text.configure(state="normal")
        self.text.delete("1.0", "end")
        self.text.insert("1.0", body)
        self.text.configure(state="disabled")


class BridgeControlSignalMonitorGui(object):
    def __init__(self, root, args):
        self.root = root
        self.args = args
        self.lock = threading.Lock()
        self.states = {
            "cmd_vel": TopicState(),
            "servo": TopicState(),
            "flipper": TopicState(),
        }

        root.title(args.window_title)
        root.geometry(args.geometry)
        root.protocol("WM_DELETE_WINDOW", self.on_close)

        self.summary_var = tk.StringVar(value="starting")
        ttk.Label(root, textvariable=self.summary_var).pack(fill="x", padx=8, pady=(6, 2))

        container = ttk.Frame(root)
        container.pack(fill="both", expand=True, padx=8, pady=6)
        container.columnconfigure(0, weight=1)
        container.columnconfigure(1, weight=1)
        container.columnconfigure(2, weight=1)
        container.rowconfigure(0, weight=1)

        self.panels = {
            "cmd_vel": SignalPanel(container, args.cmd_vel_topic),
            "servo": SignalPanel(container, args.servo_topic),
            "flipper": SignalPanel(container, args.flipper_topic),
        }
        self.panels["cmd_vel"].grid(row=0, column=0, sticky="nsew", padx=(0, 4))
        self.panels["servo"].grid(row=0, column=1, sticky="nsew", padx=4)
        self.panels["flipper"].grid(row=0, column=2, sticky="nsew", padx=(4, 0))

        self.cmd_vel_sub = rospy.Subscriber(
            args.cmd_vel_topic, Twist, self.cmd_vel_cb, queue_size=args.queue_size
        )
        self.servo_sub = rospy.Subscriber(
            args.servo_topic, TwistStamped, self.servo_cb, queue_size=args.queue_size
        )
        self.flipper_sub = rospy.Subscriber(
            args.flipper_topic, JointJog, self.flipper_cb, queue_size=args.queue_size
        )

        self.refresh()

    def on_close(self):
        rospy.signal_shutdown("ui closed")
        self.root.destroy()

    def update_state(self, name, payload, ros_stamp=None):
        with self.lock:
            self.states[name].update(payload, ros_stamp)

    def cmd_vel_cb(self, msg):
        self.update_state(
            "cmd_vel",
            {
                "linear": {"x": msg.linear.x, "y": msg.linear.y, "z": msg.linear.z},
                "angular": {"x": msg.angular.x, "y": msg.angular.y, "z": msg.angular.z},
            },
        )

    def servo_cb(self, msg):
        self.update_state(
            "servo",
            {
                "header": {
                    "seq": msg.header.seq,
                    "stamp": msg.header.stamp.to_sec(),
                    "frame_id": msg.header.frame_id,
                },
                "twist": {
                    "linear": {
                        "x": msg.twist.linear.x,
                        "y": msg.twist.linear.y,
                        "z": msg.twist.linear.z,
                    },
                    "angular": {
                        "x": msg.twist.angular.x,
                        "y": msg.twist.angular.y,
                        "z": msg.twist.angular.z,
                    },
                },
            },
            msg.header.stamp.to_sec(),
        )

    def flipper_cb(self, msg):
        self.update_state(
            "flipper",
            {
                "header": {
                    "seq": msg.header.seq,
                    "stamp": msg.header.stamp.to_sec(),
                    "frame_id": msg.header.frame_id,
                },
                "joint_names": list(msg.joint_names),
                "velocities": list(msg.velocities),
                "duration": msg.duration,
            },
            msg.header.stamp.to_sec(),
        )

    def refresh(self):
        now = time.time()
        with self.lock:
            snapshots = {
                name: {
                    "payload": dict(state.payload),
                    "count": state.count,
                    "last_wall_time": state.last_wall_time,
                    "last_ros_stamp": state.last_ros_stamp,
                    "hz": state.hz,
                }
                for name, state in self.states.items()
            }

        active = 0
        for name, snapshot in snapshots.items():
            age = now - snapshot["last_wall_time"] if snapshot["last_wall_time"] else None
            if age is None:
                status = "waiting | count=0 | hz=0.0"
                payload = {}
            else:
                active += 1
                status = "age=%.3fs | count=%d | hz=%.1f" % (
                    age,
                    snapshot["count"],
                    snapshot["hz"],
                )
                if snapshot["last_ros_stamp"] is not None:
                    status += " | msg_stamp=%.6f" % snapshot["last_ros_stamp"]
                payload = snapshot["payload"]
            self.panels[name].set_content(status, payload)

        self.summary_var.set(
            "ROS node: %s | active topics: %d/3 | refresh: %d ms"
            % (rospy.get_name(), active, self.args.refresh_ms)
        )
        if not rospy.is_shutdown():
            self.root.after(self.args.refresh_ms, self.refresh)


def build_arg_parser():
    parser = argparse.ArgumentParser(
        description="Tk UI for raw host_bridge_node control signals."
    )
    parser.add_argument("--cmd-vel-topic", default="/cmd_vel")
    parser.add_argument("--servo-topic", default="/servo_server/delta_twist_cmds")
    parser.add_argument("--flipper-topic", default="/flipper_control/jog_cmd")
    parser.add_argument("--queue-size", type=int, default=20)
    parser.add_argument("--refresh-ms", type=int, default=100)
    parser.add_argument("--geometry", default="1320x560")
    parser.add_argument("--window-title", default="Bridge Control Signal Monitor")
    return parser


def main():
    args = build_arg_parser().parse_args(rospy.myargv()[1:])
    rospy.init_node("bridge_control_signal_monitor_gui", anonymous=False)
    root = tk.Tk()
    BridgeControlSignalMonitorGui(root, args)
    root.mainloop()


if __name__ == "__main__":
    main()
