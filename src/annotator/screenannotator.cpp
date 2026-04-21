#include "screenannotator.h"

#include "logging.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QTextEdit>
#include <QVBoxLayout>

// ─── TextLabel ───────────────────────────────────────────────────────────────

TextLabel::TextLabel(const QPoint& pos, const QColor& color, int fontSize, QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::StrongFocus);

    const auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);

    editor_ = new QTextEdit(this);
    editor_->setFrameShape(QFrame::NoFrame);
    editor_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    editor_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    editor_->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    editor_->setAttribute(Qt::WA_TranslucentBackground);
    editor_->setStyleSheet(
        QString("QTextEdit { background: transparent; color: %1; font-size: %2px; border: none; }")
            .arg(color.name())
            .arg(fontSize));
    editor_->installEventFilter(this);

    layout->addWidget(editor_);
    setLayout(layout);

    // 自动跟随内容调整大小
    connect(editor_->document(), &QTextDocument::contentsChanged, this, [this] {
        const int w = std::max(120, static_cast<int>(editor_->document()->idealWidth()) + 16);
        const int h = std::max(36, static_cast<int>(editor_->document()->size().height()) + 8);
        resize(w, h);
        editor_->resize(w - 4, h - 4);
    });

    move(pos);
    resize(180, 40);
    show();
}

void TextLabel::startEditing()
{
    editor_->setFocus();
    editor_->setCursorWidth(1);
}

bool TextLabel::isEmpty() const { return editor_->toPlainText().trimmed().isEmpty(); }

void TextLabel::setSelected(bool v)
{
    selected_ = v;
    update();
}

void TextLabel::mousePressEvent(QMouseEvent* ev)
{
    emit clicked(this);
    QWidget::mousePressEvent(ev);
}

void TextLabel::keyPressEvent(QKeyEvent* ev)
{
    if (ev->key() == Qt::Key_Escape) {
        emit removeRequested(this);
        return;
    }
    QWidget::keyPressEvent(ev);
}

void TextLabel::focusOutEvent(QFocusEvent* ev)
{
    if (isEmpty()) emit removeRequested(this);
    QWidget::focusOutEvent(ev);
}

void TextLabel::paintEvent(QPaintEvent* ev)
{
    QWidget::paintEvent(ev);
    if (selected_) {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QPen pen(QColor(80, 160, 255, 180), 1, Qt::DashLine);
        p.setPen(pen);
        p.setBrush(QColor(80, 160, 255, 20));
        p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 3, 3);
    }
}

// ─── ScreenAnnotator ─────────────────────────────────────────────────────────

ScreenAnnotator::ScreenAnnotator(QWidget* parent)
    : QWidget(parent,
              Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_TransparentForMouseEvents, true); // 默认穿透
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    // 覆盖整个虚拟桌面
    const auto vg = QApplication::primaryScreen()->virtualGeometry();
    setGeometry(vg);
    logi("[annotator] created, geometry=({},{} {}x{})", vg.x(), vg.y(), vg.width(), vg.height());

    // 必须先 show() 建立窗口句柄，WA_TransparentForMouseEvents 才能正常切换
    show();
    // 初始状态穿透，不抢焦点
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
}

void ScreenAnnotator::enterDrawMode()
{
    if (draw_mode_) return;
    draw_mode_ = true;
    text_mode_ = false;
    drawing_   = false;
    logi("[annotator] enterDrawMode");

    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setCursor(Qt::CrossCursor);
    raise();
    update();
}

void ScreenAnnotator::leaveDrawMode()
{
    if (!draw_mode_) return;
    logi("[annotator] leaveDrawMode, strokes={}", strokes_.size());
    draw_mode_ = false;
    drawing_   = false;
    current_stroke_ = {};

    if (!hasContent() && !text_mode_) {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }
    setCursor(Qt::ArrowCursor);
    update();
}

void ScreenAnnotator::toggleTextMode()
{
    text_mode_ = !text_mode_;
    draw_mode_ = false;
    logi("[annotator] toggleTextMode -> text_mode={}", text_mode_);

    if (text_mode_) {
        setAttribute(Qt::WA_TransparentForMouseEvents, false);
        setCursor(Qt::IBeamCursor);
        raise();
        update();
    }
    else {
        setCursor(Qt::ArrowCursor);
        if (!hasContent()) {
            setAttribute(Qt::WA_TransparentForMouseEvents, true);
        }
        update();
    }
}

