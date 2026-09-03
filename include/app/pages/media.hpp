#pragma once

#include <QFileInfo>
#include <QGraphicsScene>
#include <QGraphicsVideoItem>
#include <QGraphicsView>
#include <QListWidget>
#include <QMap>
#include <QMediaPlayer>
#include <QMultimedia>
#include <QPluginLoader>
#include <QResizeEvent>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtWidgets>

#include "app/config.hpp"
#include "app/pages/page.hpp"
#include "app/services/jellyfin.hpp"
#include "app/widgets/selector.hpp"
#include "app/widgets/tuner.hpp"

class Arbiter;
class DashcamVideoView;

// Plain QSlider only nudges by a page-step when you click/tap the groove
// rather than jumping to that position - drag-to-seek still works, but a
// single tap (the natural touchscreen gesture) barely moves it, which is
// what "no way to scrub" actually was. Jumps straight to the tapped spot.
class ClickableSlider : public QSlider {
    Q_OBJECT

   public:
    using QSlider::QSlider;

   protected:
    void mousePressEvent(QMouseEvent *event) override;
};

class MediaPage : public QTabWidget, public Page {
    Q_OBJECT

   public:
    MediaPage(Arbiter &arbiter, QWidget *parent = nullptr);

    void init() override;
};

class BluetoothPlayerTab : public QWidget {
    Q_OBJECT

   public:
    BluetoothPlayerTab(Arbiter &arbiter, QWidget *parent = nullptr);

   private:
    Arbiter &arbiter;

    QWidget *track_widget();
    QWidget *controls_widget();
};

class RadioPlayerTab : public QWidget {
    Q_OBJECT

   public:
    RadioPlayerTab(Arbiter &arbiter, QWidget *parent = nullptr);
    ~RadioPlayerTab();

   private:
    static QMap<QString, QFileInfo> get_plugins();

    Arbiter &arbiter;
    Config *config;
    QMap<QString, QFileInfo> plugins;
    QPluginLoader loader;
    Tuner *tuner;
    Selector *plugin_selector;
    QPushButton *play_button;

    void load_plugin();
    QWidget *dialog_body();
    QWidget *tuner_widget();
    QWidget *controls_widget();

};

class DabPlayerTab : public QWidget {
    Q_OBJECT

   public:
    DabPlayerTab(Arbiter &arbiter, QWidget *parent = nullptr);
    ~DabPlayerTab();

   private:
    static QMap<QString, QFileInfo> get_plugins();

    Arbiter &arbiter;
    Config *config;
    QMap<QString, QFileInfo> plugins;
    QPluginLoader loader;
    QTimer *poll_timer;
    Selector *plugin_selector;
    QLabel *status_label;
    QLabel *now_playing_label;
    QListWidget *services_widget;
    QPushButton *stop_button;

    void load_plugin();
    void refresh();
    QWidget *dialog_body();
    QWidget *header_widget();
};

class LocalPlayerTab : public QWidget {
    Q_OBJECT

   public:
    LocalPlayerTab(Arbiter &arbiter, QWidget *parent = nullptr);

    static QString durationFmt(int total_ms);

   private:
    Arbiter &arbiter;

    QWidget *playlist_widget();
    QWidget *seek_widget();
    QWidget *controls_widget();
    void populate_dirs(QString path, QListWidget *dirs_widget);
    void populate_tracks(QString path, QListWidget *tracks_widget);

    Config *config;
    QMediaPlayer *player;
    QLabel *path_label;
};

class JellyfinTab : public QWidget {
    Q_OBJECT

   public:
    JellyfinTab(Arbiter &arbiter, QWidget *parent = nullptr);

   private:
    Arbiter &arbiter;
    Config *config;
    QMediaPlayer *player;
    QStackedWidget *content_stack;  // swaps between browser_widget (lists) and video_widget (Movie/Episode playback)
    QListWidget *browser_widget;
    QGraphicsScene *video_scene;
    QGraphicsVideoItem *video_item;
    DashcamVideoView *video_widget;
    QLabel *breadcrumb_label;
    QLabel *status_label;
    QLineEdit *username_input;
    QLineEdit *password_input;

    QList<Jellyfin::Item> nav_stack;      // breadcrumb trail (root not included)
    QList<Jellyfin::Item> current_items;  // whatever's currently listed - drives playlist build + prev/next

    void navigate(QString parentId, QString label, bool push);
    void populate(QList<Jellyfin::Item> items);
    void play_from(int index);
    QWidget *header_widget();
    QWidget *seek_widget();
    QWidget *controls_widget();
    QWidget *settings_dialog_body();
};

// QVideoWidget renders through a native (X11) overlay window on the
// GStreamer backend, which sits outside Qt's own widget stacking/clipping -
// inside a QTabWidget that let other tabs' widgets visibly bleed through
// underneath it, and it doesn't reliably grow to fill its layout stretch
// either (see conversation, "dashcam overspill" / "not taking the full
// window"). QGraphicsVideoItem paints frames as normal Qt-drawn content
// instead, so it behaves like any other widget. DashcamVideoView keeps the
// item fit to the viewport on every resize.
class DashcamVideoView : public QGraphicsView {
    Q_OBJECT

   public:
    DashcamVideoView(QGraphicsScene *scene, QGraphicsVideoItem *item, QWidget *parent = nullptr);

   protected:
    void resizeEvent(QResizeEvent *event) override;

   private:
    QGraphicsVideoItem *item;
};

class DashcamTab : public QWidget {
    Q_OBJECT

   public:
    DashcamTab(Arbiter &arbiter, QWidget *parent = nullptr);

   private:
    static const QString FOOTAGE_DIR;

    Arbiter &arbiter;

    QWidget *clips_widget();
    QWidget *seek_widget();
    QWidget *controls_widget();
    void populate_clips(QListWidget *clips_widget);

    QMediaPlayer *player;
    QGraphicsScene *video_scene;
    QGraphicsVideoItem *video_item;
    DashcamVideoView *video_widget;
};
