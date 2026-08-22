#include "DepthCameraSettings.hpp"

#include <score/serialization/DataStreamVisitor.hpp>
#include <score/serialization/JSONVisitor.hpp>

namespace Gfx::DepthCamera
{

QString DepthCameraSettings::backend() const
{
  const auto s = device.trimmed();
  const int colon = s.indexOf(':');
  return colon > 0 ? s.left(colon) : QString{};
}

}

template <>
void DataStreamReader::read(const Gfx::DepthCamera::DepthCameraSettings& n)
{
  m_stream << n.device << n.rgb << n.ir << n.depth << n.pointcloud
           << n.colorPointcloud << static_cast<int32_t>(n.align) << n.colorWidth
           << n.colorHeight << n.colorFps << n.depthWidth << n.depthHeight
           << n.depthFps << n.imu << n.irWidth << n.irHeight
           << n.irFps;
  insertDelimiter();
}

template <>
void DataStreamWriter::write(Gfx::DepthCamera::DepthCameraSettings& n)
{
  int32_t align{};
  m_stream >> n.device >> n.rgb >> n.ir >> n.depth >> n.pointcloud
      >> n.colorPointcloud >> align >> n.colorWidth >> n.colorHeight >> n.colorFps
      >> n.depthWidth >> n.depthHeight >> n.depthFps >> n.imu >> n.irWidth >> n.irHeight
      >> n.irFps;
  n.align = static_cast<Gfx::DepthCamera::DepthCameraSettings::AlignMode>(align);
  checkDelimiter();
}

template <>
void JSONReader::read(const Gfx::DepthCamera::DepthCameraSettings& n)
{
  obj["Device"] = n.device;
  obj["RGB"] = n.rgb;
  obj["IR"] = n.ir;
  obj["Depth"] = n.depth;
  obj["Pointcloud"] = n.pointcloud;
  obj["ColorPointcloud"] = n.colorPointcloud;
  obj["Align"] = static_cast<int>(n.align);
  obj["ColorWidth"] = n.colorWidth;
  obj["ColorHeight"] = n.colorHeight;
  obj["ColorFps"] = n.colorFps;
  obj["DepthWidth"] = n.depthWidth;
  obj["DepthHeight"] = n.depthHeight;
  obj["DepthFps"] = n.depthFps;
  obj["Imu"] = n.imu;
  obj["IRWidth"] = n.irWidth;
  obj["IRHeight"] = n.irHeight;
  obj["IRFps"] = n.irFps;
}

template <>
void JSONWriter::write(Gfx::DepthCamera::DepthCameraSettings& n)
{
  // tryGet, not obj[...]: rapidjson's operator[] *asserts* on a missing key,
  // which aborts the whole process. That is reachable from ordinary use --
  // Score.createDevice() in a JS script passes whatever subset of fields the
  // author wrote, and a document saved by an older build has no Align key at
  // all. Anything absent keeps the default from the struct.
  if(auto v = obj.tryGet("Device"))
    n.device = v->toString();
  if(auto v = obj.tryGet("RGB"))
    n.rgb = v->toBool();
  if(auto v = obj.tryGet("IR"))
    n.ir = v->toBool();
  if(auto v = obj.tryGet("Depth"))
    n.depth = v->toBool();
  if(auto v = obj.tryGet("Pointcloud"))
    n.pointcloud = v->toBool();
  if(auto v = obj.tryGet("ColorPointcloud"))
    n.colorPointcloud = v->toBool();
  if(auto v = obj.tryGet("Align"))
    n.align
        = static_cast<Gfx::DepthCamera::DepthCameraSettings::AlignMode>(v->toInt());
  if(auto v = obj.tryGet("ColorWidth"))
    n.colorWidth = v->toInt();
  if(auto v = obj.tryGet("ColorHeight"))
    n.colorHeight = v->toInt();
  if(auto v = obj.tryGet("ColorFps"))
    n.colorFps = v->toInt();
  if(auto v = obj.tryGet("DepthWidth"))
    n.depthWidth = v->toInt();
  if(auto v = obj.tryGet("DepthHeight"))
    n.depthHeight = v->toInt();
  if(auto v = obj.tryGet("DepthFps"))
    n.depthFps = v->toInt();
  if(auto v = obj.tryGet("Imu"))
    n.imu = v->toBool();
  if(auto v = obj.tryGet("IRWidth"))
    n.irWidth = v->toInt();
  if(auto v = obj.tryGet("IRHeight"))
    n.irHeight = v->toInt();
  if(auto v = obj.tryGet("IRFps"))
    n.irFps = v->toInt();
}
