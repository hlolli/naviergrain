#pragma once
#include <functional>
#include <string>

void install_app_menu(std::function<void()> about, std::function<void()> quit);
bool open_profile(const std::string &url);
