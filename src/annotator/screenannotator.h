#ifndef CAPTURER_SCREEN_ANNOTATOR_H
#define CAPTURER_SCREEN_ANNOTATOR_H

#include <QColor>
#include <QPoint>
#include <QWidget>
#include <vector>

class QTextEdit;

// 一条涂鸦笔画
struct Stroke {
    std::vector<QPoint> points;
    QColor              color;
    int                 width{ 3 };
};

// 一个文字标注控件
class TextLabel : public QWidget
{
    Q_OBJECT
public:
    explicit TextLabel(const QPoint& pos, const QColor& color, int fontSize,
                       QWidget* parent = nullptr);

    void startEditing();
    bool isEmpty() const;

signals:
    void removeRequested(TextLabel*);
    void clicked(TextLabel*);

protected:
    void mousePressEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
    void paintEvent(QPaintEvent*) override;

private:
    QTextEdit* editor_{};
    bool       selected_{ false };

    void setSelected(bool v);

    friend class ScreenAnnotator;
};

// 全屏透明教鞭覆盖层
class ScreenAnnotator : public QWidget
{
    Q_OBJECT
public:
    explicit ScreenAnnotator(QWidget* parent = nullptr);

    // 进入/退出涂鸦模式（按住快捷键期间调用 enter，松开调用 leave）
    void enterDrawMode();
    void leaveDrawMode();

    // 进入文字标注模式（按下快捷键切换）
    void toggleTextMode();

    // 清除所有内容
    void clearAll();

    bool isDrawMode() const { return draw_mode_; }
    bool isTextMode() const { return text_mode_; }
    bool hasContent() const { return !strokes_.empty() || !labels_.empty(); }

public slots:
    void setPenColor(const QColor& c);
    void setPenWidth(int w);
    void setTextColor(const QColor& c);
    void setFontSize(int s);

protected:
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void paintEvent(QPaintEvent*) override;

private:
    void removeLabel(TextLabel* label);
    void deselectAll();

    bool draw_mode_{ false };
    bool text_mode_{ false };
    bool drawing_{ false };   // 当前正在拖动画线

    // 涂鸦配置
    QColor pen_color_{ Qt::red };
    int    pen_width_{ 3 };

    // 文字配置
    QColor text_color_{ Qt::red };
    int    font_size_{ 16 };

    std::vector<Stroke>     strokes_;
    Stroke                  current_stroke_;
    std::vector<TextLabel*> labels_;
    TextLabel*              selected_label_{ nullptr };
};

#endif //! CAPTURER_SCREEN_ANNOTATOR_H
