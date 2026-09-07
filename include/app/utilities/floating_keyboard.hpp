#pragma once

#include <QPoint>
#include <QWidget>

class QQuickWidget;

// A small, movable stand-in for qtvirtualkeyboard's own automatic panel,
// which docks large and fixed with no way to reposition or shrink it (see
// conversation - "takes over the screen"). Embedding an InputPanel QML item
// anywhere in the app at all is enough to make Qt Virtual Keyboard suppress
// its automatic one and hand control here instead - this is its documented
// "application integration" mode, not a workaround (see floating_keyboard.qml).
// One instance lives for the app's lifetime (see dash.cpp), showing/hiding
// itself off QGuiApplication::inputMethod()'s own visibility signal, so any
// QLineEdit/QTextEdit gaining focus anywhere still pops this up exactly like
// the automatic panel used to - just smaller, and draggable by its handle.
class FloatingKeyboard : public QWidget {
    Q_OBJECT

   public:
    FloatingKeyboard(QWidget *parent = nullptr);

   protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

   private:
    // Common tail end of a press/move/release, taking whatever global
    // position was extracted from either a QMouseEvent or a QTouchEvent -
    // see eventFilter() for why both are handled.
    void begin_drag(QPoint global_pos);
    void continue_drag(QPoint global_pos);
    void end_drag();

    QQuickWidget *quick_widget;
    QWidget *handle;
    QPoint drag_offset;
    bool dragging = false;
};
