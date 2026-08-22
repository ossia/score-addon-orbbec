#pragma once

#include <Video/CameraInput.hpp>

#include <depthcam_abi.h>

#include <QString>

#include <atomic>
#include <memory>

namespace Gfx::DepthCamera
{
struct DepthCameraSettings;

/// One output of the camera: a frame queue plus the format it turned out to
/// carry. The format is only known once frames arrive, hence the atomics.
struct StreamOutput
{
  ::Video::FrameQueue queue;

  std::atomic<int> width{0};
  std::atomic<int> height{0};
  std::atomic<int> format{-1}; // AVPixelFormat
  std::atomic<double> fps{0.};
};

/**
 * @brief Owns one open camera and turns its frames into AVFrames.
 *
 * Everything camera-specific lives behind depthcam_abi.h; this class knows only
 * about the ABI. Adding a device family means writing a backend, not touching
 * anything here.
 */
class InputStream final
{
public:
  InputStream(
      const depthcam_backend_v1* backend, const QString& uri,
      const DepthCameraSettings& settings) noexcept;
  ~InputStream() noexcept;

  bool valid() const noexcept { return m_device != nullptr; }

  /// The open camera, for the settings tree. Null until a successful open, and
  /// only valid for as long as this object lives.
  depthcam_device* device() const noexcept { return m_device; }
  const depthcam_backend_v1* backend() const noexcept { return m_backend; }

  StreamOutput m_rgb;
  StreamOutput m_depth;
  StreamOutput m_ir;
  StreamOutput m_pcl;

  bool start() noexcept;
  void stop() noexcept;

  /// Each extractor registers on construction and votes once it is done: all
  /// four share one camera, so the first to go idle must not stop it.
  void registerExtractor() noexcept { m_extractors.fetch_add(1); }
  void requestStop() noexcept;

  /// Millimetres per depth unit, as reported by the backend. 0 if unknown.
  std::atomic<float> depth_unit_mm{0.f};

private:
  static void onFrameThunk(const depthcam_frame* f, void* user);
  void onFrame(const depthcam_frame& f);
  void handleImage(StreamOutput& out, const depthcam_frame& f);
  void handlePointCloud(const depthcam_frame& f);

  AVFrame* decodeCompressed(const depthcam_frame& f);
  AVCodecContext* codecContext(AVCodecID id);
  void releaseCodecs();

  const depthcam_backend_v1* m_backend{};
  depthcam_device* m_device{};

  std::atomic_bool m_running{};
  std::atomic<int> m_extractors{0};
  std::atomic<int> m_stopVotes{0};

  // Only for backends that hand us a compressed colour stream.
  AVCodecContext* m_codecMJPEG{};
  AVCodecContext* m_codec264{};
  AVCodecContext* m_codec265{};
  AVPacket* m_packet{};
};

class InputStreamExtractor final : public ::Video::ExternalInput
{
public:
  InputStreamExtractor(
      std::shared_ptr<InputStream> s, StreamOutput& out, QString filter) noexcept;
  ~InputStreamExtractor();

  bool start() noexcept override;
  void stop() noexcept override;

  AVFrame* dequeue_frame() noexcept override;
  void release_frame(AVFrame* frame) noexcept override;

  void refreshMetadata() noexcept;

  const QString filter;

private:
  std::shared_ptr<InputStream> m_stream;
  StreamOutput& m_out;
};
}
