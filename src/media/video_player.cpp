#include "tdl_app/video_player.hpp"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cvi_buffer.h"
#include "cvi_common.h"
#include "cvi_comm_sys.h"
#include "cvi_comm_vb.h"
#include "cvi_comm_video.h"
#include "cvi_sys.h"
#include "cvi_vb.h"
#include "cvi_vpss.h"
#include "tdl_app/audio_output.hpp"
#include "tdl_app/sys_context.hpp"

namespace tdl_app {
namespace {

constexpr int kDisplayVpssGroup = 1;
constexpr int kDisplayVpssInputChannel = 0;
constexpr int kMaxWidth = 720;
constexpr int kMaxHeight = 480;
constexpr int kDisplayWidth = 720;
constexpr int kDisplayHeight = 480;
constexpr int kFramePoolSize = 2;
constexpr double kPlaybackFrameRate = 15.0;
constexpr int kAudioSampleRate = 16000;
constexpr int kAudioSamplesPerFrame = 320;  // 20 ms
constexpr int kAudioBytesPerFrame = kAudioSamplesPerFrame * 2;
constexpr const char *kDisplayFilter =
    "fps=15,scale=720:480:force_original_aspect_ratio=decrease,"
    "pad=720:480:(ow-iw)/2:(oh-ih)/2:black,format=nv12";

void setError(std::string *error, const std::string &message) {
  if (error) *error = message;
}

MMF_CHN_S makeChannel(MOD_ID_E module, int device, int channel) {
  MMF_CHN_S value;
  std::memset(&value, 0, sizeof(value));
  value.enModId = module;
  value.s32DevId = device;
  value.s32ChnId = channel;
  return value;
}

struct VideoInfo {
  int width = 0;
  int height = 0;
  std::string codec;
  std::int64_t duration_ms = 0;
  double frame_rate = 0.0;
  bool has_audio = false;
};

double parseFrameRate(const std::string &text) {
  const std::size_t slash = text.find('/');
  if (slash == std::string::npos) return std::atof(text.c_str());
  const double numerator = std::atof(text.substr(0, slash).c_str());
  const double denominator = std::atof(text.substr(slash + 1).c_str());
  return denominator > 0.0 ? numerator / denominator : 0.0;
}

struct PreparedFrame {
  VIDEO_FRAME_INFO_S frame {};
  VB_BLK block = VB_INVALID_HANDLE;
};

bool prepareFrame(int width, int height, PreparedFrame *prepared,
                  std::string *error) {
  VB_CAL_CONFIG_S calc;
  std::memset(&calc, 0, sizeof(calc));
  COMMON_GetPicBufferConfig(static_cast<CVI_U32>(width),
                            static_cast<CVI_U32>(height), PIXEL_FORMAT_NV12,
                            DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN,
                            &calc);

  auto &frame = prepared->frame;
  std::memset(&frame, 0, sizeof(frame));
  frame.stVFrame.enCompressMode = COMPRESS_MODE_NONE;
  frame.stVFrame.enPixelFormat = PIXEL_FORMAT_NV12;
  frame.stVFrame.enVideoFormat = VIDEO_FORMAT_LINEAR;
  frame.stVFrame.enColorGamut = COLOR_GAMUT_BT709;
  frame.stVFrame.enDynamicRange = DYNAMIC_RANGE_SDR8;
  frame.stVFrame.u32Width = static_cast<CVI_U32>(width);
  frame.stVFrame.u32Height = static_cast<CVI_U32>(height);
  frame.stVFrame.u32Stride[0] = calc.u32MainStride;
  frame.stVFrame.u32Stride[1] = calc.u32CStride;
  frame.stVFrame.u32Length[0] = calc.u32MainYSize;
  frame.stVFrame.u32Length[1] = calc.u32MainCSize;

  prepared->block = CVI_VB_GetBlock(VB_INVALID_POOLID, calc.u32VBSize);
  if (prepared->block == VB_INVALID_HANDLE) {
    setError(error, "CVI_VB_GetBlock failed for video frame");
    return false;
  }
  frame.u32PoolId = CVI_VB_Handle2PoolId(prepared->block);
  frame.stVFrame.u64PhyAddr[0] = CVI_VB_Handle2PhysAddr(prepared->block);
  frame.stVFrame.u64PhyAddr[1] =
      frame.stVFrame.u64PhyAddr[0] +
      ALIGN(calc.u32MainYSize, calc.u16AddrAlign);
  frame.stVFrame.pu8VirAddr[0] = static_cast<CVI_U8 *>(CVI_SYS_MmapCache(
      frame.stVFrame.u64PhyAddr[0], frame.stVFrame.u32Length[0]));
  if (!frame.stVFrame.pu8VirAddr[0]) {
    setError(error, "CVI_SYS_MmapCache failed for video Y plane");
    CVI_VB_ReleaseBlock(prepared->block);
    prepared->block = VB_INVALID_HANDLE;
    return false;
  }
  frame.stVFrame.pu8VirAddr[1] = static_cast<CVI_U8 *>(CVI_SYS_MmapCache(
      frame.stVFrame.u64PhyAddr[1], frame.stVFrame.u32Length[1]));
  if (!frame.stVFrame.pu8VirAddr[1]) {
    setError(error, "CVI_SYS_MmapCache failed for video UV plane");
    CVI_SYS_Munmap(frame.stVFrame.pu8VirAddr[0],
                   frame.stVFrame.u32Length[0]);
    frame.stVFrame.pu8VirAddr[0] = nullptr;
    CVI_VB_ReleaseBlock(prepared->block);
    prepared->block = VB_INVALID_HANDLE;
    return false;
  }
  return true;
}

void releaseFrame(PreparedFrame *prepared) {
  auto &frame = prepared->frame.stVFrame;
  if (frame.pu8VirAddr[0]) {
    CVI_SYS_Munmap(frame.pu8VirAddr[0], frame.u32Length[0]);
    frame.pu8VirAddr[0] = nullptr;
  }
  if (frame.pu8VirAddr[1]) {
    CVI_SYS_Munmap(frame.pu8VirAddr[1], frame.u32Length[1]);
    frame.pu8VirAddr[1] = nullptr;
  }
  if (prepared->block != VB_INVALID_HANDLE) {
    CVI_VB_ReleaseBlock(prepared->block);
    prepared->block = VB_INVALID_HANDLE;
  }
}

void copyNv12ToFrame(const std::vector<std::uint8_t> &raw, int width,
                     int height, VIDEO_FRAME_INFO_S *frame) {
  auto &video = frame->stVFrame;
  const std::uint8_t *source_y = raw.data();
  const std::uint8_t *source_uv =
      raw.data() + static_cast<std::size_t>(width) * height;
  for (int row = 0; row < height; ++row) {
    std::memcpy(video.pu8VirAddr[0] +
                    static_cast<std::size_t>(row) * video.u32Stride[0],
                source_y + static_cast<std::size_t>(row) * width,
                static_cast<std::size_t>(width));
  }
  for (int row = 0; row < height / 2; ++row) {
    std::memcpy(video.pu8VirAddr[1] +
                    static_cast<std::size_t>(row) * video.u32Stride[1],
                source_uv + static_cast<std::size_t>(row) * width,
                static_cast<std::size_t>(width));
  }
}

void fillBlackFrame(VIDEO_FRAME_INFO_S *frame) {
  auto &video = frame->stVFrame;
  // Limited-range NV12 black: Y=16, neutral chroma U/V=128.
  std::memset(video.pu8VirAddr[0], 16, video.u32Length[0]);
  std::memset(video.pu8VirAddr[1], 128, video.u32Length[1]);
  CVI_SYS_IonFlushCache(video.u64PhyAddr[0], video.pu8VirAddr[0],
                        video.u32Length[0]);
  CVI_SYS_IonFlushCache(video.u64PhyAddr[1], video.pu8VirAddr[1],
                        video.u32Length[1]);
}

bool readChildOutput(const std::vector<const char *> &argv, std::string *output,
                     std::string *error) {
  int pipe_fds[2] = {-1, -1};
  if (pipe(pipe_fds) != 0) {
    setError(error, "cannot create ffprobe pipe");
    return false;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    setError(error, "cannot fork ffprobe");
    return false;
  }
  if (pid == 0) {
    dup2(pipe_fds[1], STDOUT_FILENO);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    std::vector<char *> exec_args;
    exec_args.reserve(argv.size());
    for (const char *value : argv) {
      exec_args.push_back(const_cast<char *>(value));
    }
    execv(exec_args[0], exec_args.data());
    _exit(127);
  }
  close(pipe_fds[1]);
  char buffer[512];
  std::string text;
  for (;;) {
    const ssize_t count = read(pipe_fds[0], buffer, sizeof(buffer));
    if (count <= 0) break;
    text.append(buffer, static_cast<std::size_t>(count));
    if (text.size() > 4096) break;
  }
  close(pipe_fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    setError(error, "ffprobe cannot read the video stream");
    return false;
  }
  *output = text;
  return true;
}

bool probeH264(const std::string &path, VideoInfo *info, std::string *error) {
  const std::vector<const char *> argv = {
      "/usr/bin/ffprobe", "-v", "error", "-select_streams",
      "v:0", "-show_entries",
      "stream=codec_name,width,height,avg_frame_rate:format=duration", "-of",
      "default=noprint_wrappers=1", path.c_str(), nullptr};
  std::string output;
  if (!readChildOutput(argv, &output, error)) return false;
  std::size_t start = 0;
  while (start < output.size()) {
    const std::size_t end = output.find('\n', start);
    const std::string line = output.substr(start, end - start);
    const std::size_t equal = line.find('=');
    if (equal != std::string::npos) {
      const std::string key = line.substr(0, equal);
      const std::string value = line.substr(equal + 1);
      if (key == "codec_name") info->codec = value;
      if (key == "width") info->width = std::atoi(value.c_str());
      if (key == "height") info->height = std::atoi(value.c_str());
      if (key == "duration") {
        info->duration_ms = static_cast<std::int64_t>(
            std::max(0.0, std::atof(value.c_str()) * 1000.0));
      }
      if (key == "avg_frame_rate") info->frame_rate = parseFrameRate(value);
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  if (info->codec != "h264") {
    setError(error, "首版仅支持 H.264 视频，当前编码为 " + info->codec);
    return false;
  }
  if (info->width <= 0 || info->height <= 0 || info->width > kMaxWidth ||
      info->height > kMaxHeight) {
    setError(error, "首版仅支持不超过 720x480 的视频，当前为 " +
                        std::to_string(info->width) + "x" +
                        std::to_string(info->height));
    return false;
  }
  // Absence of an audio stream is a normal video-only file, not a probe
  // failure.  Query all streams separately so this remains true for MP4,
  // AVI and raw H.264 inputs alike.
  const std::vector<const char *> audio_argv = {
      "/usr/bin/ffprobe", "-v", "error", "-show_entries",
      "stream=codec_type", "-of", "default=noprint_wrappers=1",
      path.c_str(), nullptr};
  if (!readChildOutput(audio_argv, &output, error)) return false;
  info->has_audio = output.find("codec_type=audio") != std::string::npos;
  if (info->frame_rate <= 0.0) info->frame_rate = 25.0;
  return true;
}

}  // namespace

class VideoPlayer::Impl {
 public:
  ~Impl() { shutdown(); }

  bool play(const std::string &path, bool loop, std::string *error) {
    stop();
    VideoInfo info;
    if (!probeH264(path, &info, error)) return false;

    std::string runtime_error;
    if (!system_.open(&runtime_error)) {
      setError(error, "MMF 初始化失败：" + runtime_error);
      return false;
    }

    if (!bindDisplay(&runtime_error)) {
      setError(error, "视频显示通路切换失败：" + runtime_error);
      return false;
    }
    for (int index = 0; index < kFramePoolSize; ++index) {
      PreparedFrame prepared;
      if (!prepareFrame(kDisplayWidth, kDisplayHeight, &prepared,
                        &runtime_error)) {
        releaseFrames();
        restoreDisplay();
        setError(error, "视频帧缓冲申请失败：" + runtime_error);
        return false;
      }
      frames_.push_back(prepared);
    }
    media_width_ = kDisplayWidth;
    media_height_ = kDisplayHeight;
    if (info.has_audio) {
      if (!audio_output_) {
        AudioOutput::Config config = AudioOutput::mono16k();
        config.points_per_frame = kAudioSamplesPerFrame;
        config.frame_count = 12;
        config.volume_db = volume_level_;
        audio_output_ = std::make_unique<AudioOutput>(config);
      }
      if (!audio_output_->isOpen() && !audio_output_->open(&runtime_error)) {
        releaseFrames();
        restoreDisplay();
        setError(error, "音频输出初始化失败：" + runtime_error);
        return false;
      }
      if (!audio_output_->setVolume(volume_level_, &runtime_error)) {
        releaseFrames();
        restoreDisplay();
        setError(error, "音量设置失败：" + runtime_error);
        return false;
      }
    }
    if (!startVideoFfmpeg(path, loop, &runtime_error)) {
      closeAudioAsync();
      releaseFrames();
      restoreDisplay();
      setError(error, runtime_error);
      return false;
    }
    if (info.has_audio && !startAudioFfmpeg(path, loop, &runtime_error)) {
      terminateFfmpeg();
      waitVideoFfmpeg();
      closeAudioAsync();
      releaseFrames();
      restoreDisplay();
      setError(error, runtime_error);
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      stop_requested_ = false;
      paused_ = false;
      audio_done_ = !info.has_audio;
      state_ = "playing";
      last_error_.clear();
      width_ = info.width;
      height_ = info.height;
      duration_ms_ = info.duration_ms;
      position_ms_ = 0;
      frame_rate_ = std::min(info.frame_rate, kPlaybackFrameRate);
      has_audio_ = info.has_audio;
      played_before_pause_ms_ = 0;
      play_started_at_ = std::chrono::steady_clock::now();
    }
    if (info.has_audio) audio_worker_ = std::thread(&Impl::runAudio, this);
    worker_ = std::thread(&Impl::run, this);
    return true;
  }

  void pause() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (state_ == "playing") {
      updatePositionLocked();
      played_before_pause_ms_ = position_ms_;
      paused_ = true;
      state_ = "paused";
      if (audio_output_) {
        std::string ignored;
        audio_output_->pause(&ignored);
      }
      if (ffmpeg_pid_ > 0) kill(ffmpeg_pid_, SIGSTOP);
      if (audio_ffmpeg_pid_ > 0) kill(audio_ffmpeg_pid_, SIGSTOP);
    }
  }

  void resume() {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (state_ != "paused") return;
      paused_ = false;
      state_ = "playing";
      play_started_at_ = std::chrono::steady_clock::now();
      if (audio_output_) {
        std::string ignored;
        audio_output_->resume(&ignored);
      }
      if (ffmpeg_pid_ > 0) kill(ffmpeg_pid_, SIGCONT);
      if (audio_ffmpeg_pid_ > 0) kill(audio_ffmpeg_pid_, SIGCONT);
    }
    pause_cv_.notify_all();
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      stop_requested_ = true;
      paused_ = false;
    }
    pause_cv_.notify_all();
    terminateFfmpeg();
    if (worker_.joinable()) worker_.join();
    if (audio_worker_.joinable()) audio_worker_.join();
    showBlackFrame();
    closeMedia(false);
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (state_ != "idle" && state_ != "error") state_ = "stopped";
  }

