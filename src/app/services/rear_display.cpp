#include <QApplication>
#include <QScreen>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "app/arbiter.hpp"
#include "app/services/rear_display.hpp"

RearDisplay::RearDisplay(Arbiter &arbiter)
    : QObject(qApp)
    , arbiter(arbiter)
{
    connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen *) { this->attach_to_screen(); });

    // Deferred rather than called directly here - RearDisplay is constructed
    // very early in app bootstrap, before the event loop is running (see
    // Jellyfin::net()'s comment for the crash that taught us that lesson -
    // querying/positioning windows this early has been similarly unreliable
    // elsewhere in this app).
    QTimer::singleShot(0, this, [this] { this->attach_to_screen(); });
}

void RearDisplay::register_view(QString id, QWidget *view)
{
    this->views[id] = view;
    if (this->stack)
        this->stack->addWidget(view);
}

void RearDisplay::show_view(QString id)
{
    this->pending_show = id;
    if (this->stack && this->views.contains(id))
        this->stack->setCurrentWidget(this->views[id]);
}

void RearDisplay::attach_to_screen()
{
    if (this->window)
        return;

    // The primary (front) screen is always first - see QGuiApplication's own
    // docs on screens(). Assumes exactly one other screen (this car's rear
    // display) rather than handling 3+, which is what's actually being
    // built here.
    const QList<QScreen *> screens = qApp->screens();
    if (screens.size() < 2)
        return;
    QScreen *rear_screen = screens[1];

    this->window = new QWidget();
    this->window->setWindowFlags(Qt::FramelessWindowHint);
    this->window->setStyleSheet("background-color: black;");

    QVBoxLayout *layout = new QVBoxLayout(this->window);
    layout->setContentsMargins(0, 0, 0, 0);
    this->stack = new QStackedWidget(this->window);
    layout->addWidget(this->stack);

    // Anything registered/shown before this screen showed up gets applied
    // now rather than lost.
    for (QWidget *view : this->views)
        this->stack->addWidget(view);
    if (!this->pending_show.isEmpty() && this->views.contains(this->pending_show))
        this->stack->setCurrentWidget(this->views[this->pending_show]);

    // X11/RandR multi-output is one big virtual desktop (confirmed via
    // xrandr on this device), so plain absolute positioning by the target
    // screen's own reported geometry is enough - no explicit QWindow/screen
    // assignment needed the way Wayland would require.
    this->window->move(rear_screen->geometry().topLeft());
    this->window->resize(rear_screen->geometry().size());
    this->window->showFullScreen();
}
