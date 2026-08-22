// Round-trips every writable control: read, write something else in range,
// read back, restore. A backend that reports a control it cannot actually set
// shows up here as a value that did not move.
#include <depthcam_abi.h>
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static std::vector<depthcam_control> ctls;
static void on_ctl(const depthcam_control* c, void*) { ctls.push_back(*c); }

int main(int argc, char** argv)
{
  void* lib = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
  if(!lib) { printf("%s\n", dlerror()); return 1; }
  const auto* b = ((score_depthcam_backend_v1_fn)dlsym(lib, "score_depthcam_backend_v1"))();
  if(!b->init(argv[2])) { printf("init failed\n"); return 1; }

  std::string uri;
  b->enumerate([](const depthcam_device_info* d, void* u) {
    if(((std::string*)u)->empty()) *(std::string*)u = d->uri; }, &uri);
  if(uri.empty()) { printf("no device\n"); return 1; }

  depthcam_open_config cfg{};
  cfg.streams = DEPTHCAM_STREAM_DEPTH;
  auto* dev = b->open(uri.c_str(), &cfg);
  if(!dev) { printf("open failed: %s\n", b->last_error()); return 1; }
  if(!b->list_controls) { printf("no controls\n"); return 0; }
  b->list_controls(dev, &on_ctl, nullptr);

  int tried = 0, moved = 0, refused = 0;
  for(auto& c : ctls)
  {
    if(!(c.access & DEPTHCAM_ACCESS_WRITE) || !(c.access & DEPTHCAM_ACCESS_READ))
      continue;
    if(c.kind == DEPTHCAM_CONTROL_ACTION)
      continue;   // firing a reboot during a test is not a good idea

    double before{};
    if(!b->get_control(dev, c.id, &before))
      continue;

    // Somewhere else in range, snapped to the step.
    const double step = c.step > 0 ? c.step : (c.max - c.min) / 4;
    double target = (before + step > c.max) ? before - step : before + step;
    if(target < c.min || target > c.max)
      continue;
    if(c.step > 0)
      target = c.min + std::round((target - c.min) / c.step) * c.step;
    if(target == before)
      continue;

    tried++;
    const bool ok = b->set_control(dev, c.id, target);
    double after{};
    b->get_control(dev, c.id, &after);

    if(!ok)
    {
      refused++;
      printf("  refused  %-38s %g -> %g (%s)\n", c.id, before, target,
             b->last_error() ? b->last_error() : "no reason");
    }
    else if(std::fabs(after - target) < 1e-6 || after != before)
    {
      moved++;
    }
    else
    {
      printf("  IGNORED  %-38s asked %g, still %g\n", c.id, target, after);
    }
    b->set_control(dev, c.id, before);
  }

  printf("%zu controls, %d writable tried: %d took, %d refused\n",
         ctls.size(), tried, moved, refused);
  b->close(dev); b->shutdown();
  return 0;
}
