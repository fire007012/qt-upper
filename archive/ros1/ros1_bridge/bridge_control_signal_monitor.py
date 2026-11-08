#!/usr/bin/env python3
import argparse
import json
import threading

import rospy
from control_msgs.msg import JointJog
from geometry_msgs.msg import Twist, TwistStamped


class BridgeControlSignalMonitor(object):
    def __init__(self, args):
        self.lock = threading.Lock()
        self.seq = 0
        self.compact = args.compact
        self.include_header = args.include_header

        self.cmd_vel_sub = rospy.Subscriber(
            args.cmd_vel_topic, Twist, self.cmd_vel_cb, queue_size=args.queue_size
        )
        self.servo_sub = rospy.Subscriber(
            args.servo_topic, TwistStamped, self.servo_cb, queue_size=args.queue_size
        )
        self.flipper_sub = rospy.Subscriber(
            args.flipper_topic, JointJog, self.flipper_cb, queue_size=args.queue_size
        )

        rospy.loginfo("bridge_control_signal_monitor subscribed cmd_vel=%s", args.cmd_vel_topic)
        rospy.loginfo("bridge_control_signal_monitor subscribed servo=%s", args.servo_topic)
        rospy.loginfo("bridge_control_signal_monitor subscribed flipper=%s", args.flipper_topic)

    def next_prefix(self, name):
        with self.lock:
            self.seq += 1
            seq = self.seq
        stamp = rospy.Time.now().to_sec()
        return "[%06d %.6f] %s" % (seq, stamp, name)

    def print_payload(self, name, payload):
        prefix = self.next_prefix(name)
        if self.compact:
            print("%s %s" % (prefix, json.dumps(payload, sort_keys=True)), flush=True)
            return

        print(prefix, flush=True)
        print(json.dumps(payload, indent=2, sort_keys=True), flush=True)

    def cmd_vel_cb(self, msg):
        self.print_payload(
            "cmd_vel",
            {
                "linear": {
                    "x": msg.linear.x,
                    "y": msg.linear.y,
                    "z": msg.linear.z,
                },
                "angular": {
                    "x": msg.angular.x,
                    "y": msg.angular.y,
                    "z": msg.angular.z,
                },
            },
        )

    def servo_cb(self, msg):
        payload = {
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
        }
        if self.include_header:
            payload["header"] = {
                "seq": msg.header.seq,
                "stamp": msg.header.stamp.to_sec(),
                "frame_id": msg.header.frame_id,
            }
        self.print_payload("servo", payload)

    def flipper_cb(self, msg):
        payload = {
            "joint_names": list(msg.joint_names),
            "velocities": list(msg.velocities),
            "duration": msg.duration,
        }
        if self.include_header:
            payload["header"] = {
                "seq": msg.header.seq,
                "stamp": msg.header.stamp.to_sec(),
                "frame_id": msg.header.frame_id,
            }
        self.print_payload("flipper", payload)


def build_arg_parser():
    parser = argparse.ArgumentParser(
        description="Print raw control signals published by host_bridge_node."
    )
    parser.add_argument("--cmd-vel-topic", default="/cmd_vel")
    parser.add_argument("--servo-topic", default="/servo_server/delta_twist_cmds")
    parser.add_argument("--flipper-topic", default="/flipper_control/jog_cmd")
    parser.add_argument("--queue-size", type=int, default=20)
    parser.add_argument(
        "--compact",
        action="store_true",
        help="print each message on one line as JSON",
    )
    parser.add_argument(
        "--include-header",
        action="store_true",
        help="include ROS Header fields for stamped messages",
    )
    return parser


def main():
    args = build_arg_parser().parse_args(rospy.myargv()[1:])
    rospy.init_node("bridge_control_signal_monitor", anonymous=False)
    BridgeControlSignalMonitor(args)
    rospy.spin()


if __name__ == "__main__":
    main()