void ScreenAnnotator::clearAll()
{
    logi("[annotator] clearAll, strokes={} labels={}", strokes_.size(), labels_.size());
    strokes_.clear();
    for (auto* l : labels_) l->deleteLater();
    labels_.clear();
    selected_label_ = nullptr;
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    update();
}

void ScreenAnnotator::setPenColor(const QColor& c) { pen_color_ = c; }
void ScreenAnnotator::setPenWidth(int w) { pen_width_ = w; }
void ScreenAnnotator::setTextColor(const QColor& c) { text_color_ = c; }
void ScreenAnnotator::setFontSize(int s) { font_size_ = s; }

void ScreenAnnotator::deselectAll()
{
    for (auto* l : labels_) l->setSelected(false);
    selected_label_ = nullptr;
}

void ScreenAnnotator::removeLabel(TextLabel* label)
{
    if (selected_label_ == label) selected_label_ = nullptr;
    labels_.erase(std::remove(labels_.begin(), labels_.end(), label), labels_.end());
    label->deleteLater();
    update();

    if (!hasContent() && !draw_mode_ && !text_mode_)
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
}

void ScreenAnnotator::mousePressEvent(QMouseEvent* ev)
{
    logi("[annotator] mousePressEvent btn={} draw={} text={}", (int)ev->button(), draw_mode_, text_mode_);
    if (draw_mode_ && ev->button() == Qt::LeftButton) {
        drawing_              = true;
        current_stroke_       = Stroke{};
        current_stroke_.color = pen_color_;
        current_stroke_.width = pen_width_;
        current_stroke_.points.push_back(ev->pos());
        deselectAll();
    }
    else if (text_mode_ && ev->button() == Qt::LeftButton) {
        deselectAll();
        auto* label = new TextLabel(ev->pos(), text_color_, font_size_, this);
        labels_.push_back(label);
        connect(label, &TextLabel::removeRequested, this, &ScreenAnnotator::removeLabel);
        connect(label, &TextLabel::clicked, this, [this](TextLabel* l) {
            deselectAll();
            l->setSelected(true);
            selected_label_ = l;
        });
        label->startEditing();
    }

    QWidget::mousePressEvent(ev);
}

void ScreenAnnotator::mouseMoveEvent(QMouseEvent* ev)
{
    if (draw_mode_ && drawing_) {
        current_stroke_.points.push_back(ev->pos());
        update();
    }
    QWidget::mouseMoveEvent(ev);
}

void ScreenAnnotator::mouseReleaseEvent(QMouseEvent* ev)
{
    if (draw_mode_ && drawing_ && ev->button() == Qt::LeftButton) {
        drawing_ = false;
        if (current_stroke_.points.size() > 1) {
            strokes_.push_back(current_stroke_);
        }
        current_stroke_ = {};
        update();
    }
    QWidget::mouseReleaseEvent(ev);
}

void ScreenAnnotator::keyPressEvent(QKeyEvent* ev)
{
    if (ev->key() == Qt::Key_Escape) {
        // ESC：先尝试撤销选中的文字，再撤销最后一条笔画
        if (selected_label_) {
            removeLabel(selected_label_);
            return;
        }
        if (!strokes_.empty()) {
            strokes_.pop_back();
            update();
            return;
        }
    }
    else if (ev->key() == Qt::Key_Z && ev->modifiers() & Qt::ControlModifier) {
        // Ctrl+Z 撤销最后一笔
        if (!strokes_.empty()) {
            strokes_.pop_back();
            update();
        }
    }
    QWidget::keyPressEvent(ev);
}

void ScreenAnnotator::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    // 调试：画模式时画半透明背景确认窗口可见
    if (draw_mode_) {
        p.fillRect(rect(), QColor(255, 0, 0, 8)); // 极淡红色背景
    }
    else if (text_mode_) {
        p.fillRect(rect(), QColor(0, 0, 255, 8)); // 极淡蓝色背景
    }

    // 画已完成的笔画
    for (const auto& stroke : strokes_) {
        if (stroke.points.size() < 2) continue;
        QPen pen(stroke.color, stroke.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        for (size_t i = 1; i < stroke.points.size(); ++i)
            p.drawLine(stroke.points[i - 1], stroke.points[i]);
    }

    // 画当前正在绘制的笔画
    if (drawing_ && current_stroke_.points.size() >= 2) {
        QPen pen(current_stroke_.color, current_stroke_.width, Qt::SolidLine, Qt::RoundCap,
                 Qt::RoundJoin);
        p.setPen(pen);
        for (size_t i = 1; i < current_stroke_.points.size(); ++i)
            p.drawLine(current_stroke_.points[i - 1], current_stroke_.points[i]);
    }
}
