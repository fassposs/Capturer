#include "screenshoter.h"

#include "circlecursor.h"
#include "clipboard.h"
#include "config.h"
#include "logging.h"

#include <algorithm>
#include <probe/system.h>
#include <QApplication>
#include <QDateTime>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QShortcut>
#include <QVBoxLayout>

// ─── ScrollShotOverlay ────────────────────────────────────────────────────────

ScrollShotOverlay::ScrollShotOverlay(const QRect &capture_rect, QWidget *parent)
    : QWidget(parent)
    , capture_rect_(capture_rect)
    , logical_w_(capture_rect.width())
{
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);

    // 捕获区域边框提示：独立窗口，只画边框，不遮挡内容
    border_hint_ = new QWidget(nullptr);
    border_hint_->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                                  Qt::WindowTransparentForInput);
    border_hint_->setAttribute(Qt::WA_TranslucentBackground);
    border_hint_->setAttribute(Qt::WA_ShowWithoutActivating);
    border_hint_->setAttribute(Qt::WA_DeleteOnClose);
    border_hint_->setGeometry(capture_rect_.adjusted(-2, -2, 2, 2));
    border_hint_->installEventFilter(this);

    const auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(6);

    label_ = new QLabel(tr("已捕获 0 屏"), this);
    label_->setAlignment(Qt::AlignCenter);
    label_->setStyleSheet("color: white; font-size: 11pt; background: transparent;");
    layout->addWidget(label_);

    const auto btn_layout = new QHBoxLayout();
    btn_layout->setSpacing(8);

    cancel_btn_ = new QPushButton(tr("取消"), this);
    cancel_btn_->setFixedHeight(28);
    cancel_btn_->setStyleSheet(
        "QPushButton { background: rgba(80,80,80,200); color: white; border: none;"
        " border-radius: 4px; padding: 0 12px; }"
        "QPushButton:hover { background: rgba(120,120,120,220); }");
    connect(cancel_btn_, &QPushButton::clicked, this, &ScrollShotOverlay::onCancel);
    btn_layout->addWidget(cancel_btn_);

    finish_btn_ = new QPushButton(tr("完成"), this);
    finish_btn_->setFixedHeight(28);
    finish_btn_->setStyleSheet(
        "QPushButton { background: rgba(64,158,255,200); color: white; border: none;"
        " border-radius: 4px; padding: 0 12px; }"
        "QPushButton:hover { background: rgba(64,158,255,240); }");
    connect(finish_btn_, &QPushButton::clicked, this, &ScrollShotOverlay::onFinish);
    btn_layout->addWidget(finish_btn_);

    layout->addLayout(btn_layout);

    adjustSize();

    timer_ = new QTimer(this);
    timer_->setInterval(TICK_MS);
    connect(timer_, &QTimer::timeout, this, &ScrollShotOverlay::onTick);
}

void ScrollShotOverlay::start()
{
    positionSelf();
    border_hint_->show();
    show();
    const auto first = captureFrame().toImage().convertToFormat(QImage::Format_RGB32);
    stitched_image_ = first;
    stitched_       = QPixmap::fromImage(first);
    frame_count_    = 1;
    updateLabel();
    timer_->start();
}

void ScrollShotOverlay::positionSelf()
{
    const auto *screen = QGuiApplication::primaryScreen();
    const auto  sg     = screen->geometry();
    const int   margin = 10;

    int x = capture_rect_.right() - width() - margin;
    int y = capture_rect_.bottom() + margin;
    x     = std::clamp(x, sg.left() + margin, sg.right() - width() - margin);
    y     = std::clamp(y, sg.top() + margin, sg.bottom() - height() - margin);
    move(x, y);
}

