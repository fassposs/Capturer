#include "scrollshot-reviewer.h"

#include "canvas/graphicsitems.h"
#include "clipboard.h"
#include "config.h"
#include "logging.h"

#include <QApplication>
#include <QDateTime>
#include <QFileDialog>
#include <QGraphicsPixmapItem>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QShortcut>
#include <QTimer>
#include <QWheelEvent>

// ─── ReviewView ───────────────────────────────────────────────────────────────
// QGraphicsView 内部子类，处理缩放和鼠标事件转发

class ReviewView final : public QGraphicsView
{
public:
    std::function<void(const QPointF&)> on_press;
    std::function<void(const QPointF&)> on_move;
    std::function<void()>               on_release;

    QPixmap bg_; // 背景长截图，由 drawBackground 单张绘制，避免 QBrush 平铺

    explicit ReviewView(QWidget* parent = nullptr) : QGraphicsView(parent)
    {
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        setResizeAnchor(QGraphicsView::AnchorUnderMouse);
        setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform |
                       QPainter::TextAntialiasing);
        setFrameStyle(QGraphicsView::NoFrame);
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    }

protected:
    void drawBackground(QPainter* painter, const QRectF&) override
    {
        if (!bg_.isNull())
            painter->drawPixmap(QPointF(0, 0), bg_);
    }
    void wheelEvent(QWheelEvent* event) override
    {
        // Ctrl: 调整画笔宽度（由上层 EditingMenu 处理）；否则缩放视图
        if (!(event->modifiers() & Qt::CTRL)) {
            const qreal factor = event->angleDelta().y() > 0 ? 1.12 : 1.0 / 1.12;
            scale(factor, factor);
            event->accept();
            return;
        }
        QGraphicsView::wheelEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (dragMode() == QGraphicsView::ScrollHandDrag || event->button() != Qt::LeftButton) {
            QGraphicsView::mousePressEvent(event);
            return;
        }
        if (on_press) on_press(mapToScene(event->pos()));
        QGraphicsView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (dragMode() != QGraphicsView::ScrollHandDrag && (event->buttons() & Qt::LeftButton)) {
            if (on_move) on_move(mapToScene(event->pos()));
        }
        QGraphicsView::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (dragMode() != QGraphicsView::ScrollHandDrag && event->button() == Qt::LeftButton) {
            if (on_release) on_release();
        }
        QGraphicsView::mouseReleaseEvent(event);
    }
};

// ─── ScrollShotReviewer ───────────────────────────────────────────────────────

