#include <ros/ros.h>
#include <sensor_msgs/CompressedImage.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cctype>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

struct StreamConfig {
  std::string name;
  std::string image_topic;
  std::string rtsp_url;
  std::string ffmpeg_path = "ffmpeg";
  std::string output_codec = "h264";
  std::string rtsp_transport = "tcp";
  int fps = 30;
  int bitrate_kbps = 2500;
  double frame_timeout_sec = 3.0;
};

struct Config {
  std::vector<StreamConfig> streams;
};

std::string trim(const std::string& value) {
  const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
    return std::isspace(ch);
  });
  const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
    return std::isspace(ch);
  }).base();
  if (begin >= end) {
    return "";
  }
  return std::string(begin, end);
}

std::string stripInlineComment(const std::string& value) {
  bool in_single_quote = false;
  bool in_double_quote = false;
  for (size_t i = 0; i < value.size(); ++i) {
    const char ch = value[i];
    if (ch == '\'' && !in_double_quote) {
      in_single_quote = !in_single_quote;
    } else if (ch == '"' && !in_single_quote) {
      in_double_quote = !in_double_quote;
    } else if (ch == '#' && !in_single_quote && !in_double_quote) {
      return value.substr(0, i);
    }
  }
  return value;
}

std::string unquote(std::string value) {
  value = trim(stripInlineComment(value));
  if (value.size() >= 2) {
    const char first = value.front();
    const char last = value.back();
    if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
      return value.substr(1, value.size() - 2);
    }
  }
  return value;
}

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

int parsePositiveInt(const std::string& key, const std::string& value) {
  try {
    const int parsed = std::stoi(value);
    if (parsed <= 0) {
      throw std::invalid_argument("must be positive");
    }
    return parsed;
  } catch (const std::exception&) {
    throw std::runtime_error(key + " must be a positive integer");
  }
}

double parsePositiveDouble(const std::string& key, const std::string& value) {
  try {
    const double parsed = std::stod(value);
    if (parsed <= 0.0) {
      throw std::invalid_argument("must be positive");
    }
    return parsed;
  } catch (const std::exception&) {
    throw std::runtime_error(key + " must be a positive number");
  }
}

void applyStreamField(StreamConfig& stream, const std::string& key, const std::string& value) {
  if (key == "name") {
    stream.name = value;
  } else if (key == "image_topic") {
    stream.image_topic = value;
  } else if (key == "rtsp_url") {
    stream.rtsp_url = value;
  } else if (key == "ffmpeg_path") {
    stream.ffmpeg_path = value.empty() ? stream.ffmpeg_path : value;
  } else if (key == "output_codec") {
    stream.output_codec = value.empty() ? stream.output_codec : value;
  } else if (key == "rtsp_transport") {
    stream.rtsp_transport = value;
  } else if (key == "fps") {
    stream.fps = parsePositiveInt(key, value);
  } else if (key == "bitrate_kbps") {
    stream.bitrate_kbps = parsePositiveInt(key, value);
  } else if (key == "frame_timeout_sec") {
    stream.frame_timeout_sec = parsePositiveDouble(key, value);
  } else {
    ROS_WARN("ignoring unknown stream config key: %s", key.c_str());
  }
}

void validateStream(const StreamConfig& stream, size_t index) {
  const std::string prefix = "streams[" + std::to_string(index) + "]";
  if (stream.image_topic.empty()) {
    throw std::runtime_error(prefix + ".image_topic is required");
  }
  if (stream.rtsp_url.empty()) {
    throw std::runtime_error(prefix + ".rtsp_url is required");
  }
}

Config loadConfig(const std::string& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open config: " + path);
  }

  Config config;
  StreamConfig* current = nullptr;
  bool in_streams = false;
  std::string line;
  int line_number = 0;

  while (std::getline(input, line)) {
    ++line_number;
    line = trim(stripInlineComment(line));
    if (line.empty()) {
      continue;
    }
    if (line == "streams:") {
      in_streams = true;
      continue;
    }
    if (!in_streams) {
      ROS_WARN("ignoring top-level config line %d: %s", line_number, line.c_str());
      continue;
    }

    if (line[0] == '-') {
      config.streams.emplace_back();
      current = &config.streams.back();
      line = trim(line.substr(1));
      if (line.empty()) {
        continue;
      }
    }
    if (!current) {
      throw std::runtime_error("stream field before list item at line " + std::to_string(line_number));
    }

    const size_t colon = line.find(':');
    if (colon == std::string::npos) {
      throw std::runtime_error("invalid YAML line " + std::to_string(line_number) + ": " + line);
    }
    const std::string key = trim(line.substr(0, colon));
    const std::string value = unquote(line.substr(colon + 1));
    applyStreamField(*current, key, value);
  }

  if (config.streams.empty()) {
    throw std::runtime_error("streams must contain at least one stream");
  }
  for (size_t i = 0; i < config.streams.size(); ++i) {
    validateStream(config.streams[i], i);
    if (config.streams[i].name.empty()) {
      config.streams[i].name = "stream_" + std::to_string(i);
    }
  }
  return config;
}