QPixmap ScrollShotOverlay::captureFrame() const
{
    // grabWindow 在高DPI下返回物理像素的pixmap，需缩放回逻辑尺寸以确保拼接一致
    auto *screen = QGuiApplication::primaryScreen();
    QPixmap frame = screen->grabWindow(
        0, capture_rect_.x(), capture_rect_.y(), capture_rect_.width(), capture_rect_.height());
    const qreal dpr = frame.devicePixelRatio();
    if (dpr > 1.0) {
        frame = frame.scaled(capture_rect_.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        frame.setDevicePixelRatio(1.0);
    }
    return frame;
}

int ScrollShotOverlay::findOverlapOffset(const QImage &last, const QImage &curr) const
{
    // 在 last 的底部取参考条带，在 curr 中从顶向下搜索匹配位置
    // 为应对快速滚动，允许在 curr 的【全高范围】内搜索（而不是仅到 height-STRIP_H）
    const int w = last.width();
    const int h = last.height();

    if (h < STRIP_H * 2 || curr.height() < STRIP_H) return -1;

    const int strip_y = h - STRIP_H; // 参考条带在 last 中的起始行

    const int samples_per_row = std::max(1, w / SAMPLE_STEP);
    const int total_samples   = (STRIP_H / SAMPLE_STEP) * samples_per_row;
    const int score_threshold = total_samples * 25 * 3;

    int best_y     = -1;
    int best_score = score_threshold;

    // 搜索终点：curr.height() - STRIP_H（至少保留 STRIP_H 行新内容）
    // 为支持快速大幅滚动，从 cy=0 一路搜到末尾
    const int search_end = curr.height() - STRIP_H;

    for (int cy = 0; cy <= search_end; cy += SAMPLE_STEP) {
        int score = 0;
        for (int sy = 0; sy < STRIP_H; sy += SAMPLE_STEP) {
            const auto *last_line = reinterpret_cast<const QRgb *>(last.constScanLine(strip_y + sy));
            const auto *curr_line = reinterpret_cast<const QRgb *>(curr.constScanLine(cy + sy));
            for (int sx = 0; sx < w; sx += SAMPLE_STEP) {
                const QRgb lp = last_line[sx];
                const QRgb cp = curr_line[sx];
                score += std::abs(qRed(lp) - qRed(cp)) + std::abs(qGreen(lp) - qGreen(cp)) +
                         std::abs(qBlue(lp) - qBlue(cp));
                if (score >= best_score) goto next_row;
            }
        }
        best_score = score;
        best_y     = cy;
    next_row:;
    }

    if (best_y < 0) return -1;

    // 滚动量不足，视为页面未移动
    if ((strip_y - best_y) < MIN_SCROLL) return -1;

    const int new_content_y = best_y + STRIP_H;
    if (new_content_y >= curr.height()) return -1;

    return new_content_y;
}

void ScrollShotOverlay::appendFrame(const QImage &curr, int offset)
{
    const int new_h   = curr.height() - offset;
    const int total_h = stitched_.height() + new_h;

    QPixmap combined(stitched_.width(), total_h);
    combined.setDevicePixelRatio(1.0);
    {
        QPainter painter(&combined);
        painter.drawPixmap(0, 0, stitched_);
        painter.drawImage(0, stitched_.height(), curr, 0, offset, curr.width(), new_h);
    }
    stitched_       = combined;
    stitched_image_ = combined.toImage().convertToFormat(QImage::Format_RGB32);
    ++frame_count_;
}

void ScrollShotOverlay::updateLabel()
{
    // 以「累计拼接高度 / 单帧高度」估算等效屏数
    const int frame_h    = capture_rect_.height();
    const int equivalent = frame_h > 0 ? (stitched_.height() + frame_h - 1) / frame_h : frame_count_;
    label_->setText(tr("已捕获 %1 屏").arg(equivalent));
}

void ScrollShotOverlay::onTick()
{
    const auto frame = captureFrame().toImage().convertToFormat(QImage::Format_RGB32);

    // stitched_image_ 是已拼接内容的完整图像，用其底部条带与当前帧匹配
    if (stitched_image_.isNull()) {
        stitched_image_ = frame;
        stitched_       = QPixmap::fromImage(frame);
        frame_count_    = 1;
        updateLabel();
        return;
    }

    const int offset = findOverlapOffset(stitched_image_, frame);
    if (offset < 0) return;

    appendFrame(frame, offset);
    updateLabel();

    if (stitched_.height() >= MAX_HEIGHT) {
        label_->setText(tr("已达上限，请点击完成"));
        timer_->stop();
    }
}

void ScrollShotOverlay::onFinish()
{
    timer_->stop();
    border_hint_->close();
    if (!stitched_.isNull()) emit finished(stitched_);
    close();
}

void ScrollShotOverlay::onCancel()
{
    timer_->stop();
    border_hint_->close();
    emit cancelled();
    close();
}

bool ScrollShotOverlay::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == border_hint_ && event->type() == QEvent::Paint) {
        QPainter painter(border_hint_);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QPen(QColor(24, 144, 255), 3, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(border_hint_->rect().adjusted(1, 1, -1, -1));
        return true;
    }
    return QWidget::eventFilter(obj, event);
}

