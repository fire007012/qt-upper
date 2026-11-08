> **已退役：ROS1 历史资料，不适用于当前 ROS2 系统。** 当前入口见 [ROS2 对接说明](../../../下位机ROS节点对接文档.md) 和 [ROS2 视频联调](../../../docs/ROS2视频联调说明.md)。原路径和命令仅保留供追溯，不要用于当前部署。

# MediaMTX 安装与 systemd 部署

本文用于在下位机部署 MediaMTX。生产环境推荐由 systemd 常驻管理 MediaMTX，`video_stack.launch` 只启动 `video_manager_node` 和 `host_bridge_node`。

## 1. 检查现有安装

```bash
command -v mediamtx
mediamtx --version
ls -l /etc/mediamtx/mediamtx.yml
```

正常应类似：

```text
/usr/local/bin/mediamtx
v1.18.0
/etc/mediamtx/mediamtx.yml
```

## 2. 安装 MediaMTX

如果机器上已经有 `/usr/local/bin/mediamtx` 和 `/etc/mediamtx/mediamtx.yml`，可以跳过本节。

从 MediaMTX Releases 下载对应 CPU 架构的 Linux 压缩包，解压后安装：

```bash
tar -xzf mediamtx_*_linux_amd64.tar.gz
sudo install -m 0755 mediamtx /usr/local/bin/mediamtx
sudo mkdir -p /etc/mediamtx
sudo install -m 0644 mediamtx.yml /etc/mediamtx/mediamtx.yml
```

如果是 ARM64 机器，下载 `linux_arm64` 包，并相应替换压缩包文件名。

验证：

```bash
/usr/local/bin/mediamtx --version
/usr/local/bin/mediamtx /etc/mediamtx/mediamtx.yml
```

看到 RTSP listener opened on `:8554` 后按 `Ctrl+C` 退出。

## 3. 安装 systemd 服务

仓库提供 unit 模板：

```bash
/home/rera/hostcomputer/deploy/systemd/mediamtx.service
```

安装：

```bash
cd /home/rera/hostcomputer
sudo cp deploy/systemd/mediamtx.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now mediamtx
```

本机开发路径如果是 `/home/dianhua/hostcomputer`，把第一行路径换成本机路径即可。

## 4. 验证服务

```bash
systemctl status mediamtx --no-pager
ss -lntp | grep 8554
journalctl -u mediamtx --since '5 minutes ago' --no-pager
```

正常应看到：

```text
listener opened on :8554
```

以及：

```text
LISTEN ... :8554
```

## 5. 与视频栈一起使用

生产启动：

```bash
sudo systemctl enable --now mediamtx
roslaunch ros1_bridge video_stack.launch
```

或者用完整 systemd 视频栈：

```bash
sudo cp deploy/systemd/robot-video-stack.service /etc/systemd/system/
sudo cp deploy/systemd/robot-video-stack.env.example /etc/default/robot-video-stack
sudo nano /etc/default/robot-video-stack
sudo systemctl daemon-reload
sudo systemctl enable --now robot-video-stack
```

确认 `/etc/default/robot-video-stack` 中保持：

```bash
START_MEDIAMTX=false
```

这样 MediaMTX 由 `mediamtx.service` 管理，视频栈只管理 ROS bridge 和 FFmpeg publisher 生命周期。

## 6. 本机回环测试

如果只是在开发机上临时测试，也可以不安装 systemd 服务，让 launch 临时启动 MediaMTX：

```bash
roslaunch ros1_bridge video_stack.launch start_mediamtx:=true
```

这适合本机验证，不建议作为下位机生产部署方式。

## 7. 常见问题

### `Unit file mediamtx.service does not exist`

说明只安装了 `mediamtx` 可执行文件，没有安装 systemd unit。执行：

```bash
cd /home/rera/hostcomputer
sudo cp deploy/systemd/mediamtx.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now mediamtx
```

### `video_manager_node` 显示 `online=False`

先确认 MediaMTX 是否监听：

```bash
ss -lntp | grep 8554
journalctl -u mediamtx --since '5 minutes ago' --no-pager
```

如果 8554 没监听，FFmpeg publisher 无法推流，摄像头会保持 `online=False`。

### 端口被占用

```bash
ss -lntp | grep 8554
```

如果已有 MediaMTX 在运行，不要再用 `start_mediamtx:=true` 启动第二个实例。
