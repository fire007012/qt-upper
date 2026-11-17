# ROS2 视频联调说明

更新：2026-11-17。当前视频入口随下位机 ROS2 档案由独立管理服务启动；Qt 保留相机列表、流控制和 FFmpeg RTSP 播放能力。日常操作见 [Qt运行管理使用与联调](Qt运行管理使用与联调.md)。

## 配置与启动

1. 下位机开发端在其独立工作区维护视频源、RTSP 发布地址、MediaMTX 配置及档案中的 video/video_required/required_modules/optional_modules。部署后以服务端 profiles 和有效配置 revision 为准。
2. Qt 连接管理服务，选择包含视频能力的档案，点击“一键启动”。服务端负责模块依赖、预检、健康检查和清理；上位机不执行 roslaunch、SSH 或下位机安装命令。
3. 必需视频模块不健康应启动失败；可选模块不健康按档案报告 degraded。在运行日志的模块状态和错误记录中查看原因，不能以控制桥在线代替视频可用。
4. 控制桥同步后，通过相机列表获取实际 rtsp_url；该地址必须能被上位机访问，不能使用仅对下位机本机有效的发布地址。Qt 播放依赖 FFmpeg，打包要求见 [上位机发布与验收](上位机发布与验收.md)。

文档39的管理健康检查还会用 ffprobe 在限定时间内确认实际收到视频帧。Qt 相机 online、MediaMTX 进程存活、端口可连或空源 mock 均不能证明真实相机发布与拉流完成。

## 故障定位与验收

分别检查管理服务可访问性、video 模块状态、相机列表、实际 RTSP 地址及收到的视频帧。记录 profile/revision、构建、失败模块、操作 ID、日志及实际播放结果。真实相机、完整发布和拉流仍需现场验证。

下位机负责确定服务归属和唯一启动入口，避免管理服务和独立 systemd 模板重复拉起视频进程。仓库保留的 `deploy/systemd/mediamtx.service` 只是独立 RTSP 服务参考，不是整车 ROS2 启动入口，是否采用由下位机部署方确定。

旧 ROS1 视频实现、robot-video-stack 模板、源配置和使用文档已移至 [ROS1历史归档](../archive/ros1/README.md)。原本地视频配置完整保存在归档内，须经下位机核对后再导入当前配置；本仓库不自动覆盖 ROS2 配置，也不修改只读参考目录。

## 上位机播放依赖

播放使用 FFmpeg 子进程，构建不链接 `libav*`，无需为本工程安装 FFmpeg 开发库。查找顺序为 `HOSTCOMPUTER_FFMPEG_PATH`、可执行文件同目录的 `ffmpeg/ffmpeg.exe`、系统 PATH。发布包将 FFmpeg 放在 Linux `bin/` 或 Windows 包根，并携带所选构建的许可证声明；实际目录和检查命令见 [发布与验收](上位机发布与验收.md)。
