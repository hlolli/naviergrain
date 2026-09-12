#include "audio.hpp"
#include "webview/webview.h"
#include "fluidgrain_ui.h"
#include "platform.hpp"
#include <chrono>
#include <cmath>
#include <iostream>
#include <sstream>
#include <vector>

// Bridge accepts only a short array of finite numbers; it exposes no paths,
// shell commands, arbitrary URLs or general-purpose native evaluation.
static std::vector<double> arguments(const std::string &text) {
  std::istringstream in(text);in.imbue(std::locale::classic());char ch;
  std::vector<double> out;
  if(!(in>>ch)||ch!='[')throw std::runtime_error("Invalid command");
  for(;;) {
    double n;if(!(in>>n)||!std::isfinite(n)||out.size()>=3)throw std::runtime_error("Invalid command");
    out.push_back(n);if(!(in>>ch))throw std::runtime_error("Invalid command");
    if(ch==']')break;if(ch!=',')throw std::runtime_error("Invalid command");
  }
  in>>std::ws;if(!in.eof())throw std::runtime_error("Invalid command");
  return out;
}
int main(int argc,char **argv) {
  try {
    Instrument instrument;
    if(argc==2&&std::string(argv[1])=="--smoke") {
      instrument.start();std::this_thread::sleep_for(std::chrono::seconds(1));
      std::cout<<instrument.snapshot()<<"\n";
      instrument.control(10,-0.7);instrument.use_cpu();
      std::this_thread::sleep_for(std::chrono::seconds(1));instrument.stop();
      std::cout<<instrument.snapshot()<<"\n";return 0;
    }
    webview::webview window(false,nullptr);
    window.set_title("naviergrain");window.set_size(1180,800,WEBVIEW_HINT_NONE);
    const auto quit=[&]{instrument.stop();window.terminate();};
    install_app_menu([&]{window.eval("document.getElementById('about').showModal()");},quit);
    window.bind("nativeCommand",[&](const std::string &request)->std::string {
      try {
        auto a=arguments(request);
        if(a[0]==0&&a.size()==2&&(a[1]==0||a[1]==1))instrument.start(a[1]!=0);
        else if(a[0]==1&&a.size()==1)instrument.stop();
        else if(a[0]==2&&a.size()==3&&a[1]>=0&&a[1]<24&&std::floor(a[1])==a[1])instrument.control(unsigned(a[1]),a[2]);
        else if(a[0]==3&&a.size()==1)return instrument.snapshot();
        else if(a[0]==4&&a.size()==1)instrument.use_cpu();
        else if(a[0]==5&&a.size()==1)window.dispatch(quit);
        else if(a[0]==6&&a.size()==2&&a[1]>=0&&a[1]<fg_profile_count&&std::floor(a[1])==a[1]) {
          if(!open_profile(fg_profile_urls[static_cast<unsigned>(a[1])]))throw std::runtime_error("Could not open profile");
        }
        else throw std::runtime_error("Invalid command");
        return "{\"ok\":true}";
      }catch(const std::exception &e){std::cerr<<e.what()<<"\n";return "{\"error\":\"The audio command failed. Check the output device and try again.\"}";}
    });
    window.set_html(fg_ui_html);window.run();instrument.stop();
  }catch(const std::exception &e){std::cerr<<e.what()<<"\n";return 1;}
}