void ScrollShotOverlay::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape)
        onCancel();
    else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        onFinish();
    QWidget::keyPressEvent(event);
}

void ScrollShotOverlay::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(QColor(30, 30, 30, 210));
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(rect(), 8, 8);
}

// ─── ScreenShoter ─────────────────────────────────────────────────────────────

#ifdef Q_OS_LINUX
#include <QDBusInterface>
#include <QDBusReply>
#endif

ScreenShoter::ScreenShoter(QWidget *parent)
    : QGraphicsView(parent)
{
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::BypassWindowManagerHint);
#ifdef NDEBUG
    setWindowFlag(Qt::WindowStaysOnTopHint, true);
#endif

    setAttribute(Qt::WA_TranslucentBackground); // FIXME: otherwise, the screen will be black when full
                                                // screen on Linux

    setFrameStyle(QGraphicsView::NoFrame);
    setContentsMargins({});
    setViewportMargins({});

    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    selector_   = new Selector(this);
    menu_       = new EditingMenu(this);
    magnifier_  = new Magnifier(this);
    undo_stack_ = new QUndoStack(this);

    scene_ = new canvas::Canvas(this);
    setScene(scene_);

    setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);

    connect(scene_, &canvas::Canvas::focusItemChanged, this, [this](auto item, auto, auto reason) {
        if (item != nullptr && reason == Qt::MouseFocusReason) {
            scene()->clearSelection(); // clear selection of text items

            const auto wrapper = dynamic_cast<GraphicsItemWrapper *>(item);
            menu_->paintGraph((wrapper->graph() == canvas::pixmap) ? canvas::none : wrapper->graph());

            menu_->fill(wrapper->filled());
            menu_->setPen(wrapper->pen());
            menu_->setFont(wrapper->font());
        }
    });

    // TODO:
    connect(menu_, &EditingMenu::scroll, this, &ScreenShoter::startScrollShot);

    connect(menu_, &EditingMenu::save, this, &ScreenShoter::save);
    connect(menu_, &EditingMenu::copy, this, &ScreenShoter::copy);
    connect(menu_, &EditingMenu::pin, this, &ScreenShoter::pin);
    connect(menu_, &EditingMenu::exit, this, &ScreenShoter::exit);

    connect(menu_, &EditingMenu::undo, undo_stack_, &QUndoStack::undo);
    connect(menu_, &EditingMenu::redo, undo_stack_, &QUndoStack::redo);

    connect(menu_, &EditingMenu::graphChanged, [this](auto graph) {
        if (graph != canvas::none) {
            selector_->status(SelectorStatus::LOCKED);
        }

        updateCursor(ResizerLocation::DEFAULT);
    });

    connect(menu_, &EditingMenu::penChanged, [this](auto graph, const auto& pen) {
        if (auto item = scene_->focusItem(); item && item->graph() == graph && item->pen() != pen) {
            undo_stack_->push(new PenChangedCommand(item, item->pen(), pen));
        }
    });

    connect(menu_, &EditingMenu::fontChanged, [this](auto graph, const auto& font) {
        if (auto item = scene_->focusOrFirstSelectedItem();
            item && item->graph() == graph && item->font() != font) {
            undo_stack_->push(new FontChangedCommand(item, item->font(), font));
        }
    });

    connect(menu_, &EditingMenu::fillChanged, [this](auto graph, auto filled) {
        if (auto item = scene_->focusItem(); item && item->graph() == graph && item->filled() != filled) {
            undo_stack_->push(new FillChangedCommand(item, filled));
        }
    });

    connect(menu_, &EditingMenu::imageArrived, [this](const auto& pixmap) {
        GraphicsItemWrapper *item = new GraphicsPixmapItem(pixmap, selector_->selected(true).center());

        undo_stack_->push(new CreatedCommand(scene(), dynamic_cast<QGraphicsItem *>(item)));

        item->onhovered([this](auto rl) { updateCursor(rl); });
        item->onmoved([=, this](auto opos) {
            undo_stack_->push(new MoveCommand(dynamic_cast<QGraphicsItem *>(item), opos));
        });
        item->onrotated([=, this](auto angle) { undo_stack_->push(new RotateCommand(item, angle)); });
        item->onresized(
            [=, this](const auto& g, auto l) { undo_stack_->push(new ResizeCommand(item, g, l)); });
    });

    // hide/show menu
    connect(selector_, &Selector::selecting, menu_, &QWidget::hide);
    connect(selector_, &Selector::captured, [this]() {
        menu_->show();
        moveMenu();
    });

    // move menu
    connect(selector_, &Selector::moved, this, &ScreenShoter::moveMenu);
    connect(selector_, &Selector::resized, this, &ScreenShoter::moveMenu);
    connect(selector_, &Selector::statusChanged, [this](auto status) {
        if ((status > SelectorStatus::READY && status < SelectorStatus::CAPTURED) ||
            status == SelectorStatus::RESIZING) {
#ifdef Q_OS_LINUX
            magnifier_->hide(); // FIXME: Ubuntu
#endif

            magnifier_->show();
        }
        else
            magnifier_->hide();
    });

    // stop
    connect(selector_, &Selector::stopped, this, &ScreenShoter::exit);

    selector_->installEventFilter(this);

    //
    connect(undo_stack_, &QUndoStack::canRedoChanged, menu_, &EditingMenu::canRedo);
    connect(undo_stack_, &QUndoStack::canUndoChanged, menu_, &EditingMenu::canUndo);

    // shortcuts
    registerShortcuts();
}

