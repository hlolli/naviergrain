#import <AppKit/AppKit.h>
#include "platform.hpp"

@interface NaviergrainMenu : NSObject {
@public
  std::function<void()> about;
  std::function<void()> quit;
}
- (void)showAbout:(id)sender;
- (void)quitApp:(id)sender;
@end
@implementation NaviergrainMenu
- (void)showAbout:(id)sender { (void)sender; about(); }
- (void)quitApp:(id)sender { (void)sender; quit(); }
@end

void install_app_menu(std::function<void()> about, std::function<void()> quit) {
  static NaviergrainMenu *controller;
  controller=[NaviergrainMenu new];
  controller->about=std::move(about);controller->quit=std::move(quit);
  NSMenu *bar=[NSMenu new];
  NSMenuItem *app=[NSMenuItem new];[bar addItem:app];
  NSMenu *menu=[[NSMenu alloc] initWithTitle:@"naviergrain"];
  NSMenuItem *info=[[NSMenuItem alloc] initWithTitle:@"About naviergrain" action:@selector(showAbout:) keyEquivalent:@""];
  info.target=controller;[menu addItem:info];[menu addItem:[NSMenuItem separatorItem]];
  NSMenuItem *exit=[[NSMenuItem alloc] initWithTitle:@"Quit naviergrain" action:@selector(quitApp:) keyEquivalent:@"q"];
  exit.target=controller;[menu addItem:exit];app.submenu=menu;
  NSApp.mainMenu=bar;
}

bool open_profile(const std::string &url) {
  return [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]]];
}
