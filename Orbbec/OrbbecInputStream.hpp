#pragma once

#include <Video/CameraInput.hpp>
#include <libobsensor/h/ObTypes.h>
namespace ob
{
class Config;
class Device;
class FrameSet;
class Pipeline;
class PointCloudFilter;

}

namespace Gfx::Orbbec
{

class InputStream final : public ::Video::ExternalInput
{
public:
  explicit InputStream(
      std::shared_ptr<ob::Config> conf, std::shared_ptr<ob::Device> device) noexcept;

  ~InputStream() noexcept;

  bool start() noexcept override;

  void stop() noexcept override;

  AVFrame* dequeue_frame() noexcept override;

  void release_frame(AVFrame* frame) noexcept override;

  ::Video::FrameQueue m_rgb_frames;
  ::Video::FrameQueue m_depth_frames;
  ::Video::FrameQueue m_ir_frames;
  ::Video::FrameQueue m_pcl_frames;
private:
  const AVCodec* codecMJPEG = nullptr;
  AVCodecContext* codecContextMJPEG = nullptr;

  const AVCodec* codec264 = nullptr;
  AVCodecContext* codecContext264 = nullptr;

  const AVCodec* codec265 = nullptr;
  AVCodecContext* codecContext265 = nullptr;

  void initH26XCodecs();

  AVFrame* decodeFrame(uint8_t * myData, int dataSize, OBFormat fmt);


  AVPixelFormat get_pixelfmt(OBFormat fmt);

  void on_data();


  std::atomic_bool m_running{};

  std::shared_ptr<ob::Config> m_config;
  std::shared_ptr<ob::Device> m_device;
  std::shared_ptr<ob::PointCloudFilter> m_pointCloud;
  std::unique_ptr<ob::Pipeline> m_pipeline;
  std::mutex m_frames_mtx;
  std::shared_ptr<ob::FrameSet> m_frameset = nullptr;
};

}