void ScreenShoter::start()
{
    if (isVisible()) return;

    const auto virtual_geometry = probe::graphics::virtual_screen_geometry();

    // geometry
    setGeometry(QRect{ virtual_geometry });  // window geometry == virtual geometry, absolute coordinate
    scene()->setSceneRect(rect());           // relative coordinate, start at (0, 0)
    selector_->setGeometry(rect());          // relative coordinate, start at (0, 0)
    selector_->coordinate(virtual_geometry); // painter coordinate, absolute coordinate

    if (screenshot(virtual_geometry)) {
        //
        selector_->start(probe::graphics::window_filter_t::visible |
                         probe::graphics::window_filter_t::children);

        selector_->show();

        show();

        // Qt::BypassWindowManagerHint: no keyboard input unless call QWidget::activateWindow()
        activateWindow();
    }

    history_idx_ = history_.size();
}

void ScreenShoter::repeat()
{
    start();

    if (!history_.empty()) {
        selector_->select(history_.back());
        selector_->status(SelectorStatus::CAPTURED);
        moveMenu();
    }
}

#ifdef Q_OS_LINUX
void ScreenShoter::DbusScreenshotArrived(uint response, const QVariantMap& results)
{
    if (!response && results.contains(QLatin1String("uri"))) {
        auto       uri  = results.value(QLatin1String("uri")).toString();
        const auto path = uri.remove(QLatin1String("file://"));

        QPixmap background(path);

        setBackground(background, probe::geometry_t{ 0, 0, static_cast<uint32_t>(background.width()),
                                                     static_cast<uint32_t>(background.height()) });
    }
}
#endif

