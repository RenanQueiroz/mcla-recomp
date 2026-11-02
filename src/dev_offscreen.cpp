// Opt-in development rendering on a hidden, nonactivating window. This does
// not isolate the process from the interactive station. No physical input is
// injected; tests use dev_pad and the game's render-target screenshot command.
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/window.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(mcla_dev_offscreen, false, "MCLA", "Development: hidden nonactivating window, muted, physical input drivers omitted; command line only")
    .debug_only();

namespace {
bool Requested() {
#ifdef _WIN32
  return REXCVAR_GET(mcla_dev_offscreen) &&
      rex::cvar::GetFlagSource("mcla_dev_offscreen")==rex::cvar::Source::kCommandLine;
#else
  return false;
#endif
}
#ifdef _WIN32
HHOOK g_hook=nullptr;
LRESULT CALLBACK OffscreenHook(int code,WPARAM wp,LPARAM lp) {
  if(code==HCBT_ACTIVATE || code==HCBT_SETFOCUS) return 1;
  if(code==HCBT_CREATEWND) {
    auto* create=reinterpret_cast<CBT_CREATEWNDW*>(lp)->lpcs;
    create->style &= ~WS_VISIBLE;
    create->dwExStyle |= WS_EX_NOACTIVATE;
    create->x=40000;create->y=40000;
  }
  return CallNextHookEx(g_hook,code,wp,lp);
}
#endif
}

bool MclaDevOffscreenRequested() { return Requested(); }

void MclaDevOffscreenSetup() {
  if(!Requested()) return;
  rex::cvar::SetFlagFromCommandLine("fullscreen","false");
  rex::cvar::SetFlagFromCommandLine("audio_mute","true");
#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
  g_hook=SetWindowsHookExW(WH_CBT,OffscreenHook,nullptr,GetCurrentThreadId());
  if(!g_hook) { REXLOG_ERROR("offscreen: cannot install no-activation guard"); ExitProcess(4); }
#endif
}

void MclaDevOffscreenReady([[maybe_unused]] rex::ui::Window* window) {
  if(!Requested()) return;
#ifdef _WIN32
  HWND hwnd=window?static_cast<HWND>(window->GetNativeWindowHandle()):nullptr;
  if(!hwnd) { REXLOG_ERROR("offscreen: no native window"); ExitProcess(4); }
  SetWindowLongPtrW(hwnd,GWL_EXSTYLE,GetWindowLongPtrW(hwnd,GWL_EXSTYLE)|WS_EX_NOACTIVATE);
  SetWindowPos(hwnd,nullptr,40000,40000,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
  ShowWindow(hwnd,SW_HIDE);
  if(IsWindowVisible(hwnd)||GetForegroundWindow()==hwnd) {
    REXLOG_ERROR("offscreen: window visibility/focus guard failed"); ExitProcess(4);
  }
  REXLOG_INFO("offscreen: hidden window verified; activation blocked; physical SDL input drivers omitted (NOP plus ScriptPad only); audio muted (interactive station)");
#endif
}
