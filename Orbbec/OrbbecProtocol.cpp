#include "OrbbecProtocol.hpp"


#include <Crousti/Executor.hpp>
#include <halp/meta.hpp>
#include <halp/controls.hpp>
#include <halp/geometry.hpp>
#include <halp/texture.hpp>
#include <Threedim/TinyObj.hpp>
#include <libobsensor/hpp/Frame.hpp>
#include <libobsensor/hpp/Sensor.hpp>
#include <libobsensor/hpp/Device.hpp>


namespace oscr
{

template <typename Node_T>
struct DeviceNode;
template <typename Node_T>
struct DeviceRenderer final : score::gfx::GenericNodeRenderer
{
  std::shared_ptr<Node_T> state;
  score::gfx::Message m_last_message{};
  ossia::time_value m_last_time{-1};

  AVND_NO_UNIQUE_ADDRESS texture_inputs_storage<Node_T> texture_ins;
  AVND_NO_UNIQUE_ADDRESS texture_outputs_storage<Node_T> texture_outs;

  AVND_NO_UNIQUE_ADDRESS buffer_inputs_storage<Node_T> buffer_ins;
  AVND_NO_UNIQUE_ADDRESS buffer_outputs_storage<Node_T> buffer_outs;

  AVND_NO_UNIQUE_ADDRESS geometry_inputs_storage<Node_T> geometry_ins;
  AVND_NO_UNIQUE_ADDRESS geometry_outputs_storage<Node_T> geometry_outs;

  const DeviceNode<Node_T>& node() const noexcept
  {
    return static_cast<const DeviceNode<Node_T>&>(score::gfx::NodeRenderer::node);
  }

  DeviceRenderer(const DeviceNode<Node_T>& p)
      : score::gfx::GenericNodeRenderer{p}
      , state{std::make_shared<Node_T>()}
  {
    prepareNewState<Node_T>(state, p);
  }

  score::gfx::TextureRenderTarget
  renderTargetForInput(const score::gfx::Port& p) override
  {
    if constexpr(avnd::texture_input_introspection<Node_T>::size > 0)
    {
      auto it = texture_ins.m_rts.find(&p);
      SCORE_ASSERT(it != texture_ins.m_rts.end());
      return it->second;
    }
    return {};
  }

  score::gfx::BufferView bufferForOutput(const score::gfx::Port& output) override
  {
    if constexpr(avnd::buffer_output_introspection<Node_T>::size > 0)
    {
      for(auto& [p, b] : buffer_outs.m_buffers)
        if(p == &output)
          return b;
    }
    return {};
  }

  QRhiTexture* textureForOutput(const score::gfx::Port& output) override
  {
    if constexpr(avnd::gpu_texture_output_introspection<Node_T>::size > 0)
    {
      // Find which output port index this is
      const auto& outputs = this->node().output;
      int port_idx = -1;
      for(int i = 0, n = outputs.size(); i < n; i++)
      {
        if(outputs[i] == &output)
        {
          port_idx = i;
          break;
        }
      }
      if(port_idx < 0)
        return nullptr;

      // Walk gpu_texture outputs; for_all_n2 gives us both the
      // predicate index and the struct field index (== port index)
      QRhiTexture* result = nullptr;
      avnd::gpu_texture_output_introspection<Node_T>::for_all_n2(
          avnd::get_outputs<Node_T>(*state),
          [&]<std::size_t PredIdx, std::size_t FieldIdx>(
              auto& field, avnd::predicate_index<PredIdx>,
              avnd::field_index<FieldIdx>) {
        if(static_cast<int>(FieldIdx) == port_idx)
          result = static_cast<QRhiTexture*>(field.texture.handle);
      });

      return result;
    }
    return nullptr;
  }

  void init(score::gfx::RenderList& renderer, QRhiResourceUpdateBatch& res) override
  {
    auto& parent = node();
    if constexpr(requires { state->prepare(); })
    {
      parent.processControlIn(
          *this, *state, m_last_message, parent.last_message, parent.m_ctx);
      state->prepare();
    }

    // Init input render targets
    if constexpr(avnd::texture_input_introspection<Node_T>::size > 0)
      texture_ins.init(*this, renderer);

    if constexpr(avnd::texture_output_introspection<Node_T>::size > 0)
      texture_outs.init(*this, renderer, res);

    if constexpr(avnd::buffer_output_introspection<Node_T>::size > 0)
      buffer_outs.init(renderer, *state, parent);

    if_possible(state->init(renderer, res));
  }

