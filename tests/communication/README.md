# 上位机通信回归

直接编译生产 `RobotTcpClient`、协议与日志/错误处理源码，通过本机临时 TCP 服务注入故障。不启动 ROS，不读取或修改 `ros2_ws_car/`，不需要 GUI 显示环境。

依赖 C++20、CMake 及 Qt6 或 Qt5 的 Core、Network、Test 开发组件。使用 Qt5 运行通信回归不表示完整应用已经支持 Qt5；完整 Qt 应用仍需 Qt6。

```sh
cmake -S tests/communication -B /tmp/hostcomputer-communication-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/hostcomputer-communication-tests -j 4
ctest --test-dir /tmp/hostcomputer-communication-tests --output-on-failure
```

测试需要允许本机回环监听，端口自动分配。包含真实退避等待的测试，整套约需 80 秒；CTest 时限为 120 秒。日志写入测试程序所在目录的 `logs/`。

覆盖正常心跳与 ACK、首次/运行中心跳丢失、无关 ACK、首次连接拒绝、超过五次失败后的持续恢复、主动取消重试、关键 ACK 类型匹配与超时、残留半帧、旧连接心跳、输出限额、重连后的中立输入与显式授权、急停、输入拒收撤权、ACK 表满时的急停请求以及不兼容协议版本。

这些测试证明客户端与测试服务之间的行为；不代替 ROS2 桥接联调、物理断网、设备输出归零等实机验收。

## 管理 v1 与界面测试（2026-11-08）

新增 `management_client_recovery` 直接编译生产 ManagementClient/RuntimeService，通过独立 ManagerStub 注入分片、坏报文、超大帧、错误请求 ID、认证失败、15秒无响应、启动/停止响应丢失、任务恢复、预检失败、必需模块异常、可选视频降级和 RECOVERY_REQUIRED。验证无自动重复 start/stop、未知 safety 保留、日志游标与截断标记。控制客户端另加管理路径能力门槛、能力随重连失效和2110撤权回归。

可选 `management_widget_flow` 使用 Qt Widgets 的 offscreen 平台，通过实际按钮验证连接、单次启动、停止、过期状态和令牌不落盘；默认 OFF，纯通信构建仍不需要 Widgets。

本机 Qt6.12 与系统默认 CMake 3.22 不匹配，使用 Qt 随附的 CMake：

```sh
/home/fire/Qt/Tools/CMake/bin/cmake -S tests/communication \
  -B /tmp/hostcomputer-communication-qt6 \
  -DCMAKE_PREFIX_PATH=/home/fire/Qt/6.12.0/gcc_64 \
  -DCMAKE_BUILD_TYPE=Debug -DHOST_BUILD_WIDGET_TESTS=ON
/home/fire/Qt/Tools/CMake/bin/cmake --build /tmp/hostcomputer-communication-qt6 -j 4
/home/fire/Qt/Tools/CMake/bin/ctest --test-dir /tmp/hostcomputer-communication-qt6 \
  --output-on-failure -j 3
```

测试需要本机回环 TCP 权限。受限环境中所有 listen 都失败表示环境禁止监听，不能据此判断业务回归，也不能删除断言或跳过后报告成功。三组用例分别含19、17、1个业务场景；含各组 init/cleanup 共43项。

## 可选：只读参考管理实现的契约测试

该脚本禁用 Python 字节码写入，仅加载传入的 manager.py；档案、令牌、状态和日志全部放在独立临时目录。后端由脚本注入，不创建子进程、ROS、CAN或相机。实际 Qt RuntimeService 与下位机真实协议服务实现完成两轮启动/停止，核对模块启动/停止次数、无残留受管资源、未知 safety 不提升为许可。它不是实际 ROS2 健康检查测试。

```sh
python3 -B tests/communication/manager_reference_integration.py \
  --manager ros2_ws_car/src/robot_manager/manager.py \
  --probe /tmp/hostcomputer-communication-qt6/manager_reference_probe
```

独立测试默认不读取参考目录；上述契约测试需显式提供路径，并且不对其中任何文件进行写入。它只监听临时回环端口，不连接实际部署服务。

## 当前统一入口（ROS1 退役后）

`scripts/run_all_tests.sh` 依次运行完整 Qt 构建、这里的独立 Qt 回归及 `protocol_smoke_test.py`。协议冒烟只启动 `scripts/mock_robot_server.py`，该 mock 不导入 ROS、不宣称支持管理协议或完整安全能力。旧桥接/视频回环脚本已移至 `archive/ros1/scripts/`，不再由当前入口运行。

```sh
CMAKE_BIN=/home/fire/Qt/Tools/CMake/bin/cmake \
CMAKE_PREFIX_PATH=/home/fire/Qt/6.12.0/gcc_64 \
HOST_BUILD_WIDGET_TESTS=ON bash scripts/run_all_tests.sh
```