  void shutdown() {
    stop();
    closeMedia(true);
    closeAudioAsync();
  }

  std::string state() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_;
  }

  std::string lastError() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_error_;
  }

  bool isPlaying() const {
    const std::string value = state();
    return value == "playing" || value == "paused";
  }

  int width() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return width_;
  }

  int height() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return height_;
  }

  std::int64_t durationMs() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return duration_ms_;
  }

  std::int64_t positionMs() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    std::int64_t value = position_ms_;
    if (state_ == "playing" && !paused_) {
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - play_started_at_)
              .count();
      value = played_before_pause_ms_ + std::max<std::int64_t>(0, elapsed);
    }
    if (duration_ms_ > 0) value = std::min(value, duration_ms_);
    return value;
  }

  bool hasAudio() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return has_audio_;
  }

  bool setVolume(int volume_level, std::string *error) {
    const int clamped = std::max(0, std::min(32, volume_level));
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (audio_output_ && !audio_output_->setVolume(clamped, error)) {
      return false;
    }
    volume_level_ = clamped;
    return true;
  }

  int volume() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return volume_level_;
  }

 private:
  bool bindDisplay(std::string *error) {
    if (display_bound_) return true;
    display_destination_ = makeChannel(CVI_ID_VPSS, kDisplayVpssGroup,
                                       kDisplayVpssInputChannel);
    MMF_CHN_S previous;
    std::memset(&previous, 0, sizeof(previous));
    has_previous_source_ =
        CVI_SYS_GetBindbyDest(&display_destination_, &previous) == CVI_SUCCESS;
    if (has_previous_source_) {
      previous_source_ = previous;
      CVI_SYS_UnBind(&previous_source_, &display_destination_);
    }
    // With no source bound, frames can be pushed directly into display VPSS.
    display_bound_ = true;
    return true;
  }

  void restoreDisplay() {
    if (!display_bound_ && !has_previous_source_) return;
    if (has_previous_source_) {
      CVI_SYS_Bind(&previous_source_, &display_destination_);
    }
    display_bound_ = false;
    has_previous_source_ = false;
  }

  bool startVideoFfmpeg(const std::string &path, bool loop,
                        std::string *error) {
    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) {
      setError(error, "cannot create FFmpeg video pipe");
      return false;
    }
    const pid_t pid = fork();
    if (pid < 0) {
      ::close(pipe_fds[0]);
      ::close(pipe_fds[1]);
      setError(error, "cannot start FFmpeg video decoder");
      return false;
    }
    if (pid == 0) {
      dup2(pipe_fds[1], STDOUT_FILENO);
      ::close(pipe_fds[0]);
      ::close(pipe_fds[1]);
      if (loop) {
        execl("/usr/bin/ffmpeg", "ffmpeg", "-nostdin", "-hide_banner",
              "-loglevel", "quiet", "-stream_loop", "-1", "-skip_frame",
              "noref", "-re", "-i", path.c_str(), "-map", "0:v:0", "-an",
              "-sn", "-dn",
              "-threads", "2", "-vf", kDisplayFilter, "-pix_fmt", "nv12",
              "-f", "rawvideo", "pipe:1", static_cast<char *>(nullptr));
      } else {
        execl("/usr/bin/ffmpeg", "ffmpeg", "-nostdin", "-hide_banner",
              "-loglevel", "quiet", "-skip_frame", "noref", "-re", "-i",
              path.c_str(), "-map", "0:v:0", "-an", "-sn", "-dn",
              "-threads", "2", "-vf", kDisplayFilter, "-pix_fmt", "nv12",
              "-f", "rawvideo", "pipe:1", static_cast<char *>(nullptr));
      }
      _exit(127);
    }
    ::close(pipe_fds[1]);
    ffmpeg_pid_ = pid;
    ffmpeg_fd_ = pipe_fds[0];
    return true;
  }

  bool startAudioFfmpeg(const std::string &path, bool loop,
                        std::string *error) {
    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) {
      setError(error, "cannot create FFmpeg audio pipe");
      return false;
    }
    const pid_t pid = fork();
    if (pid < 0) {
      ::close(pipe_fds[0]);
      ::close(pipe_fds[1]);
      setError(error, "cannot start FFmpeg audio decoder");
      return false;
    }
    if (pid == 0) {
      dup2(pipe_fds[1], STDOUT_FILENO);
      ::close(pipe_fds[0]);
      ::close(pipe_fds[1]);
      if (loop) {
        execl("/usr/bin/ffmpeg", "ffmpeg", "-nostdin", "-hide_banner",
              "-loglevel", "quiet", "-stream_loop", "-1", "-i",
              path.c_str(), "-map", "0:a:0", "-vn", "-sn", "-dn",
              "-threads", "1", "-ac", "1", "-ar", "16000", "-f",
              "s16le", "pipe:1",
              static_cast<char *>(nullptr));
      } else {
        execl("/usr/bin/ffmpeg", "ffmpeg", "-nostdin", "-hide_banner",
              "-loglevel", "quiet", "-i", path.c_str(), "-map",
              "0:a:0", "-vn", "-sn", "-dn", "-threads", "1", "-ac",
              "1", "-ar", "16000", "-f", "s16le", "pipe:1",
              static_cast<char *>(nullptr));
      }
      _exit(127);
    }
    ::close(pipe_fds[1]);
    audio_ffmpeg_pid_ = pid;
    audio_fd_ = pipe_fds[0];
    return true;
  }

  void terminateFfmpeg() {
    const pid_t pid = ffmpeg_pid_;
    if (pid > 0) {
      kill(pid, SIGCONT);
      kill(pid, SIGTERM);
    }
    const pid_t audio_pid = audio_ffmpeg_pid_;
    if (audio_pid > 0) {
      kill(audio_pid, SIGCONT);
      kill(audio_pid, SIGTERM);
    }
  }

  void waitVideoFfmpeg() {
    if (ffmpeg_fd_ >= 0) {
      ::close(ffmpeg_fd_);
      ffmpeg_fd_ = -1;
    }
    if (ffmpeg_pid_ > 0) {
      int status = 0;
      waitpid(ffmpeg_pid_, &status, 0);
      ffmpeg_pid_ = -1;
    }
  }

  void waitAudioFfmpeg() {
    if (audio_fd_ >= 0) {
      ::close(audio_fd_);
      audio_fd_ = -1;
    }
    if (audio_ffmpeg_pid_ > 0) {
      int status = 0;
      waitpid(audio_ffmpeg_pid_, &status, 0);
      audio_ffmpeg_pid_ = -1;
    }
  }

  bool shouldStop() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return stop_requested_;
  }

  bool waitUntilPlayable() {
    std::unique_lock<std::mutex> lock(state_mutex_);
    pause_cv_.wait(lock, [this] { return stop_requested_ || !paused_; });
    return !stop_requested_;
  }

  void updatePositionLocked() {
    if (state_ != "playing" || paused_) return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - play_started_at_).count();
    position_ms_ = played_before_pause_ms_ + std::max<std::int64_t>(0, elapsed);
    if (duration_ms_ > 0) position_ms_ = std::min(position_ms_, duration_ms_);
  }

  void updateVideoPosition(std::uint64_t displayed_frames) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (has_audio_) {
      updatePositionLocked();
      return;
    }
    const double milliseconds =
        static_cast<double>(displayed_frames) * 1000.0 / frame_rate_;
    position_ms_ = static_cast<std::int64_t>(milliseconds);
    if (duration_ms_ > 0) position_ms_ = std::min(position_ms_, duration_ms_);
  }

  void runAudio() {
    std::vector<std::uint8_t> pcm(kAudioBytesPerFrame);
    std::uint32_t sequence = 0;
    while (!shouldStop()) {
      if (!waitUntilPlayable()) break;
      std::size_t offset = 0;
      while (offset < pcm.size() && !shouldStop()) {
        const ssize_t count = read(audio_fd_, pcm.data() + offset,
                                   pcm.size() - offset);
        if (count > 0) {
          offset += static_cast<std::size_t>(count);
          continue;
        }
        if (count < 0 && errno == EINTR) continue;
        break;
      }
      if (offset == 0 || shouldStop()) break;
      // The final decoder packet may be shorter than 20 ms; pad it so AO
      // always receives a complete PCM frame.
      if (offset < pcm.size()) {
        std::fill(pcm.begin() + static_cast<std::ptrdiff_t>(offset), pcm.end(),
                  0);
      }
      AudioFrame frame;
      frame.bit_width = AudioBitWidth::Bits16;
      frame.sound_mode = AudioSoundMode::Mono;
      // CVI AO performs its own sample-rate pacing.  The vendor examples use
      // a zero timestamp for every user-mode frame; supplying millisecond
      // values here is ambiguous because the SDK timestamp unit is not ms.
      frame.timestamp = 0;
      frame.sequence = sequence++;
      frame.bytes_per_channel = kAudioBytesPerFrame;
      frame.channels.push_back(pcm);
      std::string ignored;
      if (!audio_output_ || !audio_output_->writeFrame(frame, 1000, &ignored)) {
        if (!shouldStop()) {
          std::lock_guard<std::mutex> lock(state_mutex_);
          last_error_ = "音频输出失败：" + ignored;
        }
        break;
      }
    }
    waitAudioFfmpeg();
    audio_done_ = true;
  }

  void run() {
    const std::size_t frame_bytes =
        static_cast<std::size_t>(media_width_) * media_height_ * 3 / 2;
    std::vector<std::uint8_t> raw(frame_bytes);
    std::string error;
    bool failed = false;
    std::uint64_t frame_index = 0;
    while (!shouldStop()) {
      if (!waitUntilPlayable()) break;
      std::size_t offset = 0;
      while (offset < raw.size() && !shouldStop()) {
        const ssize_t count =
            read(ffmpeg_fd_, raw.data() + offset, raw.size() - offset);
        if (count > 0) {
          offset += static_cast<std::size_t>(count);
          continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && !shouldStop()) {
          error = "FFmpeg 视频帧读取失败";
          failed = true;
        }
        break;
      }
      if (offset == 0 || shouldStop()) break;
      if (offset != raw.size()) {
        // FFmpeg may close the rawvideo pipe with a short trailing read at
        // normal EOF.  It is not a displayable frame, but it is not a decode
        // failure either; discard it and finish with the last complete frame.
        break;
      }

      PreparedFrame &prepared =
          frames_[static_cast<std::size_t>(frame_index % frames_.size())];
      copyNv12ToFrame(raw, media_width_, media_height_, &prepared.frame);
      prepared.frame.stVFrame.u32TimeRef =
          static_cast<CVI_U32>(frame_index);
      prepared.frame.stVFrame.u64PTS = frame_index;
      CVI_SYS_IonFlushCache(prepared.frame.stVFrame.u64PhyAddr[0],
                            prepared.frame.stVFrame.pu8VirAddr[0],
                            prepared.frame.stVFrame.u32Length[0]);
      CVI_SYS_IonFlushCache(prepared.frame.stVFrame.u64PhyAddr[1],
                            prepared.frame.stVFrame.pu8VirAddr[1],
                            prepared.frame.stVFrame.u32Length[1]);
      const int ret =
          CVI_VPSS_SendFrame(kDisplayVpssGroup, &prepared.frame, 1000);
      if (ret != CVI_SUCCESS) {
        error = "CVI_VPSS_SendFrame failed, ret=" + std::to_string(ret);
        failed = true;
        break;
      }
      ++frame_index;
      updateVideoPosition(frame_index);
    }
    if (failed) terminateFfmpeg();
    waitVideoFfmpeg();

    // Keep feeding black frames while audio drains and AO shuts down.  OSD is
    // composed by VPSS only when a video frame arrives; without this pump a
    // video with audio leaves LVGL visually frozen during teardown.
    if (!failed && !shouldStop()) {
      std::lock_guard<std::mutex> lock(state_mutex_);
      state_ = "finishing";
      if (duration_ms_ > 0) position_ms_ = duration_ms_;
    }
    while (!audio_done_ && !shouldStop()) {
      showBlackFrame();
      std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    if (audio_worker_.joinable()) audio_worker_.join();

    if (failed) {
      showBlackFrame();
      closeMedia(false);
      std::lock_guard<std::mutex> lock(state_mutex_);
      state_ = "error";
      last_error_ = error.empty() ? "硬件视频解码失败" : error;
    } else if (!shouldStop()) {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = "finished";
        if (duration_ms_ > 0) position_ms_ = duration_ms_;
      }
      // Retain the display path and keep OSD refreshes alive until the next
      // play/stop/close request.  Rebinding the camera at every EOF leaves a
      // frame-less window where controls are visible but cannot redraw.
      while (!shouldStop()) {
        showBlackFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
      }
    }
  }

  void showBlackFrame() {
    if (frames_.empty() || !display_bound_) return;
    PreparedFrame &prepared = frames_.front();
    fillBlackFrame(&prepared.frame);
    prepared.frame.stVFrame.u64PTS = 0;
    prepared.frame.stVFrame.u32TimeRef = 0;
    (void)CVI_VPSS_SendFrame(kDisplayVpssGroup, &prepared.frame, 30);
  }

  void closeMedia(bool restore_source) {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    releaseFrames();
    system_.close();
    if (restore_source) restoreDisplay();
  }

  void closeAudioAsync() {
    std::unique_ptr<AudioOutput> output = std::move(audio_output_);
    if (!output) return;
    std::thread([output = std::move(output)]() mutable {
      std::string ignored;
      output->clearBuffer(&ignored);
      output->close();
    }).detach();
  }

  void releaseFrames() {
    for (auto &frame : frames_) releaseFrame(&frame);
    frames_.clear();
  }

  SysContext system_;
  std::vector<PreparedFrame> frames_;
  std::thread worker_;
  std::thread audio_worker_;
  std::unique_ptr<AudioOutput> audio_output_;
  mutable std::mutex state_mutex_;
  std::mutex resource_mutex_;
  std::condition_variable pause_cv_;
  std::atomic<bool> audio_done_{true};
  bool stop_requested_ = true;
  bool paused_ = false;
  std::string state_ = "idle";
  std::string last_error_;
  int width_ = 0;
  int height_ = 0;
  std::int64_t duration_ms_ = 0;
  std::int64_t position_ms_ = 0;
  std::int64_t played_before_pause_ms_ = 0;
  double frame_rate_ = 25.0;
  bool has_audio_ = false;
  int volume_level_ = 24;
  std::chrono::steady_clock::time_point play_started_at_;
  int media_width_ = 0;
  int media_height_ = 0;
  int ffmpeg_fd_ = -1;
  int audio_fd_ = -1;
  pid_t ffmpeg_pid_ = -1;
  pid_t audio_ffmpeg_pid_ = -1;
  MMF_CHN_S display_destination_ {};
  MMF_CHN_S previous_source_ {};
  bool has_previous_source_ = false;
  bool display_bound_ = false;
};

