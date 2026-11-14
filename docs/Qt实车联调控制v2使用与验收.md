# Qt 实车联调控制 v2 使用与验收

日期：2026-11-13。上位机构建：`host-ros2-v2-field-1`；目标机器人构建：`robot24-field-control-2`。接口依据：[文档52](52_实车联调修复与上位机协作提示词.md)。本轮仅实现上位机和独立 mock；未连接机器人、CAN 或 ROS，未执行机器人部署、参数基线导入或设备身份登记。

## 修改文件

- `src/communication/ControlSession.{h,cpp}`：可选能力协商、换档顺序状态机、严格参数校验、实际档位读回、分步解除门槛、本地急停意图跨重连保持、服务端刷新周期、EPOCH 拒绝后的预设流程重新规划。接受或结果未知的操作始终查询原操作；不会自动创建新请求重复执行。
- `src/communication/ControlProtocol.{h,cpp}`：新增业务命令、构建标识；保留基础能力门槛和连续输入的按键白名单。
- `src/communication/RobotTcpClient.cpp`：沿用控制与管理连接架构；传入服务端速度声明、启动换档流程、控制连接启用 TCP_NODELAY。
- `src/communication/EmergencyChannel.cpp`：独立急停带 seq、启用 TCP_NODELAY；仍不等待普通操作或操作授权。
- `src/management/RuntimeManagementWidget.{h,cpp}`、`mainwindow.cpp`：档位选择、实际档位和各档原始服务端速度数值、全局禁止与清理结果、分步解除及三类物理证据展示。`invalidated:true` 的 succeeded 记录不再单独作为失败记录输出。
- `src/controller/KeyboardController.h`：连续键盘采样约 30 Hz；TTL 保持 300 ms。
- `tests/communication/{control_session_test,control_transport_test,motion_presets_test,management_widget_test}.cpp`、`control_peer.h`：独立协议、回环 TCP、offscreen 界面回归。
- `deploy/compatibility.json`：新增可选能力及现场验收边界。

## 操作步骤

1. 按现有流程连接管理服务/控制桥。控制握手保留基础能力和三组预设协商，额外声明 `speed_level_v1`、`latched_global_stop_v1`。缺少换档能力时按钮禁用；缺少独立急停能力时显示机器人软件需更新，并禁用新解除流程。独立急停请求仍可发送，但不据此宣称新增保障可用。
2. 在运行管理页选择 1–5 档，释放按键、手柄归中后点击“换档（暂停遥控）”。客户端立即撤销本地两类授权与旧 token，刷新状态和 token，等待新的物理中立 ACK，再申请操作授权并提交 `set_speed_level`。五秒内未满足发送前提则取消准备；操作一旦提交，继续按原 ID 查询，不在超时后自动重发。
3. “换档中”期间保留实际档位回显。只有操作 succeeded 且后续 `control_status.speed_level` 与目标一致时显示实际读回成功；`invalidated:true` 仍须同步 epoch/token，不当作换档失败。返回 result_unknown 时查原操作。换档后不会自动运动，须重新归中并明确点击“允许遥控”。档位选择框是待提交目标，旁边“机器人实际档位”才是当前实际值；各档数值完整显示在只读详情 `speed_control` 中，保留服务端字符串档位键及单位，不填默认速度。
4. 空格、手柄双摇杆按键组合或急停按钮发送独立 `emergency_stop`，不等 init/enable 等操作结束。界面分别显示桥接锁存、独立全局禁止 RPC、普通控制器清理 RPC，以及新鲜诊断的 C++ 禁止标志。RPC 成功、命令零速与清理成功均不表示机械停止；页面始终保留“实车机械停止：未确认”。本地急停请求也保持锁存，旧状态响应和重连不能解除它。
5. 确认原因处理完毕，等待全局禁止 RPC `succeeded/ok:true`、新鲜 `runtime_global_inhibit:true`，并查询完在途/未知操作。归中后点击原“解除急停”按钮，在确认框确认。这一步只发顶层 `clear_emergency`，不先申请操作授权；要求响应 `runtime_release_required:true`，不会复位 C++ 禁止。
6. 再归中并点击“允许操作”，随后在运行管理页点击“复位 C++ 全局禁止”，确认后发送 `reset_global_stop`、空参数和新 request_id。服务端仍检查心跳、实际诊断、各组状态及操作冲突；`RUNTIME_NOT_SETTLED` 时查明并消除原因后再显式操作。复位按钮释放、连接恢复与刷新定时器均不发送复位。
7. 查询操作终态，重新同步并检查新鲜 `runtime_global_inhibit:false`。根据各组实际状态显式 `recover → resume`，再发新的中立输入与授权；复位本身不恢复 Running 或遥控。

