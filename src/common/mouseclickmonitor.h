#ifndef CAPTURER_MOUSE_CLICK_MONITOR_H
#define CAPTURER_MOUSE_CLICK_MONITOR_H

#include <functional>
#include <QPoint>

#ifdef _WIN32
#include <Windows.h>
#endif

// 全局低级鼠标钩子，录屏期间捕获左键点击，通过回调通知
class MouseClickMonitor
{
public:
    using Callback = std::function<void(QPoint)>;

    explicit MouseClickMonitor(Callback cb);
    ~MouseClickMonitor();

    void install();
    void uninstall();

private:
#ifdef _WIN32
    static LRESULT CALLBACK mouseProc(int nCode, WPARAM wParam, LPARAM lParam);

    HHOOK   hook_{ nullptr };
    static MouseClickMonitor *instance_;
#endif

    Callback callback_;
};

#endif //! CAPTURER_MOUSE_CLICK_MONITOR_H
