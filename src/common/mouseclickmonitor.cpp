#include "mouseclickmonitor.h"

#ifdef _WIN32

MouseClickMonitor *MouseClickMonitor::instance_ = nullptr;

MouseClickMonitor::MouseClickMonitor(Callback cb)
    : callback_(std::move(cb))
{
    instance_ = this;
}

MouseClickMonitor::~MouseClickMonitor()
{
    uninstall();
    if (instance_ == this) instance_ = nullptr;
}

void MouseClickMonitor::install()
{
    if (hook_) return;
    hook_ = SetWindowsHookEx(WH_MOUSE_LL, mouseProc, nullptr, 0);
}

void MouseClickMonitor::uninstall()
{
    if (!hook_) return;
    UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
}

LRESULT CALLBACK MouseClickMonitor::mouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && wParam == WM_LBUTTONDOWN && instance_ && instance_->callback_) {
        const auto *ms = reinterpret_cast<MSLLHOOKSTRUCT *>(lParam);
        instance_->callback_(QPoint(ms->pt.x, ms->pt.y));
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

#else

MouseClickMonitor::MouseClickMonitor(Callback cb) : callback_(std::move(cb)) {}
MouseClickMonitor::~MouseClickMonitor() { uninstall(); }
void MouseClickMonitor::install() {}
void MouseClickMonitor::uninstall() {}

#endif
