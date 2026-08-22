#include "DepthCameraControls.hpp"

#include <ossia/network/base/node_attributes.hpp>
#include <ossia/network/common/complex_type.hpp>
#include <ossia/network/domain/domain.hpp>

#include <ossia-qt/invoke.hpp>

#include <QDebug>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Gfx::DepthCamera
{
namespace
{

/// How often observed read-only controls are re-read.
///
/// Slow on purpose. Everything in this category is a temperature, a supply
/// voltage or an accelerometer, none of which move fast, and each read is a
/// transfer competing with the video stream -- a TCP round trip, for a camera
/// on the network. A camera nobody is listening to costs nothing at all: the
/// worker sleeps until something is observed.
constexpr auto poll_interval = std::chrono::milliseconds{500};

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
  m_descs.reserve(descs.size());

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

    auto e = std::make_unique<Entry>();
    e->id = c.id;
    e->param = param;
    e->kind = c.kind;
    e->access = c.access;

    // Only a control we cannot write is worth watching. Everything else we
    // already know the value of, because we are the one who set it, and
    // re-reading forty of those twice a second is a stream of transfers
    // competing with the video for no information at all.
    e->pollable = (c.access & DEPTHCAM_ACCESS_READ)
                  && !(c.access & DEPTHCAM_ACCESS_WRITE)
                  && c.kind != DEPTHCAM_CONTROL_ACTION;

    // The backend's default, not the camera's current value.
    //
    // Reading every control here would be forty synchronous transfers inside
    // reconnect() on the GUI thread -- perceptible over USB, seconds over
    // Ethernet. The worker re-reads them all as its first job, so the real
    // values arrive a moment later without anything having blocked.
    //
    // Every parameter gets *a* value, without exception: a parameter holding
    // ossia::value{} has no type, and anything that walks the whole tree --
    // deviceToJson, an OSCquery export, a preset save -- throws
    // "value_to_json_value: no type" on the first one it meets, which takes the
    // application down.
    const double v = c.def;
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

    // Enum labels belong to the backend and stay valid until close, which is
    // after this object dies, so keeping the descriptor is safe.
    m_descs.push_back(c);
    e->desc = int(m_descs.size()) - 1;

    m_entries.push_back(std::move(e));
  }

  if(m_entries.empty())
    return;

  m_running.store(true, std::memory_order_release);
  m_worker = std::thread{[this] { run(); }};

  // First job: the values the constructor did not wait for.
  refresh();
}

ControlTree::~ControlTree()
{
  // The worker first, and joined, not just signalled: it reads through
  // m_backend at a device that is closed as soon as this returns.
  if(m_running.exchange(false, std::memory_order_acq_rel))
  {
    wake();
    if(m_worker.joinable())
      m_worker.join();
  }

  // Deliberately *not* callbacks_clear() on the parameters, unlike the V4L2
  // control tree this is modelled on: nothing here ever installs a callback.
  // Writes arrive through depthcam_protocol::push, which the device clears
  // before destroying this object. And the parameters cannot be touched here
  // anyway -- Device::DeviceInterface::disconnect() calls
  // root.clear_children() on the way down, so by the time this runs on a
  // reconnect or a device removal they are already gone.
  //
  // Anything the worker posted to m_context and that has not run yet dies with
  // it: ~QObject drops a QObject's undelivered events.
}

int ControlTree::indexOf(const ossia::net::parameter_base& param) noexcept
{
  for(std::size_t i = 0; i < m_entries.size(); i++)
    if(m_entries[i]->param == &param)
      return int(i);
  return -1;
}

ControlTree::Entry* ControlTree::find(const ossia::net::parameter_base& param) noexcept
{
  const int i = indexOf(param);
  return i >= 0 ? m_entries[std::size_t(i)].get() : nullptr;
}

void ControlTree::post(Request r)
{
  {
    std::lock_guard lk{m_queue_lock};
    if(!m_running.load(std::memory_order_acquire))
      return;

    // Collapse repeats. An automation curve writing at audio rate would
    // otherwise pile up thousands of requests the camera can never keep up
    // with, and only the last one was ever going to matter.
    for(auto& p : m_pending)
    {
      if(p.entry == r.entry && p.is_write == r.is_write)
      {
        p.value = r.value;
        return;
      }
    }
    m_pending.push_back(r);
  }
  m_queue_cv.notify_one();
}

void ControlTree::wake()
{
  std::lock_guard lk{m_queue_lock};
  m_queue_cv.notify_one();
}

