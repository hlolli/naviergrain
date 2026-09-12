#include "audio.hpp"
#include "fluidgrain_live.h"
extern "C" {
#include "fluidgrain_core.h"
}
#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <stdexcept>
using namespace std::chrono_literals;
static_assert(std::atomic<uint64_t>::is_always_lock_free,"Audio counters must be lock-free");
Instrument::Instrument() {
  fg_live_defaults(controls.data());
}
Instrument::~Instrument(){stop();}
void Instrument::control(unsigned index,double value) {
  if(!fg_live_control_valid(index,value))throw std::runtime_error("Control is out of range");
  std::lock_guard<std::mutex> lock(mutex);
  if(index==FG_CONTROL_RESET)reset=reset||value!=0;
  else controls[index]=value;
}
void Instrument::use_cpu(){cpu_requested.store(true);}
void Instrument::start(bool cpu) {
  if(running&&!failed)return;
  stop();
  read=0;write=0;underruns=0;played=0;waiting=true;fade=0;last[0]=last[1]=0;
  draining=false;failed=false;cpu_requested=cpu;
  {std::lock_guard<std::mutex> lock(mutex);error.clear();}
  ma_device_config config=ma_device_config_init(ma_device_type_playback);
  config.playback.format=ma_format_f32;config.playback.channels=2;
  config.sampleRate=48000;config.periodSizeInFrames=256;
  config.dataCallback=callback;config.pUserData=this;
  if(ma_device_init(nullptr,&config,&device)!=MA_SUCCESS)throw std::runtime_error("Cannot open the audio output");
  opened=true;running=true;
  try {
    worker=std::thread(&Instrument::render,this,cpu);
    auto deadline=std::chrono::steady_clock::now()+15s;
    while(write.load(std::memory_order_acquire)<1024&&!failed) {
      if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("Audio preparation timed out");
      std::this_thread::sleep_for(1ms);
    }
    if(failed)throw std::runtime_error("Synthesis preparation failed");
    if(ma_device_start(&device)!=MA_SUCCESS)throw std::runtime_error("Cannot start the audio output");
  } catch(...) {stop();throw;}
}
void Instrument::stop() {
  draining=true;running=false;
  if(worker.joinable())worker.join();
  if(opened) {
    auto deadline=std::chrono::steady_clock::now()+2s;
    while(ma_device_is_started(&device)&&read.load()<write.load()&&std::chrono::steady_clock::now()<deadline)
      std::this_thread::sleep_for(2ms);
    ma_device_uninit(&device);opened=false;
  }
}
void Instrument::callback(ma_device *device,void *output,const void *,ma_uint32 count) {
  auto &s=*static_cast<Instrument *>(device->pUserData);
  auto *out=static_cast<float *>(output);
  uint64_t r=s.read.load(std::memory_order_relaxed),w=s.write.load(std::memory_order_acquire);
  bool drain=s.draining.load(std::memory_order_relaxed);
  if(s.waiting&&(w-r>=1024||(drain&&w>r))){s.waiting=false;s.fade=32;}
  uint64_t consumed=0;
  for(unsigned i=0;i<count;++i) {
    if(!s.waiting&&r<w) {
      float gain=s.fade?float(33-s.fade--)/32:1;
      if(drain&&w-r<64)gain*=float(w-r)/64;
      for(unsigned c=0;c<2;++c)out[2*i+c]=s.last[c]=s.pcm[2*(r%queue_frames)+c]*gain;
      ++r;++consumed;
    } else {
      if(!s.waiting){s.waiting=true;s.fade=32;if(!drain)++s.underruns;}
      float gain=s.fade?float(--s.fade)/32:0;
      for(unsigned c=0;c<2;++c)out[2*i+c]=s.last[c]*gain;
    }
  }
  s.played.fetch_add(consumed,std::memory_order_relaxed);
  s.read.store(r,std::memory_order_release);
}
void Instrument::render(bool cpu) {
  FGLive *live=fg_live_create(device.sampleRate,cpu?-1:0);
  if(!live){failed=true;return;}
  unsigned batch=0;
  while(running) {
    uint64_t w=write.load(std::memory_order_relaxed),r=read.load(std::memory_order_acquire);
    // Keep control lookahead at most 2048 source frames.
    if(w-r>=2048){std::this_thread::sleep_for(250us);continue;}
    {
      std::lock_guard<std::mutex> lock(mutex);
      for(unsigned i=0;i<controls.size();++i)fg_live_control(live,i,controls[i]);
      if(reset){fg_live_control(live,FG_CONTROL_RESET,1);reset=false;}
    }
    if(cpu_requested)fg_live_cpu(live);
    if(!fg_live_render(live,512)) {
      std::lock_guard<std::mutex> lock(mutex);error="Synthesis stopped; restart playback";failed=true;break;
    }
    const double *audio=fg_live_audio(live);
    for(unsigned i=0;i<512;++i)for(unsigned c=0;c<2;++c)
      pcm[2*((w+i)%queue_frames)+c]=float(std::clamp(audio[2*i+c],-1.0,1.0));
    write.store(w+512,std::memory_order_release);
    if(++batch%2==0) {
      std::ostringstream json;json.imbue(std::locale::classic());json.precision(10);
      json<<"\"backend\":\""<<FG_DESKTOP_GPU_NAME<<"\",\"gpu\":"<<(fg_live_gpu(live)?"true":"false")<<",\"particles\":[";
      const double *particles=fg_live_particles(live);
      if(particles)for(unsigned i=0;i<FG_LIVE_PARTICLE_HEADER+(unsigned)particles[2]*FG_LIVE_PARTICLE_STRIDE;++i)json<<(i?",":"")<<particles[i];
      json<<"],\"stats\":[";const double *stats=fg_live_stats(live);
      for(unsigned i=0;i<FG_STAT_COUNT;++i)json<<(i?",":"")<<stats[i];
      json<<"],\"wave\":[";
      for(unsigned i=0;i<128;++i)json<<(i?",":"")<<(audio[i*8]+audio[i*8+1])*.5;
      json<<"],\"spectrum\":[";const double *spectrum=fg_live_spectrum(live);
      for(unsigned i=0;i<FG_LIVE_SPECTRUM_BINS;++i)json<<(i?",":"")<<spectrum[i];
      json<<"]";
      std::lock_guard<std::mutex> lock(mutex);observed=json.str();
    }
  }
  fg_live_destroy(live);
}
std::string Instrument::snapshot() {
  std::lock_guard<std::mutex> lock(mutex);
  return "{\"running\":"+std::string(running&&!failed?"true":"false")+
    ",\"underruns\":"+std::to_string(underruns.load())+",\"played\":"+std::to_string(played.load())+
    ",\"error\":\""+error+"\","+observed+"}";
}
