#pragma once

#include <QMap>
#include <QObject>
#include <QString>

class Arbiter;
class QStackedWidget;
class QWidget;

// Mirrors whichever tab's video output is currently playing onto a second
// physical screen (this car's planned rear display), when one is actually
// connected - HDMI-1 isn't wired up yet as of this writing. Front-screen
// playback (JellyfinTab, YouTubeTab, DashcamTab) is untouched either way -
// each still owns its own QMediaPlayer/QGraphicsScene exactly as before.
// The trick this relies on is that a QGraphicsScene can be shown by more
// than one QGraphicsView at once, each rendering the current frame
// independently - so "duplicating" the output is just a second
// DashcamVideoView pointed at the same scene, not a second player or a
// custom video surface.
class RearDisplay : public QObject {
    Q_OBJECT

   public:
    RearDisplay(Arbiter &arbiter);

    // view's QGraphicsScene must already be the one live-showing that tab's
    // video (i.e. construct this alongside the tab's own front video_widget,
    // pointed at the same scene/item). Safe to call before a second screen
    // exists - the view is just held until one shows up.
    void register_view(QString id, QWidget *view);

    // Call whenever that source starts playing - safe before a second
    // screen exists. Deliberately has no matching "stop"/"hide": front-end
    // navigation (tapping back, switching tabs or pages) never touches this,
    // which is the entire point - the rear screen keeps playing regardless
    // of where the front screen navigates to.
    void show_view(QString id);

   private:
    void attach_to_screen();

    Arbiter &arbiter;
    QWidget *window = nullptr;
    QStackedWidget *stack = nullptr;
    QMap<QString, QWidget *> views;
    QString pending_show;
};
