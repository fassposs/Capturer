#ifndef CAPTURER_SCREEN_SHOTER_H
#define CAPTURER_SCREEN_SHOTER_H

#include "canvas/canvas.h"
#include "canvas/command.h"
#include "hunter.h"
#include "magnifier.h"
#include "menu/editing-menu.h"
#include "resizer.h"
#include "selector.h"

#include <QGraphicsView>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

// 长截图浮层：捕获用户选定区域的多帧并自动拼接
class ScrollShotOverlay final : public QWidget
{
    Q_OBJECT

public:
    explicit ScrollShotOverlay(const QRect &capture_rect, QWidget *parent = nullptr);

signals:
    void finished(const QPixmap &result);
    void cancelled();

public slots:
    void start();

protected:
    bool eventFilter(QObject *, QEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void paintEvent(QPaintEvent *) override;

private slots:
    void onTick();
    void onFinish();
    void onCancel();

private:
    QPixmap captureFrame() const;
    int     findOverlapOffset(const QImage &last, const QImage &curr) const;
    void    appendFrame(const QImage &curr, int offset);
    void    updateLabel();
    void    positionSelf();

    static constexpr int TICK_MS     = 80;  // 缩短轮询间隔，减少快速滚动时的内容丢失
    static constexpr int STRIP_H     = 80;
    static constexpr int SAMPLE_STEP = 4;
    static constexpr int MAX_HEIGHT  = 16000;
    static constexpr int MIN_SCROLL  = 4; // 低于此像素视为未滚动，忽略

    QRect        capture_rect_;
    QPixmap      stitched_;
    QImage       stitched_image_; // 与 stitched_ 同步的 QImage，供拼接匹配用
    int          frame_count_{ 0 };
    int          logical_w_{ 0 }; // 捕获区域逻辑像素宽，用于去除DPI缩放影响

    QTimer      *timer_{};
    QLabel      *label_{};
    QPushButton *finish_btn_{};
    QPushButton *cancel_btn_{};
    QWidget     *border_hint_{}; // 捕获区域边框提示窗口
};

class ScreenShoter final : public QGraphicsView
{
    Q_OBJECT

public:
    explicit ScreenShoter(QWidget *parent = nullptr);

signals:
    void pinData(const std::shared_ptr<QMimeData>&);

    void saved(const QString& path);

    void scrollShotReady(const QPixmap& stitched);

public slots:
    void start();
    void exit();

    void repeat();

    void                       save();
    void                       copy();
    void                       pin();
    std::pair<QPixmap, QPoint> snip();

    void setStyle(const SelectorStyle& style);

    void moveMenu();

    void updateCursor(ResizerLocation);

    bool screenshot(const probe::geometry_t&);
    void setBackground(const QPixmap&, const probe::geometry_t&);

    void createItem(const QPointF& pos);
    void finishItem();

    void startScrollShot();
    void endScrollShot();

#ifdef Q_OS_LINUX
    void DbusScreenshotArrived(uint, const QVariantMap&);
#endif

protected:
    bool eventFilter(QObject *, QEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

    void wheelEvent(QWheelEvent *) override;

    void keyPressEvent(QKeyEvent *) override;
    void keyReleaseEvent(QKeyEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;

private:
    void                 registerShortcuts();
    QBrush               mosaicBrush();
    [[nodiscard]] size_t history_index();

    Selector       *selector_{}; // Layer 1
    canvas::Canvas *scene_{};

    GraphicsItemWrapper *creating_item_{};
    int                  counter_{ 0 };

    EditingMenu *menu_{};      // editing menu
    Magnifier   *magnifier_{}; // magnifier

    // history
    std::vector<hunter::prey_t> history_{};
    size_t                      history_idx_{ 0 };

    QUndoStack *undo_stack_{};

    ScrollShotOverlay *scroll_overlay_{};
};

#endif //! CAPTURER_SCREEN_SHOTER_H
