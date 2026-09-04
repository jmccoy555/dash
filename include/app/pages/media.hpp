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
#include <QShowEvent>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtWidgets>

#include "app/config.hpp"
#include "app/pages/page.hpp"
#include "app/services/jellyfin.hpp"
#include "app/services/recently_played.hpp"
#include "app/services/youtube.hpp"
#include "app/widgets/selector.hpp"
#include "app/widgets/tuner.hpp"
#include "plugins/dab_plugin.hpp"

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

   protected:
    void resizeEvent(QResizeEvent *event) override;

   private:
    QList<QWidget *> tabs;  // in the same order added - index lines up with the tab label passed to addTab()
    QList<QLabel *> tab_icons;  // one per tab, in the same order - see resize_tabs()

    void update_tab_visibility(QStringList disabled);
    // Icon-only tabs are narrow enough that QTabBar packs them off to the
    // left with the rest of the bar sitting empty - stretching each one to
    // an equal share of the full width instead spreads them across, the
    // same evenly-spaced look as the icon rail down the left edge. Redone
    // on every resize since fullscreen toggling changes how much width
    // there is to divide up.
    void resize_tabs();
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

    // Retunes to a station by id - the Recent tab's entry point, same call
    // a tile's own click handler makes.
    void play_external(QString service_id);

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
    QScrollArea *services_area;      // scroll container - services_container lives inside it
    QWidget *services_container;     // rebuilt (grouped-by-letter grid of tiles) only when the station list actually changes
    QWidget *letter_index;           // A-Z (+#) jump strip alongside services_area - one button per letter actually present, rebuilt alongside services_container
    QPushButton *stop_button;

    QString services_signature;              // id:label pairs of the last render - guards against rebuilding (and losing scroll position) every poll tick for nothing
    QMap<QString, QPushButton *> tiles;       // service id -> its tile, so now-playing highlighting can update without a full rebuild

    void load_plugin();
    void refresh();
    void rebuild_services(QList<DabService> services);
    QPushButton *service_tile(DabService service);
    QWidget *dialog_body();
    QWidget *header_widget();
};

class LocalPlayerTab : public QWidget {
    Q_OBJECT

   public:
    LocalPlayerTab(Arbiter &arbiter, QWidget *parent = nullptr);

    static QString durationFmt(int total_ms);

    // Rebuilds the playlist from path's containing folder (same as tapping
    // it normally would) and plays it - the Recent tab's entry point for
    // replaying a local track.
    void play_external(QString path);

   private:
    Arbiter &arbiter;

    QWidget *header_widget();
    QWidget *seek_widget();
    QWidget *controls_widget();
    void navigate(QString path);
    void populate(QString path);
    void search();
    void populate_search_results();
    QToolButton *build_track_tile(QString track_path, QStringList siblings, int index);

    Config *config;
    QMediaPlayer *player;
    QScrollArea *browser_area;
    QWidget *browser_container;  // the grid - rebuilt (cleared + repopulated) on every navigate()/search(), same pattern as JellyfinTab
    QLabel *path_label;
    QPushButton *home_button;
    QLineEdit *search_input;
    QString current_path;
    QString search_query;  // empty when not searching - set by search(), read by populate_search_results()
    QMap<QString, QToolButton *> track_tiles;  // absolute path -> its tile, so the currently-playing one can be highlighted without a full rebuild
};

class JellyfinTab : public QWidget {
    Q_OBJECT

   public:
    JellyfinTab(Arbiter &arbiter, QWidget *parent = nullptr);

    // Plays a single item directly by id/type, with no sibling queue (unlike
    // play_from(), which walks current_items) - the Recent tab's entry point,
    // since history only remembers the item itself, not whatever list it was
    // originally played from.
    void play_external(QString item_id, Jellyfin::ItemType type);

   private:
    Arbiter &arbiter;
    Config *config;
    QMediaPlayer *player;
    QStackedWidget *content_stack;  // swaps between browser_area (tile grid) and video_widget (Movie/Episode playback)
    QScrollArea *browser_area;
    QWidget *browser_container;  // the actual grid - rebuilt (cleared + repopulated) on every navigate()/populate()
    QGraphicsScene *video_scene;
    QGraphicsVideoItem *video_item;
    DashcamVideoView *video_widget;
    DashcamVideoView *rear_video_widget;  // second view of the same video_scene - see RearDisplay
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
    QWidget *settings_widget();
};

class YouTubeTab : public QWidget {
    Q_OBJECT

   public:
    YouTubeTab(Arbiter &arbiter, QWidget *parent = nullptr);

    // Plays a video directly by id - the Recent tab's entry point. Reuses
    // YouTube::play()'s existing cache-or-download behaviour and the
    // ready_to_play handler already wired up in the constructor, so this is
    // just triggering it rather than duplicating any playback logic.
    void play_external(QString video_id);

   private:
    Arbiter &arbiter;
    QMediaPlayer *player;
    QStackedWidget *content_stack;  // swaps between results_area (tile grid) and video_widget (playback)
    QLineEdit *search_input;
    QScrollArea *results_area;
    QWidget *results_container;  // the actual grid - rebuilt on every populate()
    QGraphicsScene *video_scene;
    QGraphicsVideoItem *video_item;
    DashcamVideoView *video_widget;
    DashcamVideoView *rear_video_widget;  // second view of the same video_scene - see RearDisplay
    QLabel *status_label;

    QList<YouTube::Video> current_results;
    int now_playing_index = -1;
    bool showing_favorites = false;  // so a favourite toggled while viewing the favourites list disappears immediately

    void search();
    void show_favorites();
    void populate(QList<YouTube::Video> results);
    void play_from(int index);
    QWidget *header_widget();
    QWidget *seek_widget();
    QWidget *controls_widget();
};

// A tile grid over RecentlyPlayed's history, spanning every source that has
// one - tapping a tile switches straight to that source's own tab and plays
// it there (rather than trying to play it from here with no seek/controls
// bar of its own), so one tap both takes you to the right place and starts
// playback, instead of needing a second tap once you arrive.
class RecentTab : public QWidget {
    Q_OBJECT

   public:
    RecentTab(Arbiter &arbiter, QTabWidget *media_page, LocalPlayerTab *local_tab, JellyfinTab *jellyfin_tab,
              YouTubeTab *dashtube_tab, DabPlayerTab *dab_tab, QWidget *parent = nullptr);

   private:
    Arbiter &arbiter;
    QTabWidget *media_page;
    LocalPlayerTab *local_tab;
    JellyfinTab *jellyfin_tab;
    YouTubeTab *dashtube_tab;
    DabPlayerTab *dab_tab;
    QScrollArea *area;
    QWidget *container;

    void populate();
    void play(const RecentlyPlayed::Entry &entry);
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
    void showEvent(QShowEvent *event) override;

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
    DashcamVideoView *rear_video_widget;  // second view of the same video_scene - see RearDisplay
};
