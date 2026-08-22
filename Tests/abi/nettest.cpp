// Exercises the Orbbec backend's two network paths: broadcast enumeration and
// an explicit "orbbec:net:HOST:PORT" address.
#include <depthcam_abi.h>
#include <dlfcn.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <thread>

static std::atomic<int> n_color{0}, n_depth{0}, n_ir{0}, n_pcl{0};

static void on_frame(const depthcam_frame* f, void*)
{
  switch(f->stream)
  {
    case DEPTHCAM_STREAM_COLOR:
      if(n_color++ == 0) printf("  color %dx%d fmt=%d\n", f->width, f->height, f->format);
      break;
    case DEPTHCAM_STREAM_DEPTH:
      if(n_depth++ == 0) printf("  depth %dx%d fmt=%d unit=%g\n", f->width, f->height, f->format, f->depth_unit_mm);
      break;
    case DEPTHCAM_STREAM_IR: n_ir++; break;
    case DEPTHCAM_STREAM_POINTCLOUD:
      if(n_pcl++ == 0) printf("  pcl %d points fmt=%d\n", f->point_count, f->format);
      break;
  }
  if(f->release) f->release(f->owner);
}

int main(int argc, char** argv)
{
  const char* solib = argv[1];
  const char* resdir = argv[2];
  const char* uri = argc > 3 ? argv[3] : nullptr;

  void* lib = dlopen(solib, RTLD_LAZY | RTLD_LOCAL);
  if(!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
  auto entry = (score_depthcam_backend_v1_fn)dlsym(lib, "score_depthcam_backend_v1");
  const auto* b = entry();
  printf("backend '%s' abi=%u\n", b->name, b->abi_version);

  if(!b->init(resdir)) { printf("init failed: %s\n", b->last_error()); return 1; }
  if(const char* e = b->last_error()) printf("init note: %s\n", e);

  printf("\n-- enumerate --\n");
  int n = 0;
  b->enumerate([](const depthcam_device_info* d, void* u) {
    (*(int*)u)++;
    printf("  %-40s  %-22s  %-10s  sn=%s\n", d->uri, d->name, d->transport, d->serial);
  }, &n);
  printf("  %d device(s)\n", n);

  if(!uri) { b->shutdown(); return 0; }

  printf("\n-- open %s --\n", uri);
  depthcam_open_config cfg{};
  cfg.streams = DEPTHCAM_STREAM_COLOR | DEPTHCAM_STREAM_DEPTH | DEPTHCAM_STREAM_POINTCLOUD;
  cfg.align = DEPTHCAM_ALIGN_COLOR_TO_DEPTH;  // the default in score

  // DEPTHCAM_TEST_STREAMS overrides the mask, and clearing the alignment with
  // it. Needed to reach an Azure Kinect under a sanitizer: anything involving
  // depth pulls in libdepthengine, a closed Microsoft blob that refuses to
  // initialise in an instrumented process, so colour-only is the only way to
  // exercise that backend at all there.
  if(const char* e = std::getenv("DEPTHCAM_TEST_STREAMS"))
  {
    cfg.streams = uint32_t(std::strtoul(e, nullptr, 0));
    cfg.align = DEPTHCAM_ALIGN_NONE;
  }
  const auto t0 = std::chrono::steady_clock::now();
  auto* dev = b->open(uri, &cfg);
  const auto dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if(!dev) { printf("  open failed after %.1fs: %s\n", dt, b->last_error() ? b->last_error() : "?"); b->shutdown(); return 1; }
  printf("  opened in %.1fs\n", dt);

  if(!b->start(dev, &on_frame, nullptr)) { printf("  start failed: %s\n", b->last_error()); b->close(dev); b->shutdown(); return 1; }
  std::this_thread::sleep_for(std::chrono::seconds(6));
  b->stop(dev);
  printf("  frames: color=%d depth=%d ir=%d pcl=%d\n", n_color.load(), n_depth.load(), n_ir.load(), n_pcl.load());
  b->close(dev);
  b->shutdown();
  return (n_color || n_depth) ? 0 : 2;
}