bool ScreenShoter::screenshot(const probe::geometry_t& geometry)
{
#ifdef Q_OS_LINUX
    if (probe::system::windowing_system() == probe::system::windowing_system_t::Wayland) {
        QDBusInterface interface("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                 "org.freedesktop.portal.Screenshot");

        QDBusReply<QDBusObjectPath> reply = interface.call(
            "Screenshot", "", QVariantMap{ { "interactive", false }, { "handle_token", "123456" } });
        if (!reply.isValid()) {
            loge("[WAYLOAD] org.freedesktop.portal.Screenshot");
            return false;
        }
        QDBusConnection::sessionBus().connect(QString(), reply.value().path(),
                                              "org.freedesktop.portal.Request", "Response", this,
                                              SLOT(DbusScreenshotArrived(uint, QVariantMap)));
    }
    else {
#endif
        // TODO: 180ms (4K + 2K two monitors), speed up
        const auto background = QGuiApplication::primaryScreen()->grabWindow(
            probe::graphics::virtual_screen().handle, geometry.x, geometry.y,
            static_cast<int>(geometry.width), static_cast<int>(geometry.height));

        setBackground(background, geometry);

#ifdef Q_OS_LINUX
    }
#endif

    return true;
}

void ScreenShoter::setBackground(const QPixmap& background, const probe::geometry_t& geometry)
{
    setBackgroundBrush(background);
    setCacheMode(QGraphicsView::CacheBackground);

    magnifier_->setGrabPixmap(background, geometry);
}

QBrush ScreenShoter::mosaicBrush()
{
    auto     pixmap = QPixmap::fromImage(backgroundBrush().textureImage());
    QPainter painter(&pixmap);

    scene()->render(&painter, pixmap.rect());

    return pixmap.scaled(pixmap.size() / 9, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .scaled(pixmap.size());
}

void ScreenShoter::updateCursor(const ResizerLocation location)
{
    if (menu_->graph() & canvas::eraser) {
        setCursor(
            QCursor{ cursor::circle(menu_->pen().width(), { QColor{ 136, 136, 136 }, 3 }, Qt::NoBrush) });
    }
    else if (menu_->graph() & canvas::mosaic) {
        setCursor(QCursor{ cursor::circle(menu_->pen().width(), { QColor{ 136, 136, 136 }, 3 }) });
    }
    else {
        setCursor(getCursorByLocation(location, Qt::CrossCursor));
    }
}

bool ScreenShoter::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == selector_ && event->type() == QEvent::MouseButtonDblClick) {
        mouseDoubleClickEvent(dynamic_cast<QMouseEvent *>(event));
        return true;
    }
    return QGraphicsView::eventFilter(obj, event);
}

void ScreenShoter::mousePressEvent(QMouseEvent *event)
{
    const auto ffs_item = scene_->focusOrFirstSelectedItem();

    // focus or blur
    QGraphicsView::mousePressEvent(event);

    //
    if (event->isAccepted() || event->button() != Qt::LeftButton) return;

    // do not create text items if a text item has focus
    if ((creating_item_ && (creating_item_->graph() & canvas::text)) ||
        (ffs_item && (ffs_item->graph() & canvas::text))) {
        if (menu_->graph() & canvas::text) return;
    }

    createItem(event->pos());
}

void ScreenShoter::mouseMoveEvent(QMouseEvent *event)
{
    if (creating_item_) {
        creating_item_->push(event->pos());
    }

    QGraphicsView::mouseMoveEvent(event);
}

void ScreenShoter::mouseReleaseEvent(QMouseEvent *event)
{
    if (creating_item_) {
        // handle the creating text item on the second click
        if (!((creating_item_->graph() & canvas::text) && creating_item_ == scene_->focusItem())) {
            finishItem();
        }
    }

    QGraphicsView::mouseReleaseEvent(event);
}

