# Qt 配置维护使用与联调

日期：2026-11-08。依据 [文档41](41_配置维护事务与Qt对接.md)，匹配下位机构建系列 `robot24-p0-config-2`。上位机已实现管理配置事务和独立回归，真实下位机部署、ROS2/硬件与双方现场验收另行完成。

## 入口与操作

1. 工具栏打开“运行管理 / 启动系统”，填写管理地址、端口和共享令牌，连接后切换“配置维护”。配置使用独立的管理 v1 连接；服务端未公布全部九项配置能力时，此页不可操作，运行管理继续可用。部署必须显式启用 `enable_config_transactions=true`。
2. 选择档案，查看只读源档案、schema 与源 bundle revision。编辑项只开放 schema 标记可编辑的 `startup_timeout`（秒，范围取 schema）和 `video_required`（布尔，要求档案已有 video=true）。其他字段及 schema 来源信息只读；不编辑设备标定、限值或 ROS 参数。
3. 修改后点击“核对并保存候选”，确认补丁、影响全部组、应用前停机及应用后重启要求。服务端返回的候选差异与影响显示在预览区。保存不切换管理源，不自动应用。历史候选从列表选择后点击“查看候选差异”；应用确认始终显示实际已加载候选。
4. 在“运行管理”显式停止系统，等到管理状态为 stopped、无恢复待处理且管理操作核对完成；再回配置页点击“确认应用候选”。服务端仍会独立验证资源、归属、revision 和维护前提。客户端不自动 stop/start/enable/resume。
5. 查看独立配置 operation_id、状态、phase、verification 和错误。accepted/running 仅表示接受/执行中；succeeded + manager_source_readback 只表示管理源已激活并读回。停止状态运行版本仍为未知/未运行，显示“待显式启动/重启”。
6. 显式启动前，运行管理会重新查询 profiles，使用该档案返回的 config_revision，而不是把 bundle revision 直接当作启动参数。只有后端 config.status 返回实际运行 revision、观测时间和 pending_restart=false，页面才显示管理启动策略已被运行消费；这不证明任意 ROS 参数或设备配置已生效。
7. 回滚时从审计记录复制历史 bundle revision，先“查看回滚目标”，核对目标档案和全包摘要，再“确认回滚”。回滚影响全部档案，遵循相同维护及操作查询流程；不会自动启动。当前页只展示所选档案的历史内容，全包受影响档案由摘要列出。
8. “加载审计首页 / 下一页审计”每次查询最多50项，日志视图最多1500个文本块。候选、源和实际运行版本分开显示；应用与回滚详情保留机器错误码。

配置进行中、有未确认写请求或配置操作result_unknown时，运行页禁止启动，停止入口仍由原管理服务控制。存在其他管理客户端时，最终互斥与 revision 校验由服务端保证。REVISION_CONFLICT 清除本地候选和回滚确认，重新加载后必须重新核对。

## 断线和未知结果

写请求发送前，将 `config-UUID`、方法、原参数保存到本机 QSettings，操作响应另保存 operation_id。记录按管理地址、端口和 hello.robot_id 隔离，令牌仅在内存。无法持久保存恢复记录时拒绝发起新写请求。

断线/超时后，每约3秒重连并重新核对能力、schema、profiles、status/source 和已知 config.operation。所有操作 ID 均属配置命名空间，不与运行管理 operation 混用。客户端不会自动创建新 ID 重放写请求。响应过大也可能意味着请求已执行，保留原请求，不能把 RESPONSE_TOO_LARGE 当作未执行证明。

若应用响应丢失、没有 operation_id，可在重连查询源和状态后点击“用原ID恢复未知请求”。确认后严格使用原 ID、方法与参数；后端已接受时查询原结果，尚未接受时可能开始执行。这个按钮需要明确确认，不等于纯只读查询。已有 operation_id 时自动轮询其结果。

result_unknown / MANAGER_RESTART / ROLLBACK_UNKNOWN 不显示成功；以当前 source/status 为准，先确认管理 stop，再显式回滚或以新源重新保存并应用候选；未知操作未对账前保持启动禁用。普通“重新加载”不会解除未知请求保护，也不会自动丢弃持久请求记录。

## 可复现验证

独立回归不需要 ROS 或 Widgets；界面回归可选开启。Qt6.12 使用随附的新版 CMake：

```sh
/home/fire/Qt/Tools/CMake/bin/cmake -S tests/communication \
  -B /tmp/hostcomputer-config-tests \
  -DCMAKE_PREFIX_PATH=/home/fire/Qt/6.12.0/gcc_64 \
  -DHOST_BUILD_WIDGET_TESTS=ON
/home/fire/Qt/Tools/CMake/bin/cmake --build /tmp/hostcomputer-config-tests -j 4
/home/fire/Qt/Tools/CMake/bin/ctest --test-dir /tmp/hostcomputer-config-tests --output-on-failure -j 4
python3 -B tests/communication/configuration_reference_integration.py \
  --manager ros2_ws_car/src/robot_manager/manager.py \
  --probe /tmp/hostcomputer-config-tests/configuration_reference_probe
```

最后一条显式只读导入提供的真实管理实现，注入无设备后端：验证保存不激活、预览、已接受响应丢失与原ID恢复、激活后读回、切指针后失败回滚、显式回滚及审计；断言无隐式启停和重复配置操作。Python 禁止写字节码，档案/状态/令牌/Qt设置均放临时目录，不在参考目录运行构建或生成脚本。默认 CTest 只使用上位机独立 mock。

管理连接使用v1，控制链路已按文档40接入v2，见[控制与诊断指南](Qt控制v2与诊断使用.md)。管理就绪会按身份与端点版本协商v2；配置闭环和无设备控制契约均不代表实机验收。
