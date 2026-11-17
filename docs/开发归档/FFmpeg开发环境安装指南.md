> 历史归档（2026-11-17 整理）。本文保留当时的设计、接口交接或验证记录；版本、路径、命令和临时授权仅对应原记录，不作为当前操作依据。当前说明见 [文档入口](../README.md)。

# FFmpeg 开发环境安装指南

## 什么时候需要安装

协作者在开发机上运行上位机、RTSP 本机回环测试、或下位机桥接节点的视频推流功能时，需要本机能执行：

```bash
ffmpeg -version
ffprobe -version
```

项目构建不链接 `libav*`，因此不需要安装 FFmpeg 开发库。这里只要求 FFmpeg 命令行工具可用。

正式发布包的要求不同：发布包应把 `ffmpeg.exe` 或 `ffmpeg` 放在上位机可执行文件同目录。详见 [上位机部署说明.md](../上位机发布与验收.md)。

## Linux

Ubuntu / Debian：

```bash
sudo apt update
sudo apt install -y ffmpeg
ffmpeg -version
ffprobe -version
```

Fedora：

```bash
sudo dnf install -y ffmpeg ffmpeg-free
ffmpeg -version
ffprobe -version
```

Arch：

```bash
sudo pacman -S ffmpeg
ffmpeg -version
ffprobe -version
```

如果系统仓库版本过旧，也可以下载静态构建，把 `ffmpeg` 和 `ffprobe` 放入某个目录，并将该目录加入 `PATH`。

## Windows

推荐做法是下载预编译 FFmpeg，并把 `bin` 目录加入 `PATH`。

步骤：

1. 下载 Windows x64 FFmpeg 构建，例如 `ffmpeg-release-essentials.zip`。
2. 解压到固定目录，例如：

```text
C:\Tools\ffmpeg
```

3. 确认目录内存在：

```text
C:\Tools\ffmpeg\bin\ffmpeg.exe
C:\Tools\ffmpeg\bin\ffprobe.exe
```

4. 将下面目录加入用户或系统 `PATH`：

```text
C:\Tools\ffmpeg\bin
```

5. 重新打开 PowerShell，验证：

```powershell
ffmpeg -version
ffprobe -version
```

如果不想修改 `PATH`，也可以设置环境变量：

```powershell
$env:HOSTCOMPUTER_FFMPEG_PATH = "C:\Tools\ffmpeg\bin\ffmpeg.exe"
```

这个变量只影响当前 PowerShell 会话。需要永久生效时，请在 Windows 环境变量设置里添加。

## macOS

当前项目主要目标是 Windows 和 Linux，但 macOS 开发机可以用 Homebrew：

```bash
brew install ffmpeg
ffmpeg -version
ffprobe -version
```

## 项目内检查

开发机检查：

```bash
python3 scripts/check_host_runtime.py \
  --app-dir build/Desktop_Qt_6_7_3-Debug
```

如果只想检查命令行工具：

```bash
ffmpeg -version
ffprobe -version
```

正式发布包检查：

```bash
python3 scripts/check_host_runtime.py \
  --app-dir /path/to/release/hostcomputer \
  --require-bundled-ffmpeg
```

`--require-bundled-ffmpeg` 会要求 FFmpeg 位于上位机可执行文件同目录，用来防止发布包在未安装 FFmpeg 的机器上无法播放视频。

## 常见问题

如果上位机提示无法启动 FFmpeg：

- 先确认 `ffmpeg -version` 能在终端或 PowerShell 中运行。
- 如果能运行终端命令但上位机仍找不到，重启 IDE 或终端，确保它继承了新的 `PATH`。
- 如果使用 `HOSTCOMPUTER_FFMPEG_PATH`，确认路径指向 `ffmpeg.exe` 或 `ffmpeg` 文件本身，而不是目录。
- Windows 路径里有空格时可以正常使用环境变量，不需要额外加引号。

如果 RTSP 能连但没有画面：

- 确认 `ffprobe -rtsp_transport tcp rtsp://...` 能看到视频流。
- 确认下位机推流端输出 H264 `yuv420p`，避免播放器兼容性问题。