  void update(
      score::gfx::RenderList& renderer, QRhiResourceUpdateBatch& res,
      score::gfx::Edge* e) override
  {
    auto& parent = node();
   // parent.processControlIn(
   //     *this, *state, m_last_message, parent.last_message, parent.m_ctx);

    bool updated = false;
    if constexpr(avnd::texture_input_introspection<Node_T>::size > 0)
    {
      updated |= texture_ins.update(*this, renderer, res);
    }

    if constexpr(avnd::texture_output_introspection<Node_T>::size > 0)
    {
      this->defaultUBOUpdate(renderer, res);
    }

    if_possible(state->update(renderer, res, e));

    if(updated)
    {
      // We must notify the graph that the previous nodes have to be recomputed
    }
  }

  void release(score::gfx::RenderList& r) override
  {
    if constexpr(avnd::texture_input_introspection<Node_T>::size > 0)
      texture_ins.release();

    if constexpr(avnd::texture_output_introspection<Node_T>::size > 0)
      texture_outs.release(*this, r);

    if constexpr(avnd::buffer_output_introspection<Node_T>::size > 0)
      buffer_outs.release(r);

    if constexpr(avnd::geometry_input_introspection<Node_T>::size > 0)
      geometry_ins.release(r);

    if constexpr(avnd::texture_input_introspection<Node_T>::size > 0 || avnd::texture_output_introspection<Node_T>::size > 0)
    {
      this->defaultRelease(r);
    }

    if_possible(state->release(r));
  }

  void inputAboutToFinish(
      score::gfx::RenderList& renderer, const score::gfx::Port& p,
      QRhiResourceUpdateBatch*& res) override
  {
    if constexpr(
        avnd::texture_input_introspection<Node_T>::size > 0
        || avnd::buffer_input_introspection<Node_T>::size > 0
        || avnd::geometry_input_introspection<Node_T>::size > 0)
    {
      res = renderer.state.rhi->nextResourceUpdateBatch();

      if constexpr(avnd::texture_input_introspection<Node_T>::size > 0)
        texture_ins.inputAboutToFinish(this->node(), p, res);
      if constexpr(avnd::buffer_input_introspection<Node_T>::size > 0)
        buffer_ins.inputAboutToFinish(renderer, res, *state, this->node());
      if constexpr(avnd::geometry_input_introspection<Node_T>::size > 0)
        geometry_ins.inputAboutToFinish(
            renderer, res, this->geometry, *state, this->node());
    }

    if_possible(state->inputAboutToFinish(renderer, p, res));
  }

  void runInitialPasses(
      score::gfx::RenderList& renderer, QRhiCommandBuffer& commands,
      QRhiResourceUpdateBatch*& res, score::gfx::Edge& edge) override
  {
    qDebug("HABIBI0");
    auto& parent = node();
    auto& rhi = *renderer.state.rhi;

    if constexpr(
        avnd::texture_input_introspection<Node_T>::size > 0
        || avnd::buffer_input_introspection<Node_T>::size > 0
        || avnd::geometry_input_introspection<Node_T>::size > 0)
    {
      // FIXME: for geometry, here we should optimize if we know we aren't going to need them on the CPU, OR if it is a type ?
      // Insert a synchronisation point to allow readbacks to complete
      rhi.finish();
    }

    // If we are paused, we don't run the processor implementation.
    // if(parent.last_message.token.date == m_last_time)
    //   return;
    // m_last_time = parent.last_message.token.date;

    if constexpr(avnd::texture_input_introspection<Node_T>::size > 0)
      texture_ins.runInitialPasses(*this, rhi);
    if constexpr(avnd::buffer_input_introspection<Node_T>::size > 0)
      buffer_ins.readInputBuffers(renderer, parent, *state);
    if constexpr(avnd::geometry_input_introspection<Node_T>::size > 0)
      geometry_ins.readInputGeometries(renderer, this->geometry, parent, *state);

    buffer_outs.prepareUpload(*res);

    // Run the processor
    if_possible(state->runInitialPasses(renderer, commands, res, edge));

    state->pcl = parent.dec->dequeue_frame();

    if_possible((*state)());

    // Upload output buffers
    if constexpr(avnd::buffer_output_introspection<Node_T>::size > 0)
      buffer_outs.upload(renderer, *state, *res);

    // Upload output textures
    if constexpr(avnd::texture_output_introspection<Node_T>::size > 0)
    {
      texture_outs.runInitialPasses(*this, renderer, res);

      commands.resourceUpdate(res);
      res = renderer.state.rhi->nextResourceUpdateBatch();
    }

    // Copy the geometry
    if constexpr(avnd::geometry_output_introspection<Node_T>::size > 0)
      geometry_outs.upload(renderer, *this->state, edge);

    // Copy the data to the model node
    // parent.processControlOut(*this->state);
  }
};

template <typename Node_T>
struct DeviceNode final
    : score::gfx::NodeModel
    , GpuNodeElements<Node_T>
{
  std::shared_ptr<Gfx::Orbbec::InputStreamExtractor> dec;
  DeviceNode(std::shared_ptr<Gfx::Orbbec::InputStreamExtractor> dec)
      : dec{dec}
  {
    qDebug("CALICE");
    initGfxPorts<Node_T>(this, this->input, this->output);
  }

  score::gfx::NodeRenderer*
  createRenderer(score::gfx::RenderList& r) const noexcept override
  {
    qDebug("TABARNAK");
    return new DeviceRenderer<Node_T>{*this};
  }
};
}