`arm_secured`、`support_trusted`、`payload_retained` 缺失时显示“未接入/未知”；unknown、expired、stale 及过期运行反馈均不显示证据满足。不提供物理条件注入入口。这些条件关联源组故障后的依赖组继续运行，不把它们展示成整车启动必须全部满足的条件。

## 构建、验证与部署

所有构建产物放在 `/tmp`，不读取或写入下位机参考目录。下面使用本机现有 Qt 6.12 与随附 CMake；其他机器将工具及 Qt 前缀换成当地安装路径。

```sh
/home/fire/Qt/Tools/CMake/bin/cmake -S tests/communication \
  -B /tmp/hostcomputer-field-tests-qt6 \
  -DCMAKE_PREFIX_PATH=/home/fire/Qt/6.12.0/gcc_64 \
  -DHOST_BUILD_WIDGET_TESTS=ON
/home/fire/Qt/Tools/CMake/bin/cmake --build /tmp/hostcomputer-field-tests-qt6 -j 4
/home/fire/Qt/Tools/CMake/bin/ctest --test-dir /tmp/hostcomputer-field-tests-qt6 --output-on-failure -j 4

/home/fire/Qt/Tools/CMake/bin/cmake -S . -B /tmp/hostcomputer-field-app \
  -DCMAKE_PREFIX_PATH=/home/fire/Qt/6.12.0/gcc_64
/home/fire/Qt/Tools/CMake/bin/cmake --build /tmp/hostcomputer-field-app -j 4
```

测试只使用本机临时端口，需要允许回环 TCP 监听；不向机器人发送数据。最小通信回归仍可使用 AGENTS.md 中的系统 Qt5/Qt6 命令；完整应用必须用 Qt6。

部署时关闭旧上位机，按[既有发布流程](上位机发布与验收.md)将新构建及其 Qt/视频依赖部署到上位机，再启动 `hostcomputer`，核对机器人身份和能力。不能将本机开发构建目录当作干净机器发布包。机器人端软件更新、桥接重启、ROS1 参数基线与六轴只读身份登记由下位机开发/现场端在独立获授权工作区按文档52执行；本仓库未实施这些动作。

现场需另行记录机器人构建、实际各档速度、急停忙操作及失败重试、显式解除、真实机械停止时间、制动/载荷保持证据、CAN 故障与恢复。当前不标记实车验收、一键启动现场交付或物理停止确认完成。

## 本轮验证记录

- Qt 6.12 完整应用构建通过，`--version` 回显 `host-ros2-v2-field-1 Qt 6.12.0`。
- Qt6 的 8 组 CTest 已验证通过：通信恢复、管理恢复、配置事务、管理界面、键盘物理中立、控制 v2 会话、控制 v2 TCP、三组预设。控制会话报告 28 项、三组预设报告 36 项（均含 init/cleanup）。首次 Qt6 运行发现新增测试保存了临时 JSON 对象的引用，修正为 `QJsonValue` 副本后，该失败组通过 CTest 复测；其他七组已通过。
- offscreen 完整窗口启动/退出检查通过；该环境出现 Quick3D 软件渲染提示，因此此项仅作为窗口及控制器生命周期检查，不计作 3D 渲染验收。
- 未进行 Qt→真实机器人联调、ROS/CAN、设备持久参数写入或物理停止验收。测试日志、构建与缓存位于 `/tmp/hostcomputer-field-*`。
