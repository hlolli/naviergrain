#pragma once
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include "miniaudio.h"

class Instrument {
public:
  Instrument();
  ~Instrument();
  void start(bool cpu=false);
  void stop();
  void control(unsigned index,double value);
  void use_cpu();
  std::string snapshot();
private:
  static constexpr unsigned queue_frames=4096;
  static void callback(ma_device *,void *,const void *,ma_uint32);
  void render(bool cpu);
  ma_device device{};
  bool opened=false;
  std::thread worker;
  std::atomic<bool> running{false},draining{false},cpu_requested{false},failed{false};
  std::atomic<uint64_t> read{0},write{0},underruns{0},played{0};
  std::array<float,queue_frames*2> pcm{};
  bool waiting=true;
  unsigned fade=0;
  float last[2]={0,0};
  std::mutex mutex;
  std::array<double,24> controls{};
  bool reset=false;
  std::string observed="\"gpu\":false,\"view\":[],\"stats\":[],\"wave\":[]";
  std::string error;
};
