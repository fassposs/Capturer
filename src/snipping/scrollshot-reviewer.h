#ifndef CAPTURER_SCROLLSHOT_REVIEWER_H
#define CAPTURER_SCROLLSHOT_REVIEWER_H

#include "canvas/canvas.h"
#include "canvas/command.h"
#include "menu/editing-menu.h"
#include "resizer.h"

#include <QGraphicsView>
#include <QPixmap>
#include <QUndoStack>
#include <QWidget>
#include <memory>

// 长截图审阅窗口：支持拖拽平移、滚轮缩放，以及与普通截图相同的编辑工具
class ScrollShotReviewer final : public QWidget
{
    Q_OBJECT

public:
    explicit ScrollShotReviewer(const QPixmap& stitched, QWidget* parent = nullptr);

signals:
    void pinData(const std::shared_ptr<QMimeData>&);
    void saved(const QString& path);

public slots:
    void save();
    void copy();
    void pin();

protected:
    void resizeEvent(QResizeEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void showEvent(QShowEvent*) override;

private:
    void createItem(const QPointF& scene_pos);
    void finishItem();
    void updateCursor(ResizerLocation);
    void updateMenuPos();
    void syncDragMode();

    QGraphicsView*       view_{};
    canvas::Canvas*      canvas_{};
    EditingMenu*         menu_{};
    QUndoStack*          undo_stack_{};
    GraphicsItemWrapper* creating_item_{};
    int                  counter_{ 0 };
    QPixmap              stitched_{};
};

#endif //! CAPTURER_SCROLLSHOT_REVIEWER_H
