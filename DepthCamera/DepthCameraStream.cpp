#include "DepthCameraStream.hpp"

#include <cstring>

#include <DepthCamera/DepthCameraSettings.hpp>

#include <Video/GStreamerCompatibility.hpp>

#include <QDebug>

#include <utility>

namespace Gfx::DepthCamera
{
namespace
{
constexpr std::size_t max_queued_frames = 16;
/// Lower than the image bound: each queued cloud pins a buffer inside the
/// backend's own pool, and a coloured cloud is tens of megabytes.
constexpr std::size_t max_queued_pointclouds = 3;

AVPixelFormat to_av_format(int fmt)
{
  switch(fmt)
  {
    case DEPTHCAM_FMT_RGB24:
      return AV_PIX_FMT_RGB24;
    case DEPTHCAM_FMT_BGR24:
      return AV_PIX_FMT_BGR24;
    case DEPTHCAM_FMT_RGBA:
      return AV_PIX_FMT_RGBA;
    case DEPTHCAM_FMT_BGRA:
      return AV_PIX_FMT_BGRA;
    case DEPTHCAM_FMT_RGB0:
      return AV_PIX_FMT_RGB0;
    case DEPTHCAM_FMT_BGR0:
      return AV_PIX_FMT_BGR0;
    case DEPTHCAM_FMT_GRAY8:
      return AV_PIX_FMT_GRAY8;
    case DEPTHCAM_FMT_GRAY16:
      return AV_PIX_FMT_GRAY16LE;
    case DEPTHCAM_FMT_GRAYF32:
      return AV_PIX_FMT_GRAYF32LE;
    case DEPTHCAM_FMT_YUYV422:
      return AV_PIX_FMT_YUYV422;
    case DEPTHCAM_FMT_UYVY422:
      return AV_PIX_FMT_UYVY422;
    case DEPTHCAM_FMT_NV12:
      return AV_PIX_FMT_NV12;
    case DEPTHCAM_FMT_YUV420P:
      return AV_PIX_FMT_YUV420P;
    default:
      // Compressed and point-cloud formats are handled separately.
      return AV_PIX_FMT_NONE;
  }
}

AVCodecID to_codec_id(int fmt)
{
  switch(fmt)
  {
    case DEPTHCAM_FMT_MJPEG:
      return AV_CODEC_ID_MJPEG;
    case DEPTHCAM_FMT_H264:
      return AV_CODEC_ID_H264;
    case DEPTHCAM_FMT_H265:
      return AV_CODEC_ID_H265;
    default:
      return AV_CODEC_ID_NONE;
  }
}

/// Hands a backend buffer back when libav is done with it. `owner` is the
/// backend's own opaque pointer, so nothing is copied on the way in.
struct BackendBufferHolder
{
  void* owner{};
  void (*release)(void*){};
};

void release_backend_buffer(void* opaque, uint8_t*)
{
  auto* h = static_cast<BackendBufferHolder*>(opaque);
  if(h->release)
    h->release(h->owner);
  delete h;
}

/// Wrap a backend-owned buffer in an AVFrame without copying it.
bool wrap_borrowed(AVFrame& frame, const depthcam_frame& f)
{
  auto* holder = new BackendBufferHolder{f.owner, f.release};
  frame.buf[0] = av_buffer_create(
      const_cast<uint8_t*>(static_cast<const uint8_t*>(f.data)), f.bytes,
      &release_backend_buffer, holder, AV_BUFFER_FLAG_READONLY);
  if(!frame.buf[0])
  {
    delete holder;
    return false;
  }
  frame.data[0] = frame.buf[0]->data;
  return true;
}
}

InputStream::InputStream(
    const depthcam_backend_v1* backend, const QString& uri,
    const DepthCameraSettings& settings) noexcept
    : m_backend{backend}
{
  if(!m_backend || !m_backend->open)
    return;

  depthcam_open_config cfg{};
  if(settings.rgb)
    cfg.streams |= DEPTHCAM_STREAM_COLOR;
  if(settings.ir)
    cfg.streams |= DEPTHCAM_STREAM_IR;
  if(settings.depth)
    cfg.streams |= DEPTHCAM_STREAM_DEPTH;
  if(settings.pointcloud)
    cfg.streams |= DEPTHCAM_STREAM_POINTCLOUD;
  if(settings.imu)
    cfg.streams |= DEPTHCAM_STREAM_IMU;

  cfg.color_width = settings.colorWidth;
  cfg.color_height = settings.colorHeight;
  cfg.color_fps = settings.colorFps;
  cfg.ir_width = settings.irWidth;
  cfg.ir_height = settings.irHeight;
  cfg.ir_fps = settings.irFps;
  cfg.depth_width = settings.depthWidth;
  cfg.depth_height = settings.depthHeight;
  cfg.depth_fps = settings.depthFps;
  cfg.color_pointcloud = settings.colorPointcloud ? 1 : 0;

  switch(settings.align)
  {
    case DepthCameraSettings::AlignMode::DepthToColor:
      cfg.align = DEPTHCAM_ALIGN_DEPTH_TO_COLOR;
      break;
    case DepthCameraSettings::AlignMode::ColorToDepth:
      cfg.align = DEPTHCAM_ALIGN_COLOR_TO_DEPTH;
      break;
    default:
      cfg.align = DEPTHCAM_ALIGN_NONE;
      break;
  }

  const auto u = uri.toUtf8();
  m_device = m_backend->open(u.constData(), &cfg);
  if(!m_device)
  {
    const char* err = m_backend->last_error ? m_backend->last_error() : nullptr;
    qDebug() << "[depthcam] could not open" << uri << ":"
             << (err ? err : "no reason given");
  }
}

InputStream::~InputStream() noexcept
{
  close();
  releaseCodecs();
}

uint32_t InputStream::activeStreams() const noexcept
{
  if(!m_device || !m_backend)
    return 0;
  if(!m_backend->active_streams)
    return ~0u; // a backend that does not say is taken at its word
  return m_backend->active_streams(m_device);
}

void InputStream::close() noexcept
{
  stop();
  if(m_device && m_backend && m_backend->close)
    m_backend->close(m_device);
  m_device = nullptr;
}

bool InputStream::start() noexcept
{
  if(!m_device || !m_backend || !m_backend->start)
    return false;
  if(m_running.load(std::memory_order_acquire))
    return true;

  m_stopVotes.store(0, std::memory_order_relaxed);
  m_running.store(true, std::memory_order_release);

  if(!m_backend->start(m_device, &InputStream::onFrameThunk, this))
  {
    const char* err = m_backend->last_error ? m_backend->last_error() : nullptr;
    qDebug() << "[depthcam] could not start streaming:"
             << (err ? err : "no reason given");
    m_running.store(false, std::memory_order_release);
    return false;
  }
  return true;
}

void InputStream::stop() noexcept
{
  if(!m_running.exchange(false, std::memory_order_acq_rel))
    return;

  if(m_backend && m_backend->stop && m_device)
    m_backend->stop(m_device);

  m_rgb.queue.drain();
  m_ir.queue.drain();
  m_depth.queue.drain();
  m_pcl.queue.drain();
}

void InputStream::requestStop() noexcept
{
  if(m_stopVotes.fetch_add(1) + 1 >= m_extractors.load())
    stop();
}

void InputStream::onFrameThunk(const depthcam_frame* f, void* user)
{
  if(f && user)
    static_cast<InputStream*>(user)->onFrame(*f);
}

void InputStream::onFrame(const depthcam_frame& f)
{
  if(!m_running.load(std::memory_order_acquire))
    return;
  if(!f.data || f.bytes == 0)
    return;

  switch(f.stream)
  {
    case DEPTHCAM_STREAM_COLOR:
      handleImage(m_rgb, f);
      break;
    case DEPTHCAM_STREAM_IR:
      handleImage(m_ir, f);
      break;
    case DEPTHCAM_STREAM_DEPTH:
      if(f.depth_unit_mm > 0.f)
        depth_unit_mm.store(f.depth_unit_mm, std::memory_order_relaxed);
      handleImage(m_depth, f);
      break;
    case DEPTHCAM_STREAM_POINTCLOUD:
      handlePointCloud(f);
      break;
    case DEPTHCAM_STREAM_IMU:
      handleImu(f);
      break;
    default:
      // Unknown stream: still hand the buffer back.
      if(f.release)
        f.release(f.owner);
      break;
  }
}

void InputStream::setImuCallback(imu_callback cb) noexcept
{
  std::lock_guard lock{m_imu_mutex};
  m_imu_cb = std::move(cb);
}

void InputStream::handleImu(const depthcam_frame& f)
{
  // Not queued like the image streams: an inertial sample is 32 bytes and is
  // only interesting while it is current. Queuing would add latency to the one
  // stream here where latency is the whole point, and a listener that fell
  // behind would then be fed history.
  if(f.format == DEPTHCAM_FMT_IMU && f.bytes >= sizeof(depthcam_imu_sample))
  {
    depthcam_imu_sample sample{};
    std::memcpy(&sample, f.data, sizeof(sample));

    std::lock_guard lock{m_imu_mutex};
    if(m_imu_cb)
      m_imu_cb(sample);
  }

  if(f.release)
    f.release(f.owner);
}

void InputStream::handleImage(StreamOutput& out, const depthcam_frame& f)
{
  // Every path below must either take ownership of the buffer or release it,
  // exactly once.
  if(out.queue.size() >= max_queued_frames)
  {
    if(f.release)
      f.release(f.owner);
    return;
  }

  if(const auto codec = to_codec_id(f.format); codec != AV_CODEC_ID_NONE)
  {
    AVFrame* decoded = decodeCompressed(f);
    if(f.release)
      f.release(f.owner); // the decoder copied what it needed
    if(decoded)
    {
      out.width.store(decoded->width, std::memory_order_relaxed);
      out.height.store(decoded->height, std::memory_order_relaxed);
      out.format.store(decoded->format, std::memory_order_relaxed);
      out.queue.enqueue(decoded);
    }
    return;
  }

  const auto pixfmt = to_av_format(f.format);
  if(pixfmt == AV_PIX_FMT_NONE)
  {
    if(f.release)
      f.release(f.owner);
    return;
  }

  ::Video::AVFramePointer frame = out.queue.newFrame();
  frame->format = pixfmt;
  frame->width = f.width;
  frame->height = f.height;

  if(!wrap_borrowed(*frame, f))
  {
    if(f.release)
      f.release(f.owner);
    return;
  }

  // Fill in the remaining planes/strides for the borrowed buffer. The frame
  // already owns its reference, so a failure here still releases correctly when
  // the AVFramePointer goes out of scope.
  if(!::Video::initFrameFromRawData(frame.get(), frame->data[0], f.bytes))
    return;

  if(f.stride > 0)
    frame->linesize[0] = f.stride;

  out.width.store(f.width, std::memory_order_relaxed);
  out.height.store(f.height, std::memory_order_relaxed);
  out.format.store(int(pixfmt), std::memory_order_relaxed);

  out.queue.enqueue(frame.release());
}

void InputStream::handlePointCloud(const depthcam_frame& f)
{
  if(m_pcl.queue.size() >= max_queued_pointclouds)
  {
    if(f.release)
      f.release(f.owner);
    return;
  }

  const int floats_per_point = (f.format == DEPTHCAM_FMT_XYZRGB) ? 6 : 3;
  int points = f.point_count;
  if(points <= 0)
    points = int(f.bytes / (floats_per_point * sizeof(float)));
  if(points <= 0)
  {
    if(f.release)
      f.release(f.owner);
    return;
  }

  ::Video::AVFramePointer frame = m_pcl.queue.newFrame();

  // A point cloud is not an image, but it rides the same queue as the textures.
  // width carries the point count and height the floats per point, so the
  // consumer never re-derives them from a byte size.
  frame->format = AV_PIX_FMT_NONE;
  frame->width = points;
  frame->height = floats_per_point;
  frame->linesize[0] = int(f.bytes);

  if(!wrap_borrowed(*frame, f))
  {
    if(f.release)
      f.release(f.owner);
    return;
  }

  m_pcl.queue.enqueue(frame.release());
}

AVCodecContext* InputStream::codecContext(AVCodecID id)
{
  AVCodecContext** slot = nullptr;
  switch(id)
  {
    case AV_CODEC_ID_MJPEG:
      slot = &m_codecMJPEG;
      break;
    case AV_CODEC_ID_H264:
      slot = &m_codec264;
      break;
    case AV_CODEC_ID_HEVC:
      slot = &m_codec265;
      break;
    default:
      return nullptr;
  }

  if(*slot)
    return *slot;

  const AVCodec* codec = avcodec_find_decoder(id);
  if(!codec)
    return nullptr;

  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  if(!ctx)
    return nullptr;
  if(avcodec_open2(ctx, codec, nullptr) < 0)
  {
    avcodec_free_context(&ctx);
    return nullptr;
  }

  *slot = ctx;
  return ctx;
}

void InputStream::releaseCodecs()
{
  for(auto* slot : {&m_codecMJPEG, &m_codec264, &m_codec265})
    if(*slot)
      avcodec_free_context(slot);
  if(m_packet)
    av_packet_free(&m_packet);
}

AVFrame* InputStream::decodeCompressed(const depthcam_frame& f)
{
  AVCodecContext* ctx = codecContext(to_codec_id(f.format));
  if(!ctx)
    return nullptr;

  // av_init_packet was deprecated in FFmpeg 5 and removed in 7.
  if(!m_packet)
  {
    m_packet = av_packet_alloc();
    if(!m_packet)
      return nullptr;
  }

  m_packet->data
      = const_cast<uint8_t*>(static_cast<const uint8_t*>(f.data));
  m_packet->size = int(f.bytes);

  if(avcodec_send_packet(ctx, m_packet) < 0)
    return nullptr;

  AVFrame* frame = av_frame_alloc();
  if(!frame)
    return nullptr;
  if(avcodec_receive_frame(ctx, frame) < 0)
  {
    av_frame_free(&frame);
    return nullptr;
  }
  return frame;
}

InputStreamExtractor::InputStreamExtractor(
    std::shared_ptr<InputStream> s, StreamOutput& out, QString f) noexcept
    : filter{std::move(f)}
    , m_stream{std::move(s)}
    , m_out{out}
{
  m_stream->registerExtractor();
  this->realTime = true;
  this->dts_per_flicks = 0;
  this->flicks_per_dts = 0;
  refreshMetadata();
}

InputStreamExtractor::~InputStreamExtractor() = default;

void InputStreamExtractor::refreshMetadata() noexcept
{
  this->width = m_out.width.load(std::memory_order_relaxed);
  this->height = m_out.height.load(std::memory_order_relaxed);
  this->fps = m_out.fps.load(std::memory_order_relaxed);
  this->pixel_format
      = static_cast<AVPixelFormat>(m_out.format.load(std::memory_order_relaxed));
}

bool InputStreamExtractor::start() noexcept
{
  const bool ok = m_stream->start();
  refreshMetadata();
  return ok;
}

void InputStreamExtractor::stop() noexcept
{
  // Not m_stream->stop(): the other nodes may still be rendering from the same
  // camera.
  m_stream->requestStop();
}

AVFrame* InputStreamExtractor::dequeue_frame() noexcept
{
  return m_out.queue.dequeue();
}

void InputStreamExtractor::release_frame(AVFrame* frame) noexcept
{
  m_out.queue.release(frame);
}

}
