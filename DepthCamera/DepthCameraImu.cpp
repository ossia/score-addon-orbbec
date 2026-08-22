#include "DepthCameraImu.hpp"

#include <ossia/network/base/node_attributes.hpp>
#include <ossia/network/domain/domain.hpp>

#include <DepthCamera/DepthCameraStream.hpp>

#include <cmath>

namespace Gfx::DepthCamera
{
namespace
{

ossia::net::parameter_base* makeVec3(
    ossia::net::node_base& parent, const char* name, const char* description,
    float bound, const char* unit)
{
  auto* node = parent.create_child(name);
  if(!node)
    return nullptr;

  auto* param = node->create_parameter(ossia::val_type::VEC3F);
  if(!param)
    return nullptr;

  // A domain, even though nothing clamps to it: it is what tells a slider or an
  // automation curve what range to draw. The bound is the sensor's largest
  // full-scale setting, so a camera configured for a smaller one simply never
  // reaches the edges.
  param->set_domain(ossia::make_domain(-bound, bound));
  param->set_access(ossia::access_mode::GET);
  param->set_value_quiet(ossia::value{ossia::vec3f{0.f, 0.f, 0.f}});
  ossia::net::set_description(*node, description);
  ossia::net::set_unit(*node, ossia::parse_pretty_unit(unit));
  return param;
}

}

ImuTree::ImuTree(
    const std::shared_ptr<InputStream>& stream, ossia::net::device_base& dev,
    ossia::net::node_base& parent)
    : m_stream{stream}
{
  if(!m_stream)
    return;

  auto* root = parent.find_child(std::string_view{"imu"});
  if(!root)
    root = parent.create_child("imu");
  if(!root)
    return;

  // The bounds are the widest full scale any of these cameras offers: 16 g for
  // the accelerometer, 2000 degrees per second for the gyroscope.
  m_accel = makeVec3(
      *root, "accel", "Acceleration in metres per second squared, including "
                      "gravity",
      16.f * 9.80665f, "distance.m/s2");
  m_gyro = makeVec3(
      *root, "gyro", "Angular velocity in radians per second",
      float(2000. * M_PI / 180.), "");

  if(auto* node = root->create_child("temperature"))
  {
    if((m_temperature = node->create_parameter(ossia::val_type::FLOAT)))
    {
      m_temperature->set_domain(ossia::make_domain(-40.f, 125.f));
      m_temperature->set_access(ossia::access_mode::GET);
      m_temperature->set_value_quiet(ossia::value{0.f});
      ossia::net::set_description(*node, "Sensor temperature in Celsius");
    }
  }

  m_stream->setImuCallback([this](const depthcam_imu_sample& s) { onSample(s); });
}

ImuTree::~ImuTree()
{
  // Blocks until any callback in flight returns; see InputStream::setImuCallback.
  if(m_stream)
    m_stream->setImuCallback({});
}

void ImuTree::onSample(const depthcam_imu_sample& s)
{
  // Camera thread. set_value, not push_value: the camera is telling us, and
  // pushing would send it back out through the protocol.
  if((s.fields & DEPTHCAM_IMU_ACCEL) && m_accel)
    m_accel->set_value(ossia::vec3f{s.accel[0], s.accel[1], s.accel[2]});

  if((s.fields & DEPTHCAM_IMU_GYRO) && m_gyro)
    m_gyro->set_value(ossia::vec3f{s.gyro[0], s.gyro[1], s.gyro[2]});

  if((s.fields & DEPTHCAM_IMU_TEMPERATURE) && m_temperature
     && std::isfinite(s.temperature_c))
    m_temperature->set_value(float(s.temperature_c));
}

}
