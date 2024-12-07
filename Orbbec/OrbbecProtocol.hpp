#pragma once

#include <Device/Protocol/DeviceInterface.hpp>
#include <Device/Protocol/DeviceSettings.hpp>

#include <Gfx/GfxInputDevice.hpp>
#include <Gfx/SharedInputSettings.hpp>

#include <ossia/gfx/texture_parameter.hpp>
#include <ossia/network/generic/generic_device.hpp>
#include <ossia/network/generic/generic_node.hpp>
#include <ossia/network/base/protocol.hpp>


#include <ossia/gfx/texture_parameter.hpp>

#include <Gfx/GfxApplicationPlugin.hpp>
#include <Gfx/GfxExecContext.hpp>
#include <Gfx/Graph/VideoNode.hpp>
#include <Orbbec/OrbbecInputStream.hpp>

namespace Gfx::Orbbec
{
using orbbec_decoder = InputStream;
class orbbec_parameter : public ossia::gfx::texture_parameter
{
  GfxExecutionAction* context{};

public:
  std::shared_ptr<InputStream> decoder;
  int32_t node_id{};
  score::gfx::CameraNode* node{};

  orbbec_parameter(
      const std::shared_ptr<InputStream>& dec, ossia::net::node_base& n,
      GfxExecutionAction& ctx)
      : ossia::gfx::texture_parameter{n}
      , context{&ctx}
      , decoder{dec}
      , node{new score::gfx::CameraNode(decoder, QString(""))}
  {
    node_id = context->ui->register_node(std::unique_ptr<score::gfx::CameraNode>(node));
  }

  void pull_texture(port_index idx) override
  {
    context->setEdge(port_index{this->node_id, 0}, idx);

    score::gfx::Message m;
    m.node_id = node_id;
    context->ui->send_message(std::move(m));
  }

  virtual ~orbbec_parameter() { context->ui->unregister_node(node_id); }
};

class orbbec_node : public ossia::net::node_base
{
  ossia::net::device_base& m_device;
  node_base* m_parent{};
  std::unique_ptr<orbbec_parameter> m_parameter;

public:
  orbbec_node(
      const std::shared_ptr<orbbec_decoder>& settings, GfxExecutionAction& ctx,
      ossia::net::device_base& dev, std::string name)
      : m_device{dev}
      , m_parameter{std::make_unique<orbbec_parameter>(settings, *this, ctx)}
  {
    m_name = std::move(name);
  }

  orbbec_parameter* get_parameter() const override { return m_parameter.get(); }

private:
  ossia::net::device_base& get_device() const override { return m_device; }
  ossia::net::node_base* get_parent() const override { return m_parent; }
  ossia::net::node_base& set_name(std::string) override { return *this; }
  ossia::net::parameter_base* create_parameter(ossia::val_type) override
  {
    return m_parameter.get();
  }
  bool remove_parameter() override { return false; }

  std::unique_ptr<ossia::net::node_base> make_child(const std::string& name) override
  {
    return {};
  }
  void removing_child(ossia::net::node_base& node_base) override { }
};

class orbbec_protocol : public ossia::net::protocol_base
{
public:
  // orbbec_camera orbbec;
  explicit orbbec_protocol(const SharedInputSettings& stgs);

  bool pull(ossia::net::parameter_base&) override;
  bool push(const ossia::net::parameter_base&, const ossia::value& v) override;
  bool push_raw(const ossia::net::full_parameter_data&) override;
  bool observe(ossia::net::parameter_base&, bool) override;
  bool update(ossia::net::node_base& node_base) override;

  void start_execution() override;
  void stop_execution() override;
};

class orbbec_device : public ossia::net::generic_device
{
public:
  orbbec_device( std::shared_ptr<ob::Config> conf, std::shared_ptr<ob::Device> device,
      const SharedInputSettings& settings, GfxExecutionAction& ctx,
      std::unique_ptr<orbbec_protocol> proto, std::string name);
};

}