ScrollShotReviewer::ScrollShotReviewer(const QPixmap& stitched, QWidget* parent)
    : QWidget(parent), stitched_(stitched)
{
    setWindowTitle(tr("长截图"));
    setWindowFlags(Qt::Window);
    setAttribute(Qt::WA_DeleteOnClose);

    undo_stack_ = new QUndoStack(this);
    canvas_     = new canvas::Canvas(this);

    // 将拼接图作为场景背景 pixmap item（不参与编辑）
    canvas_->setBackgroundBrush(QBrush(stitched_));
    canvas_->setSceneRect(QRectF(QPointF(0, 0), stitched_.size()));

    auto* review_view = new ReviewView(this);
    review_view->bg_  = stitched_; // 单张绘制，不平铺
    view_ = review_view;
    view_->setScene(canvas_);
    view_->setDragMode(QGraphicsView::ScrollHandDrag); // 默认：拖拽平移

    // 编辑工具回调
    review_view->on_press   = [this](const QPointF& pos) { createItem(pos); };
    review_view->on_move    = [this](const QPointF& pos) {
        if (creating_item_) creating_item_->push(pos);
    };
    review_view->on_release = [this]() { finishItem(); };

    // 长截图审阅窗口不需要长截图按钮（ADVANCED_GROUP）
    menu_ = new EditingMenu(this, EditingMenu::GRAPH_GROUP | EditingMenu::REDO_UNDO_GROUP |
                                      EditingMenu::SAVE_GROUP | EditingMenu::EXIT_GROUP);
    menu_->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);

    // 按画面比例初始化窗口大小：最大化到当前屏幕的 80%
    const auto* screen  = QApplication::primaryScreen();
    const auto  sg      = screen->geometry();
    const int   init_w  = std::min(stitched_.width() + 20,  static_cast<int>(sg.width()  * 0.8));
    const int   init_h  = std::min(stitched_.height() + 20, static_cast<int>(sg.height() * 0.8));
    resize(init_w, init_h);
    move(sg.center() - QPoint{ init_w / 2, init_h / 2 });

    // 注意：此时不要调用 fitInView，因为视图还未完成布局
    // 延迟到 showEvent 中处理，确保视图大小正确

    const auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view_);

    // ── 信号连接 ────────────────────────────────────────────────────────────

    connect(menu_, &EditingMenu::graphChanged, [this](auto graph) {
        syncDragMode();
        updateCursor(ResizerLocation::DEFAULT);
        (void)graph;
    });

    connect(menu_, &EditingMenu::penChanged, [this](auto graph, const auto& pen) {
        if (auto item = canvas_->focusItem(); item && item->graph() == graph && item->pen() != pen)
            undo_stack_->push(new PenChangedCommand(item, item->pen(), pen));
    });

    connect(menu_, &EditingMenu::fontChanged, [this](auto graph, const auto& font) {
        if (auto item = canvas_->focusOrFirstSelectedItem();
            item && item->graph() == graph && item->font() != font)
            undo_stack_->push(new FontChangedCommand(item, item->font(), font));
    });

    connect(menu_, &EditingMenu::fillChanged, [this](auto graph, auto filled) {
        if (auto item = canvas_->focusItem(); item && item->graph() == graph && item->filled() != filled)
            undo_stack_->push(new FillChangedCommand(item, filled));
    });

    connect(menu_, &EditingMenu::imageArrived, [this](const auto& pixmap) {
        auto* item = new GraphicsPixmapItem(pixmap, canvas_->sceneRect().center());
        undo_stack_->push(new CreatedCommand(canvas_, dynamic_cast<QGraphicsItem*>(item)));
        item->onhovered([this](auto rl) { updateCursor(rl); });
        item->onmoved([=, this](auto opos) {
            undo_stack_->push(new MoveCommand(dynamic_cast<QGraphicsItem*>(item), opos));
        });
        item->onrotated([=, this](auto angle) { undo_stack_->push(new RotateCommand(item, angle)); });
        item->onresized([=, this](const auto& g, auto l) {
            undo_stack_->push(new ResizeCommand(item, g, l));
        });
    });

    connect(menu_, &EditingMenu::save,   this, &ScrollShotReviewer::save);
    connect(menu_, &EditingMenu::copy,   this, &ScrollShotReviewer::copy);
    connect(menu_, &EditingMenu::pin,    this, &ScrollShotReviewer::pin);
    connect(menu_, &EditingMenu::exit,   this, &ScrollShotReviewer::close);
    connect(menu_, &EditingMenu::undo,   undo_stack_, &QUndoStack::undo);
    connect(menu_, &EditingMenu::redo,   undo_stack_, &QUndoStack::redo);
    connect(menu_, &EditingMenu::scroll, []() {}); // 审阅窗口内不支持嵌套长截图

    connect(undo_stack_, &QUndoStack::canRedoChanged, menu_, &EditingMenu::canRedo);
    connect(undo_stack_, &QUndoStack::canUndoChanged, menu_, &EditingMenu::canUndo);

    connect(canvas_, &canvas::Canvas::focusItemChanged, this, [this](auto item, auto, auto reason) {
        if (item && reason == Qt::MouseFocusReason) {
            canvas_->clearSelection();
            const auto wrapper = dynamic_cast<GraphicsItemWrapper*>(item);
            menu_->paintGraph((wrapper->graph() == canvas::pixmap) ? canvas::none : wrapper->graph());
            menu_->fill(wrapper->filled());
            menu_->setPen(wrapper->pen());
            menu_->setFont(wrapper->font());
        }
    });

    // ── 快捷键 ──────────────────────────────────────────────────────────────

    connect(new QShortcut(Qt::CTRL | Qt::Key_S, this), &QShortcut::activated, this,
            &ScrollShotReviewer::save);
    connect(new QShortcut(Qt::CTRL | Qt::Key_C, this), &QShortcut::activated, this,
            &ScrollShotReviewer::copy);
    connect(new QShortcut(Qt::Key_P, this), &QShortcut::activated, this, &ScrollShotReviewer::pin);
    connect(new QShortcut(Qt::Key_Escape, this), &QShortcut::activated, this, &ScrollShotReviewer::close);
    connect(new QShortcut(Qt::CTRL | Qt::Key_Z, this), &QShortcut::activated, undo_stack_,
            &QUndoStack::undo);
    connect(new QShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_Z, this), &QShortcut::activated, undo_stack_,
            &QUndoStack::redo);
    connect(new QShortcut(QKeySequence::Delete, this), &QShortcut::activated, [this] {
        if (!canvas_->selectedItems().isEmpty() || canvas_->focusItem())
            undo_stack_->push(new DeleteCommand(canvas_));
    });

    // Space 切换平移/编辑模式（类似 ScreenShoter 的 Space 切换）
    auto* space_shortcut = new QShortcut(Qt::Key_Space, this);
    space_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(space_shortcut, &QShortcut::activated, [this] {
        if (menu_->graph() != canvas::none) {
            view_->setDragMode(QGraphicsView::ScrollHandDrag);
            view_->setCursor(Qt::OpenHandCursor);
        }
    });
}

