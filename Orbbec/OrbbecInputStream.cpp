#include "OrbbecInputStream.hpp"
#include <libobsensor/ObSensor.hpp>
#include <magic_enum/magic_enum.hpp>
#include <Video/GStreamerCompatibility.hpp>

namespace Gfx::Orbbec
{

InputStream::InputStream(std::shared_ptr<ob::Config> conf, std::shared_ptr<ob::Device> device) noexcept
    : m_config{conf}
    , m_device{device}
{
  realTime = true;
  initH26XCodecs();
  m_pipeline = std::make_unique<ob::Pipeline>();
  m_pointCloud = std::make_shared<ob::PointCloudFilter>();
  m_pointCloud->setCreatePointFormat(OB_FORMAT_RGB_POINT);
}

InputStream::~InputStream() noexcept { stop(); }

bool InputStream::start() noexcept
{
  if(m_running)
    return false;

  m_pipeline->start(m_config, [&](std::shared_ptr<ob::FrameSet> output) {
    {
      std::lock_guard<std::mutex> lock(m_frames_mtx);
      using namespace std;
      m_frameset.swap(output);
    }
    on_data();
  });

  m_running.store(true, std::memory_order_release);
  return true;
}

void InputStream::stop() noexcept
{
  // Stop the running status
  m_running.store(false, std::memory_order_release);

  m_pipeline->stop();

  // Remove frames that were in flight
  m_rgb_frames.drain();
}

AVFrame *InputStream::dequeue_frame() noexcept { return m_rgb_frames.dequeue(); }

void InputStream::release_frame(AVFrame *frame) noexcept { m_rgb_frames.release(frame); }

void InputStream::initH26XCodecs()
{
  codecMJPEG = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
  codecContextMJPEG = avcodec_alloc_context3(codecMJPEG);
  avcodec_open2(codecContextMJPEG, codecMJPEG, NULL);

  codec264 = avcodec_find_decoder(AV_CODEC_ID_H264);
  codecContext264 = avcodec_alloc_context3(codec264);
  avcodec_open2(codecContext264, codec264, NULL);

  codec265 = avcodec_find_decoder(AV_CODEC_ID_HEVC);
  codecContext265 = avcodec_alloc_context3(codec265);
  avcodec_open2(codecContext265, codec265, NULL);
}

AVFrame *InputStream::decodeFrame(uint8_t *myData, int dataSize, OBFormat fmt){

  AVPacket packet;
  av_init_packet(&packet);
  packet.data = myData;
  packet.size = dataSize;

  // Allocate an AVFrame for decoded data
  AVFrame* frame = av_frame_alloc();

  AVCodecContext* codecContext{};
  switch(fmt)
  {
    case OB_FORMAT_MJPG:
      codecContext = codecContextMJPEG;
      break;
    case OB_FORMAT_H264:
      codecContext = codecContext264;
      break;
    case OB_FORMAT_H265:
      codecContext = codecContext265;
      break;
  }
  if(!codecContext)
  {
    av_frame_free(&frame);
    return nullptr;
  }

  if (int ret = avcodec_send_packet(codecContext, &packet); ret < 0)
  {
    av_frame_free(&frame);
    return nullptr;
  }

  if(avcodec_receive_frame(codecContext, frame) < 0)
  {
    av_frame_free(&frame);
    return nullptr;
  }
  return frame;
}

AVPixelFormat InputStream::get_pixelfmt(OBFormat fmt)
{
  switch(fmt)
  {
    case OB_FORMAT_YUYV:
    case OB_FORMAT_YUY2:
    case OB_FORMAT_GRAY:
      return AVPixelFormat::AV_PIX_FMT_YUYV422;
      return AVPixelFormat::AV_PIX_FMT_YUYV422;
    case OB_FORMAT_UYVY:
      return AVPixelFormat::AV_PIX_FMT_UYVY422;
    case OB_FORMAT_NV12:
      return AVPixelFormat::AV_PIX_FMT_NV12;
    case OB_FORMAT_NV21:
      return AVPixelFormat::AV_PIX_FMT_NV21;
    case OB_FORMAT_Y16:
    case OB_FORMAT_RLE:
    case OB_FORMAT_RVL:
    case OB_FORMAT_Z16:
      return AVPixelFormat::AV_PIX_FMT_GRAY16LE;

    case OB_FORMAT_Y8:
      return AVPixelFormat::AV_PIX_FMT_GRAY8;

      // Y10..14: SDK will unpack into Y16 by default
    case OB_FORMAT_Y10:
      return AVPixelFormat::AV_PIX_FMT_GRAY16LE;
    case OB_FORMAT_Y12:
    case OB_FORMAT_YV12: // Y12: left / YV12: right
      return AVPixelFormat::AV_PIX_FMT_GRAY16LE;
    case OB_FORMAT_Y14:
      return AVPixelFormat::AV_PIX_FMT_GRAY16LE;

    case OB_FORMAT_I420:
      return AVPixelFormat::AV_PIX_FMT_YUV420P;
    case OB_FORMAT_RGB:
      return AV_PIX_FMT_RGB24;
    case OB_FORMAT_BGR:
      return AV_PIX_FMT_BGR24;
    case OB_FORMAT_BGRA:
      return AV_PIX_FMT_BGRA;
    case OB_FORMAT_RGBA:
      return AV_PIX_FMT_RGBA;

    case OB_FORMAT_BYR2: // V4L2_PIX_FMT_SBGGR8 / bayer
      return AV_PIX_FMT_BAYER_BGGR8;

    case OB_FORMAT_UNKNOWN:
    case OB_FORMAT_MJPG:
    case OB_FORMAT_Y11:
    case OB_FORMAT_H264:
    case OB_FORMAT_H265:
    case OB_FORMAT_HEVC:
    case OB_FORMAT_ACCEL:
    case OB_FORMAT_GYRO:
    case OB_FORMAT_POINT:
    case OB_FORMAT_RGB_POINT:
    case OB_FORMAT_COMPRESSED:
    case OB_FORMAT_BA81: // V4L2_PIX_FMT_SBGGR8 / bayer... but orbbec doc says "Is same as Y8, using for right ir stream" ???
    case OB_FORMAT_RW16:
    default:
      return AV_PIX_FMT_NONE;
  }
  return AV_PIX_FMT_NONE;
}

void InputStream::on_data()
{
  if(!m_running)
    return;
  if(!m_frameset)
    return;

  /// Handle color frame
  auto colorp = m_frameset->colorFrame();
  if(!colorp)
    return;

  auto& color = *colorp;
  void* p = color.data();
  std::size_t sz = color.dataSize();
  if(sz <= 1)
    return;

  auto pixfmt = get_pixelfmt(color.getFormat());
  if(pixfmt == AV_PIX_FMT_NONE)
  {
    if(auto frame = decodeFrame(static_cast<uint8_t*>(p), sz, color.getFormat()))
    {
      m_rgb_frames.enqueue(frame);
    }
  }
  else
  {
    ::Video::AVFramePointer frame = m_rgb_frames.newFrame();

    frame->format = pixfmt;
    frame->width = color.getWidth();
    frame->height = color.getHeight();

    // Here we need to copy the buffer.
    const auto storage = ::Video::initFrameBuffer(*frame, sz);
    if(::Video::initFrameFromRawData(frame.get(), storage, sz))
    {
      // Copy the content as we're going on *adventures*
      memcpy(storage, p, sz);

      m_rgb_frames.enqueue(frame.release());
    }

  }

  /// Handle depth frame and pcl
  if(auto depth = m_frameset->depthFrame())
  {
    auto depthValueScale = m_frameset->depthFrame()->getValueScale();
    m_pointCloud->setPositionDataScaled(depthValueScale);
    try {
      std::shared_ptr<ob::Frame> pointCloudFrame = m_pointCloud->process(m_frameset);
      auto* points = reinterpret_cast<const OBColorPoint*>(pointCloudFrame->getData());
      const auto N =  pointCloudFrame->getDataSize() / double(sizeof(OBColorPoint));

      qDebug() << magic_enum::enum_name(pointCloudFrame->getFormat()) << N;
      // pointCloudToMesh(m_frameset->depthFrame(), m_frameset->colorFrame());
    }
    catch(std::exception &e) {
      qDebug() << "Get point cloud failed" ;
    }
  }

}

}