bool looksLikeJpegFormat(const std::string& format) {
  const std::string lower = toLower(format);
  return lower.find("jpeg") != std::string::npos ||
         lower.find("jpg") != std::string::npos ||
         lower.find("mjpeg") != std::string::npos;
}

bool writeAll(int fd, const uint8_t* data, size_t size) {
  size_t written = 0;
  while (written < size) {
    const ssize_t ret = ::write(fd, data + written, size - written);
    if (ret > 0) {
      written += static_cast<size_t>(ret);
      continue;
    }
    if (ret < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

std::vector<std::string> buildFfmpegCommand(const StreamConfig& stream) {
  const int safe_fps = std::max(stream.fps, 1);

  std::vector<std::string> command = {
      stream.ffmpeg_path,
      "-hide_banner",
      "-loglevel",
      "warning",
      "-fflags",
      "nobuffer",
      "-f",
      "mjpeg",
      "-r",
      std::to_string(safe_fps),
      "-i",
      "pipe:0",
      "-an",
  };

  const std::string codec = toLower(stream.output_codec);
  if (codec == "copy" || codec == "passthrough") {
    command.insert(command.end(), {"-c:v", "copy"});
  } else if (codec == "h264" || codec == "libx264") {
    command.insert(command.end(), {
        "-c:v",
        "libx264",
        "-pix_fmt",
        "yuv420p",
        "-profile:v",
        "baseline",
        "-preset",
        "ultrafast",
        "-tune",
        "zerolatency",
        "-g",
        std::to_string(safe_fps),
        "-keyint_min",
        std::to_string(safe_fps),
        "-sc_threshold",
        "0",
        "-x264-params",
        "repeat-headers=1",
        "-b:v",
        std::to_string(std::max(stream.bitrate_kbps, 1)) + "k",
    });
  } else {
    command.insert(command.end(), {
        "-c:v",
        stream.output_codec,
        "-b:v",
        std::to_string(std::max(stream.bitrate_kbps, 1)) + "k",
    });
  }

  if (!stream.rtsp_transport.empty()) {
    command.insert(command.end(), {"-rtsp_transport", stream.rtsp_transport});
  }
  command.insert(command.end(), {"-f", "rtsp", stream.rtsp_url});
  return command;
}

std::string joinCommand(const std::vector<std::string>& command) {
  std::string result;
  for (const auto& part : command) {
    if (!result.empty()) {
      result += " ";
    }
    result += part;
  }
  return result;
}

}  // namespace

class StreamRuntime {
 public:
  StreamRuntime(ros::NodeHandle& nh, StreamConfig config)
      : nh_(nh), config_(std::move(config)) {}

  ~StreamRuntime() {
    stopFfmpeg();
  }

  void start() {
    subscriber_ = nh_.subscribe(
        config_.image_topic,
        1,
        &StreamRuntime::onImage,
        this,
        ros::TransportHints().tcpNoDelay());

    ROS_INFO("[%s] subscribing %s -> %s",
             config_.name.c_str(),
             config_.image_topic.c_str(),
             config_.rtsp_url.c_str());
  }

  void checkTimeout(const ros::Time& now) {
    if (child_pid_ <= 0 || last_frame_time_.isZero()) {
      return;
    }
    const double silence_sec = (now - last_frame_time_).toSec();
    if (silence_sec < config_.frame_timeout_sec) {
      return;
    }
    ROS_WARN("[%s] no image frames for %.2fs, stopping ffmpeg",
             config_.name.c_str(),
             silence_sec);
    stopFfmpeg();
  }

 private:
  bool ensureFfmpegRunning() {
    if (child_pid_ > 0) {
      int status = 0;
      const pid_t ret = ::waitpid(child_pid_, &status, WNOHANG);
      if (ret == 0) {
        return stdin_fd_ >= 0;
      }
      if (ret == child_pid_) {
        ROS_WARN("[%s] ffmpeg exited, status=%d", config_.name.c_str(), status);
      }
      closePipe();
      child_pid_ = -1;
    }

    return startFfmpeg();
  }

  bool startFfmpeg() {
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) {
      ROS_ERROR("[%s] pipe failed: %s", config_.name.c_str(), std::strerror(errno));
      return false;
    }

    const std::vector<std::string> command = buildFfmpegCommand(config_);

    const pid_t pid = ::fork();
    if (pid < 0) {
      ROS_ERROR("[%s] fork failed: %s", config_.name.c_str(), std::strerror(errno));
      ::close(pipe_fds[0]);
      ::close(pipe_fds[1]);
      return false;
    }

    if (pid == 0) {
      ::close(pipe_fds[1]);
      if (::dup2(pipe_fds[0], STDIN_FILENO) < 0) {
        _exit(127);
      }
      ::close(pipe_fds[0]);

      std::vector<char*> argv;
      argv.reserve(command.size() + 1);
      for (const auto& item : command) {
        argv.push_back(const_cast<char*>(item.c_str()));
      }
      argv.push_back(nullptr);
      ::execvp(argv[0], argv.data());
      _exit(127);
    }

    ::close(pipe_fds[0]);
    stdin_fd_ = pipe_fds[1];
    child_pid_ = pid;
    ROS_INFO("[%s] started ffmpeg pid=%d: %s",
             config_.name.c_str(),
             static_cast<int>(pid),
             joinCommand(command).c_str());
    return true;
  }

  void onImage(const sensor_msgs::CompressedImageConstPtr& msg) {
    if (msg->data.empty()) {
      return;
    }
    last_frame_time_ = ros::Time::now();
    if (!looksLikeJpegFormat(msg->format)) {
      if (!warned_format_) {
        ROS_WARN("[%s] dropping non-JPEG compressed image format=%s",
                 config_.name.c_str(),
                 msg->format.c_str());
        warned_format_ = true;
      }
      return;
    }
    if (!ensureFfmpegRunning()) {
      return;
    }
    if (!writeAll(stdin_fd_, msg->data.data(), msg->data.size())) {
      ROS_WARN("[%s] failed to write image to ffmpeg stdin: %s",
               config_.name.c_str(),
               std::strerror(errno));
      stopFfmpeg();
    }
  }

  void closePipe() {
    if (stdin_fd_ >= 0) {
      ::close(stdin_fd_);
      stdin_fd_ = -1;
    }
  }

  void stopFfmpeg() {
    closePipe();
    if (child_pid_ <= 0) {
      return;
    }
    int status = 0;
    if (::waitpid(child_pid_, &status, WNOHANG) == 0) {
      ::kill(child_pid_, SIGTERM);
      for (int i = 0; i < 10; ++i) {
        if (::waitpid(child_pid_, &status, WNOHANG) == child_pid_) {
          child_pid_ = -1;
          return;
        }
        ::usleep(100000);
      }
      ::kill(child_pid_, SIGKILL);
      ::waitpid(child_pid_, &status, 0);
    }
    child_pid_ = -1;
  }

  ros::NodeHandle& nh_;
  StreamConfig config_;
  ros::Subscriber subscriber_;
  bool warned_format_ = false;
  ros::Time last_frame_time_;
  pid_t child_pid_ = -1;
  int stdin_fd_ = -1;
};

class RosImageRtspNode {
 public:
  explicit RosImageRtspNode(Config config) : config_(std::move(config)) {}

  void start() {
    for (const auto& stream : config_.streams) {
      streams_.emplace_back(new StreamRuntime(nh_, stream));
      streams_.back()->start();
    }
    watchdog_timer_ = nh_.createTimer(
        ros::Duration(0.5),
        &RosImageRtspNode::onWatchdogTimer,
        this);
    ROS_INFO("ros_image_rtsp_node started %zu stream(s)", streams_.size());
  }

 private:
  void onWatchdogTimer(const ros::TimerEvent& event) {
    const ros::Time now = event.current_real;
    for (const auto& stream : streams_) {
      stream->checkTimeout(now);
    }
  }

  Config config_;
  ros::NodeHandle nh_;
  ros::Timer watchdog_timer_;
  std::vector<std::unique_ptr<StreamRuntime>> streams_;
};

int main(int argc, char** argv) {
  ::signal(SIGPIPE, SIG_IGN);
  ros::init(argc, argv, "ros_image_rtsp_node");

  std::string config_path;
  ros::NodeHandle private_nh("~");
  private_nh.param<std::string>("config", config_path, "");
  if (argc > 1 && std::string(argv[1]).find(":=") == std::string::npos) {
    config_path = argv[1];
  }
  if (config_path.empty()) {
    ROS_ERROR("usage: rosrun ros1_bridge ros_image_rtsp_node /path/to/ros_image_rtsp.yaml");
    return 1;
  }

  try {
    RosImageRtspNode node(loadConfig(config_path));
    node.start();
    ros::spin();
  } catch (const std::exception& exc) {
    ROS_ERROR("%s", exc.what());
    return 1;
  }

  return 0;
}