// ── 私有方法 ──────────────────────────────────────────────────────────────────

void ScrollShotReviewer::syncDragMode()
{
    if (menu_->graph() == canvas::none) {
        view_->setDragMode(QGraphicsView::ScrollHandDrag);
    }
    else {
        view_->setDragMode(QGraphicsView::NoDrag);
    }
}

void ScrollShotReviewer::updateMenuPos()
{
    if (!menu_->isVisible()) return;

    const QRect va        = view_->geometry();
    const int   x         = va.left() + (va.width() - menu_->width()) / 2;
    const int   margin    = 6;
    const auto* screen    = QApplication::primaryScreen();
    const QRect sg        = screen->availableGeometry();
    const int   va_bottom = mapToGlobal(QPoint(0, va.bottom())).y();
    const int   va_top    = mapToGlobal(QPoint(0, va.top())).y();

    int y = 0;

    // 优先放在预览区下边线下面（屏幕下方有足够空间）
    if (va_bottom + menu_->height() + margin < sg.bottom()) {
        y = va.bottom() + margin;
        menu_->setSubMenuShowAbove(false);
    }
    // 其次放在预览区上边线上面（屏幕上方有足够空间）
    else if (va_top - menu_->height() - margin > sg.top()) {
        y = va.top() - menu_->height() - margin;
        menu_->setSubMenuShowAbove(true);
    }
    // 否则放在下边线上面（在预览区内）
    else {
        y = va.bottom() - menu_->height() - margin;
        menu_->setSubMenuShowAbove(true);
    }

    menu_->move(mapToGlobal(QPoint{ x, y }));
}

void ScrollShotReviewer::updateCursor(const ResizerLocation location)
{
    if (menu_->graph() & canvas::eraser)
        view_->setCursor(Qt::CrossCursor);
    else
        view_->setCursor(getCursorByLocation(location, Qt::ArrowCursor));
}

