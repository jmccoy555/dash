#include <QGuiApplication>
#include <QInputMethod>
#include <QMouseEvent>
#include <QQuickWidget>
#include <QScreen>
#include <QSettings>
#include <QTouchEvent>
#include <QVBoxLayout>

#include "app/utilities/floating_keyboard.hpp"

FloatingKeyboard::FloatingKeyboard(QWidget *parent)
    : QWidget(parent)
    , quick_widget(new QQuickWidget(this))
    , handle(new QWidget(this))
{
    // Tool + FramelessWindowHint keeps this out of any taskbar/alt-tab
    // equivalent and borderless; WindowDoesNotAcceptFocus is the important
    // one - without it, tapping a key would activate this window and steal
    // focus from whatever QLineEdit/QTextEdit is actually being typed into,
    // which would immediately hide the keyboard again (inputMethod's
    // visible tracks focus, not "the keyboard is in use").
    this->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus);
    this->setAttribute(Qt::WA_ShowWithoutActivating);

    // Plain drag handle, deliberately separate from quick_widget - the
    // InputPanel fills quick_widget entirely, so dragging has to come from
    // somewhere that isn't also a key. WA_AcceptTouchEvents + handling
    // QEvent::Touch* directly alongside the mouse events (not just relying
    // on Qt's touch-to-mouse synthesis) matches OpenAutoFrame's own
    // approach for this touchscreen - taps synthesize fine everywhere else
    // in the app, but a held-and-dragged touch is a different, less
    // reliably-synthesized interaction (confirmed live - dragging this
    // handle did nothing before this).
    this->handle->setFixedHeight(28);
    this->handle->setStyleSheet("background-color: #3a3a3a;");
    this->handle->setCursor(Qt::SizeAllCursor);
    this->handle->setAttribute(Qt::WA_AcceptTouchEvents);
    this->handle->installEventFilter(this);

    // Deliberately NOT SizeRootObjectToView - that forces the QML content
    // into whatever box this widget happens to be, which is backwards from
    // what's needed here and was exactly what clipped the keyboard's top
    // row live (a guessed height shorter than the style's real content).
    // The default SizeViewToRootObject does the opposite - this widget
    // (and, via the layout below, the whole window) follows the InputPanel
    // item's own real size instead of the other way around.
    this->quick_widget->setSource(QUrl("qrc:/floating_keyboard.qml"));

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(this->handle);
    layout->addWidget(this->quick_widget, 1);

    // The QML loads asynchronously - quick_widget has no real size yet at
    // this point in the constructor, so positioning (which needs the final
    // height to anchor to the bottom of the screen) has to wait for it.
    connect(this->quick_widget, &QQuickWidget::statusChanged, this, [this](QQuickWidget::Status status) {
        if (status != QQuickWidget::Ready)
            return;

        this->adjustSize();

        QScreen *screen = QGuiApplication::primaryScreen();
        QSize screen_size = screen ? screen->size() : QSize(800, 480);

        QSettings settings;
        if (settings.contains("FloatingKeyboard/pos"))
            this->move(settings.value("FloatingKeyboard/pos").toPoint());
        else
            this->move((screen_size.width() - this->width()) / 2, screen_size.height() - this->height() - 20);
    });

    this->hide();

    connect(QGuiApplication::inputMethod(), &QInputMethod::visibleChanged, this, [this] {
        this->setVisible(QGuiApplication::inputMethod()->isVisible());
    });
}

void FloatingKeyboard::begin_drag(QPoint global_pos)
{
    this->dragging = true;
    this->drag_offset = global_pos - this->pos();
}

void FloatingKeyboard::continue_drag(QPoint global_pos)
{
    if (!this->dragging)
        return;
    this->move(global_pos - this->drag_offset);
}

void FloatingKeyboard::end_drag()
{
    if (!this->dragging)
        return;
    this->dragging = false;
    QSettings settings;
    settings.setValue("FloatingKeyboard/pos", this->pos());
}

bool FloatingKeyboard::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != this->handle)
        return QWidget::eventFilter(watched, event);

    switch (event->type()) {
        case QEvent::MouseButtonPress:
            this->begin_drag(static_cast<QMouseEvent *>(event)->globalPos());
            return true;
        case QEvent::MouseMove:
            this->continue_drag(static_cast<QMouseEvent *>(event)->globalPos());
            return true;
        case QEvent::MouseButtonRelease:
            this->end_drag();
            return true;
        case QEvent::TouchBegin: {
            auto *touch_event = static_cast<QTouchEvent *>(event);
            if (!touch_event->touchPoints().isEmpty())
                this->begin_drag(touch_event->touchPoints().first().screenPos().toPoint());
            return true;
        }
        case QEvent::TouchUpdate: {
            auto *touch_event = static_cast<QTouchEvent *>(event);
            if (!touch_event->touchPoints().isEmpty())
                this->continue_drag(touch_event->touchPoints().first().screenPos().toPoint());
            return true;
        }
        case QEvent::TouchEnd:
        case QEvent::TouchCancel:
            this->end_drag();
            return true;
        default:
            return QWidget::eventFilter(watched, event);
    }
}
