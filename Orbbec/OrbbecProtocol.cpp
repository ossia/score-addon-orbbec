#include "OrbbecProtocol.hpp"

namespace Gfx::Orbbec{

orbbec_device::orbbec_device(
    const SharedInputSettings& settings, GfxExecutionAction& ctx,
    std::unique_ptr<orbbec_protocol> proto, std::string name)
    : ossia::net::generic_device{std::move(proto), name}
{
  auto& stream = static_cast<orbbec_protocol*>(m_protocol.get())->stream;
  // auto& k = ((orbbec_protocol*)m_protocol.get())->orbbec;
  //if(settings.rgb)
  {
    auto decoder = std::make_shared<InputStreamExtractor>(stream, stream->m_rgb_frames);
    this->add_child(
        std::make_unique<orbbec_node>(std::move(decoder), ctx, *this, "rgb"));
  }
  //if(settings.ir)
  {
    auto decoder = std::make_shared<InputStreamExtractor>(stream, stream->m_ir_frames);
    this->add_child(
        std::make_unique<orbbec_node>(std::move(decoder), ctx, *this, "ir"));
  }
  //if(settings.depth)
  {
    auto decoder
        = std::make_shared<InputStreamExtractor>(stream, stream->m_depth_frames);
    this->add_child(
        std::make_unique<orbbec_node>(std::move(decoder), ctx, *this, "depth"));
  }

  // if(settings.pointcloud)
  {
    auto decoder = std::make_shared<InputStreamExtractor>(stream, stream->m_pcl_frames);
    this->add_child(
        std::make_unique<orbbec_node>(std::move(decoder), ctx, *this, "pointcloud"));
  }
}

orbbec_protocol::orbbec_protocol(
    std::shared_ptr<ob::Config> config, std::shared_ptr<ob::Device> device,
    const SharedInputSettings& stgs)
    : ossia::net::protocol_base{flags{}}
{
  stream = std::make_shared<InputStream>(config, device);

  //  orbbec.load(stgs);
}

bool orbbec_protocol::pull(ossia::net::parameter_base&)
{
  return false;
}

bool orbbec_protocol::push(const ossia::net::parameter_base&, const ossia::value& v)
{
  return false;
}

bool orbbec_protocol::push_raw(const ossia::net::full_parameter_data&)
{
  return false;
}

bool orbbec_protocol::observe(ossia::net::parameter_base&, bool)
{
  return false;
}

bool orbbec_protocol::update(ossia::net::node_base& node_base)
{
  return false;
}

void orbbec_protocol::start_execution()
{
  stream->start();
}

void orbbec_protocol::stop_execution()
{
  stream->stop();
}

orbbec_parameter::orbbec_parameter(
    const std::shared_ptr<InputStreamExtractor>& dec, bool pcl, ossia::net::node_base& n,
    GfxExecutionAction& ctx)
    : ossia::gfx::texture_parameter{n}
    , context{&ctx}
    , decoder{dec}
    , node{
          !pcl ? (score::gfx::Node*)new score::gfx::CameraNode{decoder}
               : (score::gfx::Node*)new score::gfx::BufferNode{decoder}}
{
  node_id = context->ui->register_node(std::unique_ptr<score::gfx::Node>(node));
}
}