void ScreenShoter::createItem(const QPointF& pos)
{
    if (menu_->graph() == canvas::none) return;
    if (creating_item_) finishItem();

    switch (menu_->graph()) {
    case canvas::rectangle: creating_item_ = new GraphicsRectItem(pos, pos); break;
    case canvas::ellipse:   creating_item_ = new GraphicsEllipseItem(pos, pos); break;
    case canvas::arrow:     creating_item_ = new GraphicsArrowItem(pos, pos); break;
    case canvas::line:      creating_item_ = new GraphicsLineItem(pos, pos); break;
    case canvas::curve:     creating_item_ = new GraphicsCurveItem(pos, sceneRect().size()); break;
    case canvas::counter:   creating_item_ = new GraphicsCounterItem(pos, ++counter_); break;
    case canvas::text:      creating_item_ = new GraphicsTextItem(pos); break;
    case canvas::eraser:
        creating_item_ = new GraphicsEraserItem(pos, sceneRect().size(), backgroundBrush());
        break;
    case canvas::mosaic:
        creating_item_ = new GraphicsMosaicItem(pos, sceneRect().size(), mosaicBrush());
        break;
    default: return;
    }

    creating_item_->setPen(menu_->pen());
    creating_item_->setFont(menu_->font());
    creating_item_->fill(menu_->filled());

    creating_item_->onhovered([this](auto location) { updateCursor(location); });
    creating_item_->onmoved([item = creating_item_, this](auto opos) {
        undo_stack_->push(new MoveCommand(dynamic_cast<QGraphicsItem *>(item), opos));
    });
    creating_item_->onresized([item = creating_item_, this](const auto& g, auto l) {
        undo_stack_->push(new ResizeCommand(item, g, l));
        if (item->graph() & canvas::text) menu_->setFont(item->font());
    });

    creating_item_->onrotated(
        [item = creating_item_, this](auto angle) { undo_stack_->push(new RotateCommand(item, angle)); });

    scene_->add(creating_item_);
    dynamic_cast<QGraphicsItem *>(creating_item_)->setFocus();
}

void ScreenShoter::finishItem()
{
    if (!creating_item_) return;

    if (creating_item_->invalid()) {
        scene_->remove(creating_item_);
        delete creating_item_;
        creating_item_ = nullptr;
    }
    else {
        creating_item_->end();
        undo_stack_->push(new CreatedCommand(scene(), dynamic_cast<QGraphicsItem *>(creating_item_)));
        creating_item_ = nullptr;
    }
}

void ScreenShoter::wheelEvent(QWheelEvent *event)
{
    if (menu_->graph() != canvas::none) {
        const auto delta = event->angleDelta().y() / 120;
        auto       pen   = menu_->pen();

        if ((event->modifiers() & Qt::CTRL) && canvas::has_color(menu_->graph())) {
            auto color = pen.color();
            color.setAlpha(std::clamp<int>(color.alpha() + delta * 5, 0, 255));
            pen.setColor(color);

            menu_->setPen(pen, false);
            updateCursor(ResizerLocation::DEFAULT);
        }
        else if (canvas::has_width(menu_->graph())) {
            pen.setWidth(std::clamp(menu_->pen().width() + delta, 1, 71));

            menu_->setPen(pen, false);
            updateCursor(ResizerLocation::DEFAULT);
        }
    }

    QGraphicsView::wheelEvent(event);
}

void ScreenShoter::keyPressEvent(QKeyEvent *event)
{
    // hotkey 'Space': move the selector while editing.
    if (selector_->status() == SelectorStatus::LOCKED && event->key() == Qt::Key_Space &&
        !event->isAutoRepeat()) {
        selector_->status(SelectorStatus::CAPTURED);
    }
    else if (event->key() == Qt::Key_Shift && !event->isAutoRepeat()) {
        selector_->showCrossHair(true);
    }

    QGraphicsView::keyPressEvent(event);
}