VideoPlayer::VideoPlayer() : impl_(new Impl) {}
VideoPlayer::~VideoPlayer() = default;
bool VideoPlayer::play(const std::string &path, bool loop, std::string *error) {
  return impl_->play(path, loop, error);
}
void VideoPlayer::pause() { impl_->pause(); }
void VideoPlayer::resume() { impl_->resume(); }
void VideoPlayer::stop() { impl_->stop(); }
void VideoPlayer::close() { impl_->shutdown(); }
std::string VideoPlayer::state() const { return impl_->state(); }
std::string VideoPlayer::lastError() const { return impl_->lastError(); }
bool VideoPlayer::isPlaying() const { return impl_->isPlaying(); }
int VideoPlayer::width() const { return impl_->width(); }
int VideoPlayer::height() const { return impl_->height(); }
std::int64_t VideoPlayer::durationMs() const { return impl_->durationMs(); }
std::int64_t VideoPlayer::positionMs() const { return impl_->positionMs(); }
bool VideoPlayer::hasAudio() const { return impl_->hasAudio(); }
bool VideoPlayer::setVolume(int volume_level, std::string *error) {
  return impl_->setVolume(volume_level, error);
}
int VideoPlayer::volume() const { return impl_->volume(); }

}  // namespace tdl_app