class PCLToMesh2
{
public:
  halp_meta(name, "Pointcloud to mesh")
  halp_meta(category, "Visuals/Meshes")
  halp_meta(c_name, "pointcloud_to_mesh")
  halp_meta(manual_url, "https://ossia.io/score-docs/processes/pointcloud-to-mesh.html")
  halp_meta(uuid, "2450ffbf-04ed-4b42-8848-69f200d2742a")

  enum BufferType
  {
    XYZ,
    XYZ_RGB,
    XYZW,
    XYZW_RGBA
  };
  struct ins
  {
    Threedim::PositionControl position;
    Threedim::RotationControl rotation;
    Threedim::ScaleControl scale;
    halp::enum_t<BufferType, "Buffer type"> type;
  } inputs;

  struct
  {
    struct
    {
      // Use Noiuse::dynamic_geometry
      halp_meta(name, "Geometry");
      halp::dynamic_geometry mesh;
      float transform[16]{};
      bool dirty_mesh = false;
      bool dirty_transform = false;
    } geometry;
  } outputs;

  AVFrame*  pcl{};

  PCLToMesh2()
  {
    Threedim::rebuild_transform(inputs, outputs);
    outputs.geometry.dirty_mesh = true;
  }

  void operator()()
  {
    if(!pcl)
      return;
    auto& tex = *pcl;

    // FIXME optimize
    // if (!tex.changed)
    //   return;

    // float* data = reinterpret_cast<float*>(tex.bytes);
    // create_mesh(std::span<float>(data, tex.bytesize / sizeof(float)));

    auto& mesh = outputs.geometry.mesh;
    auto& buffers = mesh.buffers;
    auto& bindings = mesh.bindings;
    auto& attributes = mesh.attributes;
    auto& inputs = mesh.input;

    mesh.topology = halp::primitive_topology::points;
    mesh.cull_mode = halp::cull_mode::none;
    mesh.front_face = halp::front_face::counter_clockwise;

    buffers.clear();
    bindings.clear();
    attributes.clear();
    inputs.clear();

    data.assign(pcl->data[0], pcl->data[0]+ pcl->linesize[0]);

    qDebug()<<data.size();
    // Buffers
    halp::geometry_cpu_buffer buf{};
    buf.raw_data = data.data();
    buf.byte_size = pcl->linesize[0];
    buf.dirty = true;
    buffers.push_back(buf);

    // Bindings
    int vertice_stride = 0;
    switch(this->inputs.type)
    {
      case XYZ:
        vertice_stride = 3;
        attributes.push_back(
            {.binding = 0,
             .semantic = halp::attribute_semantic::position,
             .format = halp::attribute_format::float3,
             .byte_offset = 0});
        break;
      case XYZ_RGB:
        vertice_stride = 6;
        attributes.push_back({
            .binding = 0,
            .semantic = halp::attribute_semantic::position,
            .format = halp::attribute_format::float3,
            .byte_offset = 0,
        });
        attributes.push_back({
            .binding = 0,
            .semantic = halp::attribute_semantic::color0,
            .format = halp::attribute_format::float3,
            .byte_offset = 3 * sizeof(float),
        });
        break;
      default:
        return;
    }

    bindings.push_back({
        .stride = vertice_stride * int(sizeof(float)),
        .step_rate = 1,
        .classification = halp::binding_classification::per_vertex
    });

    // Input. We have only one buffer so one input.
    // FIXME what when more than one buffer.
    struct halp::geometry_input xyz_input;
    xyz_input.buffer = 0;
    xyz_input.byte_offset = 0;
    inputs.push_back(xyz_input);

    // Vertices count.
    outputs.geometry.mesh.vertices = (pcl->linesize[0]/ (sizeof(float) * vertice_stride));

    outputs.geometry.dirty_mesh = true;
  }
  std::vector<char> data;

};

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
    , node{(score::gfx::Node*)new score::gfx::CameraNode{decoder}}
{
  node_id = context->ui->register_node(std::unique_ptr<score::gfx::Node>(node));
}

orbbec_pcl_parameter::orbbec_pcl_parameter(
    const std::shared_ptr<InputStreamExtractor>& dec, bool pcl, ossia::net::node_base& n,
    GfxExecutionAction& ctx)
    : ossia::gfx::geometry_parameter{n}
    , context{&ctx}
    , decoder{dec}
    , node{ (score::gfx::Node*)new oscr::DeviceNode<PCLToMesh2>{decoder}}
{
  node_id = context->ui->register_node(std::unique_ptr<score::gfx::Node>(node));
}
}
