#include "OrbbecProtocol.hpp"

namespace Gfx::Orbbec{


orbbec_device::orbbec_device( std::shared_ptr<ob::Config> config, std::shared_ptr<ob::Device> device,
    const SharedInputSettings& settings, GfxExecutionAction& ctx,
    std::unique_ptr<orbbec_protocol> proto, std::string name)
    : ossia::net::generic_device{std::move(proto), name}
{
  auto stream = std::make_shared<InputStream>(config, device);
  // auto& k = ((orbbec_protocol*)m_protocol.get())->orbbec;
  //if(settings.rgb)
  {
    auto decoder = std::shared_ptr<orbbec_decoder>(
        new orbbec_decoder{stream, stream.rgb, 1920, 1080, AV_PIX_FMT_BGR0, QString{}});
    this->add_child(
        std::make_unique<orbbec_node>(std::move(decoder), ctx, *this, "rgb"));
  }
  //if(settings.ir)
  {
    auto decoder = std::shared_ptr<orbbec_decoder>(new orbbec_decoder{
                                                                        k.irFrames, 512, 424, AV_PIX_FMT_GRAYF32LE,
                                                                        "float v = sqrt(tex.r / 65535); processed = vec3(v); processed.a = 1;"});
    this->add_child(
        std::make_unique<orbbec_node>(std::move(decoder), ctx, *this, "ir"));
  }
  //if(settings.depth)
  {
    auto decoder = std::shared_ptr<orbbec_decoder>(new orbbec_decoder{
                                                                        k.depthFrames, 512, 424, AV_PIX_FMT_GRAYF32LE,
                                                                        "float v = sqrt(tex.r / 65535); processed.rgb = vec3(v); processed.a = 1;"});
    this->add_child(
        std::make_unique<orbbec_node>(std::move(decoder), ctx, *this, "depth"));
  }
  // if(settings.rgb  && settings.depth)
  {
    // FIXME pcl
  }
}

orbbec_protocol::orbbec_protocol(const SharedInputSettings& stgs)
    : ossia::net::protocol_base{flags{}}
{
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
  orbbec.start();
}

void orbbec_protocol::stop_execution()
{
  orbbec.stop();
}

bool orbbec_decoder::start() noexcept
{
  return true;
}

void orbbec_decoder::stop() noexcept { }

}
}