void ControlTree::run()
{
  std::vector<Request> batch;
  std::vector<std::pair<int, double>> results;

  while(m_running.load(std::memory_order_acquire))
  {
    {
      std::unique_lock lk{m_queue_lock};
      if(m_pending.empty())
      {
        // Sleep until there is something to do. With nothing observed there is
        // no timeout at all, so an idle camera is not touched.
        if(m_polled.load(std::memory_order_relaxed) > 0)
          m_queue_cv.wait_for(lk, poll_interval);
        else
          m_queue_cv.wait(lk);
      }
      batch.swap(m_pending);
      m_pending.clear();
    }

    if(!m_running.load(std::memory_order_acquire))
      break;

    results.clear();

    for(const auto& r : batch)
    {
      auto& e = *m_entries[std::size_t(r.entry)];
      if(r.is_write)
      {
        if(m_backend.set_control)
          m_backend.set_control(&m_device, e.id.c_str(), r.value);

        // Read back what the camera settled on rather than trusting the write:
        // ranges are quantised (an Orbbec white balance snaps to 100K steps)
        // and some settings are refused outright while an `auto` is on.
        double got{};
        if((e.access & DEPTHCAM_ACCESS_READ) && m_backend.get_control
           && m_backend.get_control(&m_device, e.id.c_str(), &got))
          results.emplace_back(r.entry, got);
      }
      else if((e.access & DEPTHCAM_ACCESS_READ) && m_backend.get_control)
      {
        double got{};
        if(m_backend.get_control(&m_device, e.id.c_str(), &got))
          results.emplace_back(r.entry, got);
      }
    }
    batch.clear();

    // Then the sensors anything is listening to.
    if(m_polled.load(std::memory_order_relaxed) > 0 && m_backend.get_control)
    {
      for(std::size_t i = 0; i < m_entries.size(); i++)
      {
        auto& e = *m_entries[i];
        if(!e.pollable || e.observers.load(std::memory_order_relaxed) <= 0)
          continue;
        double got{};
        if(m_backend.get_control(&m_device, e.id.c_str(), &got))
          results.emplace_back(int(i), got);
      }
    }

    if(results.empty())
      continue;

    // Published from the Qt thread: this runs the parameters' callbacks, which
    // reach the explorer, the execution engine and anything listening over OSC.
    ossia::qt::run_async(&m_context, [this, values = results] {
      for(auto& [idx, v] : values)
        publish(*m_entries[std::size_t(idx)], v);
    });
  }
}

bool ControlTree::write(const ossia::net::parameter_base& param, const ossia::value& v)
{
  const int idx = indexOf(param);
  if(idx < 0)
    return false;

  auto& e = *m_entries[std::size_t(idx)];
  if(!(e.access & DEPTHCAM_ACCESS_WRITE) || !m_backend.set_control)
    return false;

  const auto& c = m_descs[std::size_t(e.desc)];

  double raw{};
  switch(e.kind)
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

  // Queued, not sent. This is called from the GUI thread, from the execution
  // engine and from whichever thread an OSC message arrived on, and a write is
  // a synchronous transfer to the camera.
  post({.entry = idx, .value = raw, .is_write = true});

  // The tree now shows what was asked for. If the camera clamps or rounds it,
  // the worker's read-back corrects it -- which cannot be done here, because
  // this runs inside the parameter's own callback and callback_container::send
  // holds a non-recursive mutex across it.
  e.last = raw;
  e.has_last = true;
  return true;
}

bool ControlTree::read(ossia::net::parameter_base& param)
{
  const int idx = indexOf(param);
  if(idx < 0)
    return false;

  auto& e = *m_entries[std::size_t(idx)];
  if(!(e.access & DEPTHCAM_ACCESS_READ) || !m_backend.get_control)
    return false;

  // The parameter already holds the last known value, so a pull has nothing to
  // wait for; this only asks for a fresher one.
  post({.entry = idx, .is_write = false});
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
  if(!e->pollable)
    return true; // ours, but nothing to watch for

  const int before = e->observers.load(std::memory_order_relaxed);
  const int after = std::max(0, before + (enable ? 1 : -1));
  e->observers.store(after, std::memory_order_relaxed);

  if(before == 0 && after > 0)
  {
    if(m_polled.fetch_add(1, std::memory_order_relaxed) == 0)
      wake();
  }
  else if(before > 0 && after == 0)
  {
    m_polled.fetch_sub(1, std::memory_order_relaxed);
  }
  return true;
}

void ControlTree::refresh()
{
  for(std::size_t i = 0; i < m_entries.size(); i++)
  {
    const auto& e = *m_entries[i];
    if((e.access & DEPTHCAM_ACCESS_READ) && e.kind != DEPTHCAM_CONTROL_ACTION)
      post({.entry = int(i), .is_write = false});
  }
}

}