默认 BUILD_DIR=/tmp/hostcomputer-build-smoke、COMMUNICATION_BUILD_DIR=/tmp/hostcomputer-communication-tests、LOG_DIR=/tmp/hostcomputer-test-logs；可以分别指定外部目录。脚本会拒绝把这些产物写入下位机只读参考目录或历史归档。完整应用要求 Qt6；仅运行通信回归仍可使用上面的独立 CMake 命令。Qt 客户端现名为 RobotTcpClient，构建和测试已统一使用新文件名。

原“进程存活+UDP乱包”流程已退出统一入口：当前控制传输是 TCP/JSON，进程存活不能替代业务稳定性或实际运动归零验收。需要界面进程冒烟时显式设置 RUN_APP_SMOKE=1；真实网络、ROS2与视频验收另行执行。

## 配置事务 v1（文档41）

`configuration_transactions` 直接编译生产 ConfigurationService/ManagementClient，无需 Widgets。覆盖字段范围与视频依赖、保存不激活、维护门槛、应用后源与运行版本区分、revision冲突清除确认、失败回滚与结果未知、响应丢失后跨客户端重建的持久原ID恢复、RESPONSE_TOO_LARGE保留请求、显式回滚、审计分页、坏状态和配置能力关闭。默认mock不导入下位机代码。

界面组新增实际候选保存/确认应用、schema范围与只读信息、断线失效，以及配置写请求阻止启动、激活后重新读取profiles并使用新档案revision的验证。Qt对话框确认由测试显式点击，不跳过确认流程。

显式只读契约测试：

```sh
python3 -B tests/communication/configuration_reference_integration.py \
  --manager ros2_ws_car/src/robot_manager/manager.py \
  --probe /tmp/hostcomputer-config-tests/configuration_reference_probe
```

先按上文CMake步骤构建（该命令示例的构建目录为 `/tmp/hostcomputer-config-tests`）。脚本使用真实管理实现、临时档案和无设备后端，注入已接受但响应丢失及切指针后失败，核对原ID同参数只生成一个操作、失败恢复源、显式回滚和审计；不隐式启停、不写入参考目录。使用说明见 [Qt配置维护](../../docs/Qt配置维护使用与联调.md)。

最新管理器声明控制v2端点；管理probe验证两轮启停各交付一个受支持端点（共2个），实际生产Controller再核对v2并协商身份/能力。上文“43项”为历史批次计数，当前以CTest输出为准。


## 控制v2、输入与诊断（文档40）

默认新增 `control_v2_session`（注入单调时钟，无socket）、`control_v2_transport`（独立C++回环服务）及可选 `keyboard_physical_neutral`（启用HOST_BUILD_WIDGET_TESTS）。默认测试不导入参考目录。

覆盖身份/六项能力、两级授权及状态不能授予权限、令牌RTT预算/零有效期/重新采样、epoch与迟到授权隔离、物理中立和过滤旧离散边沿、错误seq、接受未知的持久原ID恢复、迟到接受落盘、损坏记录阻止新业务、accepted不等于完成、invalidated不产生业务结果、健康心跳下令牌丢失/实际状态过期撤权、普通连接断开后的独立急停、按键仍按住时重置不能伪造中立、遥测过期、脱敏及有界导出。

显式只读真实控制桥契约：

```sh
python3 -B tests/communication/control_reference_integration.py \
  --bridge ros2_ws_car/src/host_bridge/host_bridge_node.py \
  --probe /tmp/hostcomputer-config-tests/control_reference_probe \
  --cycles 100
```

使用实际HostBridgeServer/ControlV2，注入持续更新的模拟runtime与输出捕获，所有journal/settings位于临时目录。默认五项：epoch撤权/跨重连急停；接受响应丢失后同ID仅一次init；30秒慢任务期间通信/独立急停<1秒/模拟输出归零/迟到结果invalidated；100轮主动断开/重连、显式中立及授权、每轮模拟运动且无积压增长；第二控制端OWNER_BUSY、监控不夺权、停止输入而保持心跳/freshness时watchdog撤权归零。

可用 `--case epochAndEmergency|acceptedResponseLost|slowBackendAndIndependentEmergency|repeatedReconnectsNeverRestoreAuthorization|multipleClientsAndInputWatchdog` 单独复现。主动断开后等待150ms供真实接收/写线程退出，再开始下一轮；立即重连可能遇到旧连接尚未释放，客户端须自动恢复，轮次不等于TCP尝试数。输入已有在途帧时重试采样，不能把正常的发送限流当断线或业务失败。该组通常需数分钟，Python与QtTest总时限均按轮数设置；失败不会跳过断言或改用伪参考实现。

这些契约只验证模拟输出。物理断网、真实ROS2/设备、跨机器校时及完整视频隔离必须另行验收。UI进程8秒存活也仅证明创建及事件循环，不证明Quick3D、手柄或视频。

当前同级目录布局的联动步骤见 [上下位机联动排查与启动](../../docs/上下位机联动排查与启动.md)。只读契约的参考路径应显式使用 `/home/fire/ros2_ws_car/src/...`，不要求将下位机复制进上位机仓库。ROS2 临时接口覆盖层现在同时生成 `SetProfile`、`GroupOperation` 和 `ExecuteArmGoal`，支持已协商的分组控制；缺失分组服务不能当作 lifecycle 服务已通过。视频专项新增 `video_widget_lifecycle`，直接验证生产播放器销毁及显式停止后销毁，覆盖子解码器析构发出停止信号的情况。

