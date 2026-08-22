#include "DepthCameraControls.hpp"

#include <ossia/network/base/node_attributes.hpp>
#include <ossia/network/common/complex_type.hpp>
#include <ossia/network/domain/domain.hpp>

#include <QDebug>

#include <algorithm>
#include <cmath>

namespace Gfx::DepthCamera
{
namespace
{

/// How often observed read-only controls are re-read.
///
/// Slow on purpose. Everything in this category is a temperature, a supply
/// voltage or an accelerometer, none of which move fast, and each read is a USB
/// control transfer competing with the video stream. A camera nobody is
/// listening to costs nothing at all: the timer only runs while at least one
/// control has an observer.
constexpr int poll_interval_ms = 500;

ossia::val_type type_for(const depthcam_control& c) noexcept
{
  switch(c.kind)
  {
    case DEPTHCAM_CONTROL_BOOL:
      return ossia::val_type::BOOL;
    case DEPTHCAM_CONTROL_ACTION:
      return ossia::val_type::IMPULSE;
    case DEPTHCAM_CONTROL_ENUM:
      // A named enum reads and writes as its label; an unnamed one is just an
      // int with a restricted range.
      return c.enum_labels && c.enum_count > 0 ? ossia::val_type::STRING
                                               : ossia::val_type::INT;
    case DEPTHCAM_CONTROL_FLOAT:
      return ossia::val_type::FLOAT;
    case DEPTHCAM_CONTROL_INT:
    default:
      return ossia::val_type::INT;
  }
}

ossia::domain domain_for(const depthcam_control& c)
{
  switch(c.kind)
  {
    case DEPTHCAM_CONTROL_ENUM: {
      if(c.enum_labels && c.enum_count > 0)
      {
        std::vector<std::string> labels;
        labels.reserve(std::size_t(c.enum_count));
        for(int i = 0; i < c.enum_count; i++)
          labels.push_back(c.enum_labels[i] ? c.enum_labels[i] : "");
        return ossia::make_domain(std::move(labels));
      }
      return ossia::make_domain(int(c.min), int(c.max));
    }
    case DEPTHCAM_CONTROL_INT:
      return ossia::make_domain(int(c.min), int(c.max));
    case DEPTHCAM_CONTROL_FLOAT:
      return ossia::make_domain(float(c.min), float(c.max));
    case DEPTHCAM_CONTROL_BOOL:
    case DEPTHCAM_CONTROL_ACTION:
    default:
      return {};
  }
}

ossia::access_mode access_for(const depthcam_control& c) noexcept
{
  const bool r = (c.access & DEPTHCAM_ACCESS_READ) != 0;
  const bool w = (c.access & DEPTHCAM_ACCESS_WRITE) != 0;
  if(r && w)
    return ossia::access_mode::BI;
  return w ? ossia::access_mode::SET : ossia::access_mode::GET;
}

/// The index an enum label stands for, given that labels are indexed from `min`
/// in steps of `step`.
int enum_index_of(const depthcam_control& c, int value) noexcept
{
  const double step = c.step > 0 ? c.step : 1;
  const int i = int(std::lround((value - c.min) / step));
  return (i >= 0 && i < c.enum_count) ? i : -1;
}

std::string description_for(const depthcam_control& c)
{
  std::string d = c.name ? c.name : "";
  if(c.description && *c.description)
  {
    if(!d.empty())
      d += " -- ";
    d += c.description;
  }
  if(c.kind == DEPTHCAM_CONTROL_ACTION)
    d += d.empty() ? "trigger" : " (trigger)";
  return d;
}

/// Walks `a/b/c`, creating what is missing. Returns the node the leaf goes in
/// and writes the leaf name to @p leaf.
ossia::net::node_base*
resolve_group(ossia::net::node_base& root, std::string_view id, std::string& leaf)
{
  ossia::net::node_base* cur = &root;

  for(;;)
  {
    const auto slash = id.find('/');
    if(slash == std::string_view::npos)
    {
      leaf = std::string{id};
      return leaf.empty() ? nullptr : cur;
    }

    const auto segment = id.substr(0, slash);
    id = id.substr(slash + 1);
    if(segment.empty())
      continue;

    // find_child before create_child: create_child uniquifies a name that is
    // already taken, so every control in a group would otherwise land in
    // "color", "color.1", "color.2"...
    auto* next = cur->find_child(segment);
    if(!next)
      next = cur->create_child(std::string{segment});
    if(!next)
      return nullptr;
    cur = next;
  }
}

} // namespace

ControlTree::ControlTree(
    const depthcam_backend_v1& backend, depthcam_device& device,
    ossia::net::device_base& dev, ossia::net::node_base& parent)
    : m_backend{backend}
    , m_device{device}
{
  if(!m_backend.list_controls)
    return;

  // Collected first, published second: list_controls hands out pointers into
  // the backend's own storage, and creating a node in the callback would mean
  // reentering the tree while the backend is walking its property list.
  std::vector<depthcam_control> descs;
  m_backend.list_controls(
      &m_device,
      [](const depthcam_control* c, void* user) {
    if(c && c->id && *c->id)
      static_cast<std::vector<depthcam_control>*>(user)->push_back(*c);
      },
      &descs);

  if(descs.empty())
    return;

  auto* root = parent.find_child(std::string_view{"controls"});
  if(!root)
    root = parent.create_child("controls");
  if(!root)
    return;

  m_entries.reserve(descs.size());

  for(const auto& c : descs)
  {
    std::string leaf;
    auto* group = resolve_group(*root, c.id, leaf);
    if(!group)
      continue;

    auto* node = group->create_child(leaf);
    if(!node)
      continue;

    auto* param = node->create_parameter(type_for(c));
    if(!param)
      continue;

    param->set_domain(domain_for(c));
    param->set_access(access_for(c));
    param->set_bounding(ossia::bounding_mode::CLIP);
    if(auto d = description_for(c); !d.empty())
      ossia::net::set_description(*node, std::move(d));

    Entry e;
    e.id = c.id;
    e.param = param;
    e.kind = c.kind;
    e.access = c.access;

    // Every parameter gets a value here, without exception. A parameter left
    // holding ossia::value{} has no type at all, and anything that walks the
    // whole tree -- deviceToJson, an OSCquery export, a preset save -- throws
    // "value_to_json_value: no type" on the first one it meets, which takes the
    // application down. The default is also the sensible thing to show for a
    // control the camera will not let us read.
    double v = c.def;
    if(c.kind != DEPTHCAM_CONTROL_ACTION && (c.access & DEPTHCAM_ACCESS_READ)
       && m_backend.get_control)
    {
      std::lock_guard lk{m_lock};
      double got{};
      if(m_backend.get_control(&m_device, c.id, &got))
        v = got;
    }

    // set_value_quiet, not push_value: publishing here would send the value
    // straight back to the camera through the protocol.
    switch(type_for(c))
    {
      case ossia::val_type::IMPULSE:
        param->set_value_quiet(ossia::value{ossia::impulse{}});
        break;
      case ossia::val_type::BOOL:
        param->set_value_quiet(ossia::value{v != 0.});
        break;
      case ossia::val_type::FLOAT:
        param->set_value_quiet(ossia::value{float(v)});
        break;
      case ossia::val_type::STRING: {
        const auto i = enum_index_of(c, int(v));
        param->set_value_quiet(ossia::value{
            std::string{i >= 0 && c.enum_labels[i] ? c.enum_labels[i] : ""}});
        break;
      }
      default:
        param->set_value_quiet(ossia::value{int(v)});
        break;
    }

    if(c.kind != DEPTHCAM_CONTROL_ACTION)
    {
      e.last = v;
      e.has_last = true;
    }

    // Enum labels belong to the backend and stay valid until close, which is
    // after this object dies, so keeping the descriptor is safe.
    m_descs.push_back(c);
    e.desc = int(m_descs.size()) - 1;

    m_entries.push_back(std::move(e));
  }
}

ControlTree::~ControlTree()
{
  // Just the timer, which reads through m_backend at a device that is closed as
  // soon as this returns.
  //
  // Deliberately *not* callbacks_clear() on the parameters, unlike the V4L2
  // control tree this is modelled on: nothing here ever installs a callback.
  // Writes arrive through depthcam_protocol::push, which the device clears
  // before destroying this object. And the parameters cannot be touched here
  // anyway -- Device::DeviceInterface::disconnect() calls
  // root.clear_children() on the way down, so by the time this runs on a
  // reconnect or a device removal they are already gone.
  m_timer.reset();
}

ControlTree::Entry* ControlTree::find(const ossia::net::parameter_base& param) noexcept
{
  for(auto& e : m_entries)
    if(e.param == &param)
      return &e;
  return nullptr;
}

bool ControlTree::write(const ossia::net::parameter_base& param, const ossia::value& v)
{
  auto* e = find(param);
  if(!e)
    return false;
  if(!(e->access & DEPTHCAM_ACCESS_WRITE) || !m_backend.set_control)
    return false;

  const auto& c = m_descs[std::size_t(e->desc)];

  double raw{};
  switch(e->kind)
  {
    case DEPTHCAM_CONTROL_ACTION:
      // No payload: any write fires it.
      raw = 1.;
      break;
    case DEPTHCAM_CONTROL_BOOL:
      raw = ossia::convert<bool>(v) ? 1. : 0.;
      break;
    case DEPTHCAM_CONTROL_ENUM: {
      if(c.enum_labels && c.enum_count > 0)
      {
        // A label is what the explorer offers, but an index is what OSC and
        // scripts tend to send; accept either.
        if(auto s = v.target<std::string>())
        {
          int found = -1;
          for(int i = 0; i < c.enum_count && found < 0; i++)
            if(c.enum_labels[i] && *s == c.enum_labels[i])
              found = i;
          if(found < 0)
            return false;
          raw = c.min + found * (c.step > 0 ? c.step : 1);
          break;
        }
      }
      raw = double(ossia::convert<int>(v));
      break;
    }
    case DEPTHCAM_CONTROL_FLOAT:
      raw = double(ossia::convert<float>(v));
      break;
    case DEPTHCAM_CONTROL_INT:
    default:
      raw = double(ossia::convert<int>(v));
      break;
  }

  std::lock_guard lk{m_lock};
  if(!m_backend.set_control(&m_device, e->id.c_str(), raw))
    return false;

  // Deliberately not read back and corrected here: this runs inside the
  // parameter's own callback, and callback_container::send holds a
  // non-recursive mutex across it, so pushing the corrected value would
  // deadlock whichever thread wrote -- the GUI included. A camera that clamped
  // or rounded shows up on the next poll instead.
  e->last = raw;
  e->has_last = true;
  return true;
}

bool ControlTree::read(ossia::net::parameter_base& param)
{
  auto* e = find(param);
  if(!e)
    return false;
  if(!(e->access & DEPTHCAM_ACCESS_READ) || !m_backend.get_control)
    return false;

  double v{};
  {
    std::lock_guard lk{m_lock};
    if(!m_backend.get_control(&m_device, e->id.c_str(), &v))
      return false;
  }
  publish(*e, v);
  return true;
}

void ControlTree::publish(Entry& e, double v)
{
  if(e.has_last && e.last == v)
    return;
  e.last = v;
  e.has_last = true;

  const auto& c = m_descs[std::size_t(e.desc)];

  // set_value rather than push_value: this is the camera telling us, so the
  // value must reach the listeners without being sent straight back.
  switch(e.kind)
  {
    case DEPTHCAM_CONTROL_BOOL:
      e.param->set_value(ossia::value{v != 0.});
      break;
    case DEPTHCAM_CONTROL_FLOAT:
      e.param->set_value(ossia::value{float(v)});
      break;
    case DEPTHCAM_CONTROL_ENUM:
      if(c.enum_labels && c.enum_count > 0)
      {
        const auto i = enum_index_of(c, int(v));
        if(i >= 0 && c.enum_labels[i])
          e.param->set_value(ossia::value{std::string{c.enum_labels[i]}});
        break;
      }
      [[fallthrough]];
    case DEPTHCAM_CONTROL_ACTION:
      break;
    default:
      e.param->set_value(ossia::value{int(v)});
      break;
  }
}

bool ControlTree::observe(const ossia::net::parameter_base& param, bool enable)
{
  auto* e = find(param);
  if(!e)
    return false;

  e->observers = std::max(0, e->observers + (enable ? 1 : -1));
  updateTimer();
  return true;
}

void ControlTree::updateTimer()
{
  const bool wanted = std::any_of(m_entries.begin(), m_entries.end(), [](const Entry& e) {
    return e.observers > 0 && (e.access & DEPTHCAM_ACCESS_READ)
           && e.kind != DEPTHCAM_CONTROL_ACTION;
  });

  if(!wanted)
  {
    m_timer.reset();
    return;
  }
  if(m_timer)
    return;
  if(!m_backend.get_control)
    return;

  m_timer = std::make_unique<QTimer>(&m_context);
  QObject::connect(m_timer.get(), &QTimer::timeout, &m_context, [this] { poll(); });
  m_timer->start(poll_interval_ms);
}

void ControlTree::poll()
{
  // Read everything under the lock, publish outside it: publishing runs the
  // parameters' callbacks, which can come straight back here to write.
  std::vector<std::pair<Entry*, double>> got;
  {
    std::lock_guard lk{m_lock};
    for(auto& e : m_entries)
    {
      if(e.observers <= 0 || !(e.access & DEPTHCAM_ACCESS_READ)
         || e.kind == DEPTHCAM_CONTROL_ACTION)
        continue;
      double v{};
      if(m_backend.get_control(&m_device, e.id.c_str(), &v))
        got.emplace_back(&e, v);
    }
  }

  for(auto& [e, v] : got)
    publish(*e, v);
}

void ControlTree::refresh()
{
  std::vector<std::pair<Entry*, double>> got;
  {
    std::lock_guard lk{m_lock};
    if(!m_backend.get_control)
      return;
    for(auto& e : m_entries)
    {
      if(!(e.access & DEPTHCAM_ACCESS_READ) || e.kind == DEPTHCAM_CONTROL_ACTION)
        continue;
      double v{};
      if(m_backend.get_control(&m_device, e.id.c_str(), &v))
        got.emplace_back(&e, v);
    }
  }

  for(auto& [e, v] : got)
    publish(*e, v);
}

}
