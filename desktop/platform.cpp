#include "platform.hpp"
#include <cstdint>
#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

void install_app_menu(std::function<void()>, std::function<void()>) {}
bool open_profile(const std::string &url) {
#if defined(_WIN32)
  return reinterpret_cast<intptr_t>(ShellExecuteA(nullptr,"open",url.c_str(),nullptr,nullptr,SW_SHOWNORMAL))>32;
#else
  pid_t child;
  const char *args[]={"xdg-open",url.c_str(),nullptr};
  if(posix_spawnp(&child,args[0],nullptr,nullptr,const_cast<char **>(args),environ))return false;
  int status=0;return waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0;
#endif
}