void ScreenShoter::keyReleaseEvent(QKeyEvent *event)
{
    // stop moving the selector while editing
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        selector_->status(SelectorStatus::LOCKED);
    }
    else if (event->key() == Qt::Key_Shift && !event->isAutoRepeat()) {
        selector_->showCrossHair(false);
    }

    QGraphicsView::keyReleaseEvent(event);
}

void ScreenShoter::mouseDoubleClickEvent(QMouseEvent *event)
{
    QGraphicsView::mouseDoubleClickEvent(event);

    if (!event->isAccepted() && event->button() == Qt::LeftButton &&
        selector_->status() >= SelectorStatus::CAPTURED) {
        copy();
    }
}

void ScreenShoter::moveMenu()
{
    const auto area  = selector_->selected();
    auto       right = area.right() - menu_->width() - 2;
    if (right < geometry().left()) right = geometry().left();

    if (area.bottom() + (menu_->height() * 2 + 6 /*margin*/) < geometry().bottom()) {
        menu_->move(right, area.bottom() + 6);
        menu_->setSubMenuShowAbove(false);
    }
    else if (area.top() - (menu_->height() * 2 + 6) > 0) {
        menu_->move(right, area.top() - menu_->height() - 6);
        menu_->setSubMenuShowAbove(true);
    }
    else {
        menu_->move(right, area.bottom() - menu_->height() - 6);
        menu_->setSubMenuShowAbove(true);
    }
}

std::pair<QPixmap, QPoint> ScreenShoter::snip()
{
    if (history_.empty() || history_.back() != selector_->prey()) {
        history_.push_back(selector_->prey());
        history_idx_ = history_.size();
    }

    const auto rect = selector_->selected(); // absolute coordinate
    selector_->close();
    scene()->clearFocus();
    scene()->clearSelection();

    return { grab(rect.translated(-geometry().topLeft())), rect.topLeft() };
}

void ScreenShoter::save()
{
    QString default_filename =
        "Capturer_" + QDateTime::currentDateTime().toString("yyyy-MM-dd_hhmmss_zzz") + ".png";

#ifdef _WIN32
    QString filename = QFileDialog::getSaveFileName(
        this, tr("Save Image"), config::snip::path + QDir::separator() + default_filename,
        "PNG(*.png);;JPEG(*.jpg *.jpeg);;BMP(*.bmp)");
#elif __linux__
    QString filename = config::snip::path + QDir::separator() + default_filename;
#endif

    if (!filename.isEmpty()) {
        config::snip::path = QFileInfo(filename).absolutePath();

        auto [pixmap, _] = snip();
        if (!pixmap.save(filename)) {
            loge("failed to save the captured image.");
        }

        emit saved(filename);
        exit();
    }
}

void ScreenShoter::copy()
{
    clipboard::push(snip().first);

    exit();
}

void ScreenShoter::pin()
{
    auto [pixmap, pos] = snip();

    emit pinData(clipboard::push(pixmap, pos));

    exit();
}

void ScreenShoter::exit()
{
    magnifier_->close();

    menu_->close();

    selector_->close();

    //! 1.
    undo_stack_->clear();

    //! 2.
    if (creating_item_ && !dynamic_cast<QGraphicsItem *>(creating_item_)->scene()) delete creating_item_;
    creating_item_ = {};

    //! 3.
    scene_->clear();

    counter_ = 0;

    setBackgroundBrush(Qt::NoBrush);

    QWidget::close();
}

void ScreenShoter::setStyle(const SelectorStyle& style)
{
    selector_->setBorderStyle(QPen{
        style.border_color,
        static_cast<qreal>(style.border_width),
        style.border_style,
    });

    selector_->setMaskStyle(style.mask_color);
}

size_t ScreenShoter::history_index()
{
    return std::clamp<size_t>(history_idx_, 0, std::max<size_t>(0, history_.size() - 1));
}

