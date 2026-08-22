#include <depthcam_abi.h>
#include <dlfcn.h>
#include <cstdio>
#include <string>
#include <vector>
static const char* kindname(int k){
  switch(k){case DEPTHCAM_CONTROL_BOOL:return "bool";case DEPTHCAM_CONTROL_INT:return "int";
            case DEPTHCAM_CONTROL_FLOAT:return "float";case DEPTHCAM_CONTROL_ENUM:return "enum";
            case DEPTHCAM_CONTROL_ACTION:return "action";default:return "?";}}
static std::vector<depthcam_control> ctls;
static void on_ctl(const depthcam_control* c, void*){ ctls.push_back(*c); }
int main(int argc,char**argv){
  void* lib=dlopen(argv[1],RTLD_LAZY|RTLD_LOCAL); if(!lib){printf("%s\n",dlerror());return 1;}
  auto e=(score_depthcam_backend_v1_fn)dlsym(lib,"score_depthcam_backend_v1");
  const auto* b=e();
  printf("backend '%s' abi=%u controls=%s\n", b->name, b->abi_version,
         b->list_controls ? "yes" : "NO");
  if(!b->init(argv[2])){printf("init failed\n");return 1;}
  std::string uri; b->enumerate([](const depthcam_device_info* d,void* u){
    if(((std::string*)u)->empty()) *(std::string*)u=d->uri; },&uri);
  if(uri.empty()){printf("no device\n");return 1;}
  depthcam_open_config cfg{}; cfg.streams=DEPTHCAM_STREAM_DEPTH;
  auto* dev=b->open(uri.c_str(),&cfg);
  if(!dev){printf("open failed: %s\n", b->last_error()?b->last_error():"?");return 1;}
  if(b->list_controls) b->list_controls(dev,&on_ctl,nullptr);
  printf("%zu controls\n\n", ctls.size());
  std::string last_group;
  for(auto& c : ctls){
    std::string id=c.id; auto slash=id.find('/');
    std::string g = slash==std::string::npos?"":id.substr(0,slash);
    if(g!=last_group){ printf("  [%s]\n", g.c_str()); last_group=g; }
    double v=0; bool got = b->get_control && b->get_control(dev,c.id,&v);
    printf("    %-34s %-6s %c%c  range %g..%g step %g def %g",
           id.c_str()+ (slash==std::string::npos?0:slash+1), kindname(c.kind),
           (c.access&DEPTHCAM_ACCESS_READ)?'r':'-',
           (c.access&DEPTHCAM_ACCESS_WRITE)?'w':'-',
           c.min,c.max,c.step,c.def);
    if(got) printf("  = %g", v);
    if(c.enum_labels && c.enum_count > 0){
      printf("\n        values:");
      for(int i=0;i<c.enum_count;i++){
        const double val = c.min + i*(c.step>0?c.step:1);
        printf(" %g=%s", val, c.enum_labels[i]?c.enum_labels[i]:"?");
      }
    }
    printf("\n");
  }
  b->close(dev); b->shutdown(); return 0;
}