void ScrollShotReviewer::createItem(const QPointF& scene_pos)
{
    if (menu_->graph() == canvas::none) return;
    if (creating_item_) finishItem();

    switch (menu_->graph()) {
    case canvas::rectangle: creating_item_ = new GraphicsRectItem(scene_pos, scene_pos); break;
    case canvas::ellipse:   creating_item_ = new GraphicsEllipseItem(scene_pos, scene_pos); break;
    case canvas::arrow:     creating_item_ = new GraphicsArrowItem(scene_pos, scene_pos); break;
    case canvas::line:      creating_item_ = new GraphicsLineItem(scene_pos, scene_pos); break;
    case canvas::curve:
        creating_item_ = new GraphicsCurveItem(scene_pos, canvas_->sceneRect().size());
        break;
    case canvas::counter: creating_item_ = new GraphicsCounterItem(scene_pos, ++counter_); break;
    case canvas::text:    creating_item_ = new GraphicsTextItem(scene_pos); break;
    case canvas::eraser:
        creating_item_ = new GraphicsEraserItem(scene_pos, canvas_->sceneRect().size(),
                                                 canvas_->backgroundBrush());
        break;
    case canvas::mosaic: {
        // 马赛克：基于当前背景生成
        auto pixmap = stitched_;
        pixmap      = pixmap.scaled(pixmap.size() / 9, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                          .scaled(stitched_.size());
        creating_item_ = new GraphicsMosaicItem(scene_pos, canvas_->sceneRect().size(), QBrush(pixmap));
        break;
    }
    default: return;
    }

    creating_item_->setPen(menu_->pen());
    creating_item_->setFont(menu_->font());
    creating_item_->fill(menu_->filled());

    creating_item_->onhovered([this](auto location) { updateCursor(location); });
    creating_item_->onmoved([item = creating_item_, this](auto opos) {
        undo_stack_->push(new MoveCommand(dynamic_cast<QGraphicsItem*>(item), opos));
    });
    creating_item_->onresized([item = creating_item_, this](const auto& g, auto l) {
        undo_stack_->push(new ResizeCommand(item, g, l));
        if (item->graph() & canvas::text) menu_->setFont(item->font());
    });
    creating_item_->onrotated(
        [item = creating_item_, this](auto angle) { undo_stack_->push(new RotateCommand(item, angle)); });

    canvas_->add(creating_item_);
    dynamic_cast<QGraphicsItem*>(creating_item_)->setFocus();
}

void ScrollShotReviewer::finishItem()
{
    if (!creating_item_) return;

    if (creating_item_->invalid()) {
        canvas_->remove(creating_item_);
        delete creating_item_;
    }
    else {
        creating_item_->end();
        undo_stack_->push(new CreatedCommand(canvas_, dynamic_cast<QGraphicsItem*>(creating_item_)));
    }
    creating_item_ = nullptr;
}

// ── 渲染输出 ─────────────────────────────────────────────────────────────────

// 将背景图与所有标注合并渲染为单张 QPixmap
static QPixmap renderResult(canvas::Canvas* canvas, const QPixmap& background)
{
    QPixmap result(background.size());
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.drawPixmap(0, 0, background);
    canvas->render(&painter, result.rect(), canvas->sceneRect());
    return result;
}

// ── 槽 ───────────────────────────────────────────────────────────────────────

void ScrollShotReviewer::save()
{
    const QString default_name =
        "Capturer_" + QDateTime::currentDateTime().toString("yyyy-MM-dd_hhmmss_zzz") + ".png";

#ifdef _WIN32
    const QString filename = QFileDialog::getSaveFileName(
        this, tr("Save Image"), config::snip::path + QDir::separator() + default_name,
        "PNG(*.png);;JPEG(*.jpg *.jpeg);;BMP(*.bmp)");
#else
    const QString filename = config::snip::path + QDir::separator() + default_name;
#endif

    if (filename.isEmpty()) return;

    config::snip::path = QFileInfo(filename).absolutePath();

    const QPixmap result = renderResult(canvas_, stitched_);
    if (!result.save(filename)) loge("failed to save the long screenshot.");

    emit saved(filename);
    close();
}

void ScrollShotReviewer::copy()
{
    clipboard::push(renderResult(canvas_, stitched_));
    close();
}

void ScrollShotReviewer::pin()
{
    emit pinData(clipboard::push(renderResult(canvas_, stitched_), QPoint{ 0, 0 }));
    close();
}

// ── 事件 ─────────────────────────────────────────────────────────────────────

void ScrollShotReviewer::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateMenuPos();
    if (menu_->isVisible()) menu_->show(); // 触发重新定位
}

void ScrollShotReviewer::keyPressEvent(QKeyEvent* event)
{
    // Space 松开：恢复编辑模式
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat() && menu_->graph() != canvas::none) {
        syncDragMode();
        updateCursor(ResizerLocation::DEFAULT);
    }
    QWidget::keyPressEvent(event);
}

void ScrollShotReviewer::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    menu_->show();

    // 延迟调整视图缩放和菜单位置，确保所有控件已完成布局
    // 使用单发定时器，在事件循环下一次迭代时执行
    QTimer::singleShot(0, this, [this]() {
        if (view_ && canvas_) {
            view_->fitInView(canvas_->sceneRect(), Qt::KeepAspectRatio);
        }
        updateMenuPos();
    });
}