void ScreenShoter::registerShortcuts()
{
    connect(new QShortcut(Qt::CTRL | Qt::Key_S, this), &QShortcut::activated, [this] {
        if (any(selector_->status() & SelectorStatus::CAPTURED) ||
            any(selector_->status() & SelectorStatus::LOCKED)) {
            save();
        }
    });

    connect(new QShortcut(Qt::Key_P, this), &QShortcut::activated, [this] {
        if (selector_->status() == SelectorStatus::CAPTURED ||
            selector_->status() == SelectorStatus::LOCKED) {
            pin();
        }
    });

    connect(new QShortcut(Qt::Key_Tab, this), &QShortcut::activated, [this] {
        if (magnifier_->isVisible()) {
            magnifier_->toggleFormat();
        }
    });

    connect(new QShortcut(Qt::Key_F5, this), &QShortcut::activated, [this] {
        const auto menuv = menu_->isVisible();
        const auto magnv = magnifier_->isVisible();
        if (menuv) menu_->hide();
        if (magnv) magnifier_->hide();

        hide();

        screenshot(probe::graphics::virtual_screen_geometry());

        show();

        if (menuv) menu_->show();
        if (magnv) magnifier_->show();

        activateWindow();
    });

    // clang-format off
    connect(new QShortcut(Qt::Key_Return, this), &QShortcut::activated, [this] { copy(); exit(); });
    connect(new QShortcut(Qt::Key_Enter,  this), &QShortcut::activated, [this] { copy(); exit(); });
    connect(new QShortcut(Qt::Key_Escape, this), &QShortcut::activated, [this] { exit(); });

    connect(new QShortcut(Qt::CTRL | Qt::Key_Z,             this), &QShortcut::activated, undo_stack_, &QUndoStack::undo);
    connect(new QShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_Z, this), &QShortcut::activated, undo_stack_, &QUndoStack::redo);
    // clang-format on

    connect(new QShortcut(QKeySequence::Delete, this), &QShortcut::activated, [this] {
        if (!scene()->selectedItems().isEmpty() || scene()->focusItem()) {
            undo_stack_->push(new DeleteCommand(scene()));
        }
    });

    connect(new QShortcut(Qt::Key_PageUp, this), &QShortcut::activated, [=, this] {
        if (history_.empty()) return;

        history_idx_ = history_idx_ > 0 ? history_idx_ - 1 : 0;
        selector_->select(history_[history_index()]);
        selector_->status(SelectorStatus::CAPTURED);
        moveMenu();
    });

    connect(new QShortcut(Qt::Key_PageDown, this), &QShortcut::activated, [=, this] {
        if (history_.empty()) return;

        history_idx_ = history_idx_ < history_.size() - 1 ? history_idx_ + 1 : history_.size() - 1;
        selector_->select(history_[history_index()]);
        selector_->status(SelectorStatus::CAPTURED);
        moveMenu();
    });

    connect(new QShortcut(Qt::CTRL | Qt::Key_C, this), &QShortcut::activated, [=, this] {
        if (selector_->status() < SelectorStatus::CAPTURED && magnifier_->isVisible()) {
            clipboard::push(magnifier_->color(), magnifier_->colorname());
            exit();
        }
    });
}

void ScreenShoter::startScrollShot()
{
    if (selector_->status() < SelectorStatus::CAPTURED) return;
    if (scroll_overlay_) return;

    const auto capture_rect = selector_->selected(); // 绝对坐标

    menu_->hide();
    magnifier_->hide();
    setWindowOpacity(0.0); // 透明化，不遮挡被截目标

    scroll_overlay_ = new ScrollShotOverlay(capture_rect, nullptr);

    connect(scroll_overlay_, &ScrollShotOverlay::finished, this, [this](const QPixmap &stitched) {
        endScrollShot();
        emit scrollShotReady(stitched);
        exit();
    });

    connect(scroll_overlay_, &ScrollShotOverlay::cancelled, this, [this]() {
        endScrollShot();
        exit();
    });

    scroll_overlay_->start();
}

void ScreenShoter::endScrollShot()
{
    if (scroll_overlay_) {
        scroll_overlay_->close();
        scroll_overlay_->deleteLater();
        scroll_overlay_ = nullptr;
    }
    setWindowOpacity(1.0);
}