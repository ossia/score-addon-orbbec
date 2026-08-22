// Mimics score's real sequence: the device is opened when the ossia device is
// created (reconnect), then started only when the transport rolls, which can be
// many seconds later.
#include <depthcam_abi.h>
#include <dlfcn.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

static std::atomic<int> n[4]{};
static int idx_of(uint32_t s)
{
  switch(s) { case DEPTHCAM_STREAM_COLOR: return 0; case DEPTHCAM_STREAM_IR: return 1;
              case DEPTHCAM_STREAM_DEPTH: return 2; default: return 3; }
}
static void on_frame(const depthcam_frame* f, void*)
{
  n[idx_of(f->stream)]++;
  if(f->release) f->release(f->owner);
}

int main(int argc, char** argv)
{
  const char* solib = argv[1];
  const char* resdir = argv[2];
  const int gap = argc > 3 ? atoi(argv[3]) : 10;

  void* lib = dlopen(solib, RTLD_LAZY | RTLD_LOCAL);
  if(!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
  const auto* b = ((score_depthcam_backend_v1_fn)dlsym(lib, "score_depthcam_backend_v1"))();
  if(!b->init(resdir)) { printf("init failed: %s\n", b->last_error()); return 1; }

  std::string uri;
  b->enumerate([](const depthcam_device_info* d, void* u) {
    if(((std::string*)u)->empty()) *(std::string*)u = d->uri; }, &uri);
  if(uri.empty()) { printf("no device\n"); return 1; }
  printf("uri = %s\n", uri.c_str());

  depthcam_open_config cfg{};
  cfg.streams = DEPTHCAM_STREAM_COLOR | DEPTHCAM_STREAM_IR | DEPTHCAM_STREAM_DEPTH
                | DEPTHCAM_STREAM_POINTCLOUD;
  auto t0 = std::chrono::steady_clock::now();
  auto* dev = b->open(uri.c_str(), &cfg);
  auto el = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count(); };
  if(!dev) { printf("open failed: %s\n", b->last_error()); return 1; }
  printf("[%5.1fs] opened\n", el());

  printf("[%5.1fs] idling %ds before start (as score does between reconnect and play)\n", el(), gap);
  std::this_thread::sleep_for(std::chrono::seconds(gap));

  if(!b->start(dev, &on_frame, nullptr)) { printf("start failed: %s\n", b->last_error()); return 1; }
  printf("[%5.1fs] started\n", el());
  for(int i = 0; i < 8; i++)
  {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    printf("[%5.1fs] color=%d ir=%d depth=%d pcl=%d%s\n", el(),
           n[0].load(), n[1].load(), n[2].load(), n[3].load(),
           b->last_error() ? (std::string{"  err: "} + b->last_error()).c_str() : "");
  }
  b->stop(dev); b->close(dev); b->shutdown();
  return (n[0] || n[2]) ? 0 : 2;
}