本地发布证据：`python3 -B scripts/check_host_release.py --binary /tmp/hostcomputer-qt6-build/hostcomputer --output /tmp/hostcomputer-release-check.json`。脚本检查构建标识/Qt6、本机动态依赖和FFmpeg并记录二进制SHA256，不将本机环境检查等同干净机器/Windows发布验收。

## 发布与Linux联调


Linux实际ROS2 DDS mock、仅接口类型的临时覆盖层、真实H264/RTSP解码、Wayland渲染读回及管理20轮契约的命令也见该说明。ROS2测试需显式给定参考路径，不属于默认无ROS通信构建。Windows现为后续可选项，不阻塞Linux验证。

授权单测补充事件循环延迟时新令牌不能掩盖过期间隔、许可失效再恢复不能复活在途授权，以及诊断导出拒绝符号链接目标的回归。发布工具独立验证：`python3 -B -m unittest discover -s tests/tools -v`，无ROS、无回环监听。

## 三组运动预设（文档51）

`motion_presets_protocol` 直接编译生产会话层，以独立 JSON peer 覆盖七种组合、增删预设、部分失败、过期、选择代际隔离和未知结果恢复，不需要 ROS 或本机端口。`control_v2_transport` 另验证连接能力发现及全局急停；`management_widget_flow` 另验证预设多选、空选择与能力门槛。使用及边界见 [Qt三组运动预设使用与验收](../../docs/Qt三组运动预设使用与验收.md)。


文档51复核修复已增加：启动期间普通停止取消后续步骤、在途操作终态后的状态屏障、管理启动的目标/端点/新连接绑定、延迟和缺失能力声明、未知结果的显式停止对账和持久确认、停止诊断缺失/不一致/过期、界面连接阶段提示及不支持的检测命令原因。按用户要求保留手柄热插拔授权行为，并用独立输入回归固定该行为。

验证 Qt5 独立回归（包括可选 Widgets）可显式选择系统 Qt5：

```sh
cmake -S tests/communication -B /tmp/hostcomputer-communication-qt5-fixes \
  -DQT_DIR=/usr/lib/x86_64-linux-gnu/cmake/Qt5 -DHOST_BUILD_WIDGET_TESTS=ON
cmake --build /tmp/hostcomputer-communication-qt5-fixes -j3
QT_QPA_PLATFORM=offscreen ctest --test-dir /tmp/hostcomputer-communication-qt5-fixes --output-on-failure -j4
```

该命令不构建 Qt5 完整应用，不启动 ROS 或硬件。实际 Qt5 安装路径按平台调整。

## 文档52现场控制增量（2026-11-13）

`control_v2_session` 增加可选能力门槛、原始服务端速度声明、严格档位校验、中立/授权换档流程、invalidated 成功后的实际读回、未知换档原 ID 恢复、全局禁止失败保持锁存、显式 clear/reset 分离、旧状态及重连不能解除本地急停、服务端 refresh_ms 调度。`motion_presets_protocol` 在 init/enable/resume 前逐步注入 EPOCH 拒绝，核对同步后仅重建明确未执行的请求，并验证 enable/resume 未知结果不重复执行。

`control_v2_transport` 在 init/enable 操作保持 running 期间验证独立急停、心跳和 operation_query；`management_widget_flow` 验证能力门槛、实际档位、当前禁止标志优先于历史 RPC、物理条件 unknown/expired/stale 和过期反馈展示。测试服务不接触 ROS/CAN，不构成机械停止验收。操作和部署见 [Qt实车联调控制v2](../../docs/Qt实车联调控制v2使用与验收.md)。

## 完整安装栈联动（2026-11-14）

`installed_robot_probe` 不注册默认 CTest。它使用生产 Qt 管理/控制层启动真实 ROS2 Mock 整车，经真实桥接操作预设、换档、急停及显式解除，验证操作终态和实际读回，再停止软件。Python runner 在域 197 监听模拟测量轮速，使用独立临时端口、令牌和 journal，拒绝 hardware 档案。必须先完成完整安装及本地 Mock 配置准备，不能使用仅接口的测试 overlay。

```bash
source /home/fire/ros2_ws_car/linked-install/setup.bash
python3 -B tests/communication/installed_reference_integration.py \
  --config /home/fire/ros2_ws_car/linked-runtime/manager.json \
  --manager /home/fire/ros2_ws_car/src/robot_manager/manager.py \
  --probe /tmp/host-link-tests/installed_robot_probe \
  --output-dir /tmp/host-link-installed-new-run
```

输出目录必须是新目录。该测试与其他占用桥接调试端口 18080 的整车/视频用例顺序执行；测试允许回环 DDS/TCP。新增 `global_stop_reset_ready` 的回归核对普通清理期间不提交解除/复位，未修改未知结果和物理停止的验收边界。
