#include <algorithm>
#include <functional>

#include <attachedpictureframe.h>
#include <fileref.h>
#include <flacfile.h>
#include <flacpicture.h>
#include <id3v2tag.h>
#include <math.h>
#include <mp4coverart.h>
#include <mp4file.h>
#include <mp4tag.h>
#include <mpegfile.h>
#include <tag.h>
#include <tpropertymap.h>
#include <BluezQt/PendingCall>
#include <QDirIterator>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMediaPlaylist>
#include <QMouseEvent>
#include <QPair>
#include <QStyle>

#include "app/window.hpp"
#include "app/pages/media.hpp"
#include "app/utilities/icon_engine.hpp"
#include "plugins/dab_plugin.hpp"
#include "plugins/radio_plugin.hpp"

namespace {

// Pulls embedded cover art straight out of the file's own tags - local
// tracks (including anything synced down from Jellyfin, which lands here as
// plain flac/m4a/mp3 files) have no separate thumbnail source the way
// Jellyfin/DashTube's server-fetched art does, so this is the only way to
// give their tiles a picture instead of a blank placeholder. One format
// branch each since TagLib 1.13 has no unified cross-format art API yet.
QPixmap local_track_art(QString path)
{
    TagLib::ByteVector data;

    if (path.endsWith(".mp3", Qt::CaseInsensitive)) {
        TagLib::MPEG::File file(path.toLocal8Bit().constData());
        if (TagLib::ID3v2::Tag *tag = file.ID3v2Tag()) {
            TagLib::ID3v2::FrameList frames = tag->frameList("APIC");
            if (!frames.isEmpty()) {
                auto *frame = static_cast<TagLib::ID3v2::AttachedPictureFrame *>(frames.front());
                data = frame->picture();
            }
        }
    } else if (path.endsWith(".flac", Qt::CaseInsensitive)) {
        TagLib::FLAC::File file(path.toLocal8Bit().constData());
        TagLib::List<TagLib::FLAC::Picture *> pictures = file.pictureList();
        if (!pictures.isEmpty())
            data = pictures.front()->data();
    } else if (path.endsWith(".m4a", Qt::CaseInsensitive)) {
        TagLib::MP4::File file(path.toLocal8Bit().constData());
        if (TagLib::MP4::Tag *tag = file.tag()) {
            TagLib::MP4::ItemMap items = tag->itemMap();
            if (items.contains("covr")) {
                TagLib::MP4::CoverArtList covers = items["covr"].toCoverArtList();
                if (!covers.isEmpty())
                    data = covers.front().data();
            }
        }
    }

    if (data.isEmpty())
        return QPixmap();

    QPixmap pixmap;
    pixmap.loadFromData(reinterpret_cast<const uchar *>(data.data()), data.size());
    return pixmap;
}

// How many tile_width-wide columns actually fit across area's current
// viewport - every media tab's grid used a flat hardcoded column count
// before this, which either left a gap down the right side on a wide
// screen or forced tiles into a horizontal scroll on a narrow one (see
// conversation - "stretch the columns to fit to full width"). Tile width
// varies per tab (DAB's service tiles are wider/shorter than the poster-
// shaped media_tile() ones), so this takes it as a parameter rather than
// assuming one fixed size everywhere.
// Re-invokes callback whenever watched's real size actually changes -
// used to rebuild a media tab's grid once the window manager's async
// fullscreen negotiation actually lands. A tab's very first populate()
// call runs during the whole app's construction, before the main window
// has even been shown, so columns_for_width() sees whatever placeholder
// geometry Qt has at that point; deferring that first call by one event
// loop turn (QTimer::singleShot(0, ...), still worth keeping for the
// common case) isn't always enough on its own - kwin_x11 can still be
// mid-transition to fullscreen by then (confirmed live: still 1 column
// after that deferral). This reacts to whatever real geometry eventually
// arrives, however long that takes, rather than guessing a delay.
class ResizeWatcher : public QObject {
   public:
    ResizeWatcher(QWidget *watched, std::function<void()> callback, QObject *parent)
        : QObject(parent)
        , callback(callback)
    {
        watched->installEventFilter(this);
    }

   protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Resize)
            this->callback();
        return QObject::eventFilter(watched, event);
    }

   private:
    std::function<void()> callback;
};

// "Artist — Album", or whichever half is actually known - shared between
// BluetoothPlayerTab's two independent metadata sources (BlueZ's AVRCP
// data and AA's own media metadata channel), which both need the same
// "don't show an empty dash when one half is missing" handling.
QString compose_track_subtitle(QString artist, QString album)
{
    if (artist.isEmpty())
        return album;
    if (album.isEmpty())
        return artist;
    return QString("%1 — %2").arg(artist, album);
}

int columns_for_width(QScrollArea *area, int tile_width, int max_columns)
{
    // QGridLayout puts real spacing between columns (confirmed live -
    // dividing raw available width by tile width alone overflowed the
    // viewport, since it didn't account for that gap on every column after
    // the first). Estimated rather than read off a real QGridLayout, since
    // this runs before the grid it's sizing exists yet.
    const int spacing = 8;
    int available = area->viewport()->width();
    if (available <= 0 || tile_width <= 0)
        return 1;
    int columns = (available + spacing) / (tile_width + spacing);
    // Capped at max_columns (each tab's previous fixed count) even when
    // more would technically fit - more, smaller-relative tiles packed
    // into a row is worse to hit accurately while driving, not better
    // (see conversation). Filling genuinely unused width matters less
    // than that.
    return std::max(1, std::min(columns, max_columns));
}

}

void ClickableSlider::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        setValue(QStyle::sliderValueFromPosition(minimum(), maximum(), event->x(), width()));
        event->accept();
    }
    QSlider::mousePressEvent(event);
}

MediaPage::MediaPage(Arbiter &arbiter, QWidget *parent)
    : QTabWidget(parent)
    , Page(arbiter, "Media", "play_circle_outline", true, this)
{
    
}

void MediaPage::init()
{
    // Icon-only tabs, matching the rail's look - the name is still needed
    // for the per-tab Settings toggle (see update_tab_visibility() below and
    // MediaSettingsTab::MEDIA_TAB_NAMES in settings.cpp, which has to stay
    // in sync with this list by hand), so it's stashed on the widget itself
    // via setProperty() rather than shown as the tab's actual label.
    // A bit bigger than the rail's own 32 (see window.cpp's iconize call) -
    // matching the raw icon size left this row looking visually thinner/
    // shorter than the rail, since nothing else here pads it out the way
    // the rail's own button style does (see conversation).
    int icon_size = 40 * this->arbiter.layout().scale;

    // The tab's own icon slot only ever gets sized to the icon itself, sat
    // at the tab's leading edge - fine when a label follows it, but with no
    // label left it stranded off to one side of the wide equal-width cells
    // resize_tabs() hands out below. A plain centered QLabel standing in as
    // the tab's whole button (LeftSide "corner" widget, but here filling the
    // entire tab) sidesteps that - resize_tabs() keeps its width matched to
    // the tab's so the centering stays correct as the page resizes.
    auto add_tab = [this, icon_size](QWidget *widget, QString name, QString icon_name) {
        QIcon icon(new StylizedIconEngine(this->arbiter, QString(":/icons/%1.svg").arg(icon_name), true));

        this->addTab(widget, QString());
        int index = this->indexOf(widget);

        QLabel *icon_label = new QLabel(this->tabBar());
        icon_label->setPixmap(icon.pixmap(icon_size, icon_size));
        icon_label->setAlignment(Qt::AlignCenter);
        this->tabBar()->setTabButton(index, QTabBar::LeftSide, icon_label);
        this->tab_icons.append(icon_label);

        widget->setProperty("media_tab_name", name);
        this->tabs.append(widget);
    };

    DabPlayerTab *dab_tab = new DabPlayerTab(this->arbiter, this);
    LocalPlayerTab *local_tab = new LocalPlayerTab(this->arbiter, this);
    JellyfinTab *jellyfin_tab = new JellyfinTab(this->arbiter, this);
    YouTubeTab *dashtube_tab = new YouTubeTab(this->arbiter, this);

    // Built after the four tabs above since it needs live pointers to them
    // (replaying an entry means switching to the right one and calling its
    // play_external()) - added to the bar first regardless, as the natural
    // landing tab.
    add_tab(new RecentTab(this->arbiter, this, local_tab, jellyfin_tab, dashtube_tab, dab_tab, this), "Recent", "history");
    add_tab(new RadioPlayerTab(this->arbiter, this), "Radio", "radio");
    add_tab(dab_tab, "DAB", "dab");
    add_tab(new BluetoothPlayerTab(this->arbiter, this), "Bluetooth", "bluetooth");
    add_tab(local_tab, "Local", "library_music");
    add_tab(jellyfin_tab, "Jellyfin", "video_library");
    add_tab(dashtube_tab, "DashTube", "smart_display");
    add_tab(new DashcamTab(this->arbiter, this), "Dashcam", "videocam");

    // Tabs are still fully constructed either way (their services - Jellyfin,
    // YouTube, DAB's scan - already run app-wide via Session::System
    // regardless), this just hides the tab itself from the bar, the same
    // "stays alive, just not shown" shape as the Pages toggle in Layout
    // settings. Reacts live so flipping a checkbox in settings doesn't need
    // a restart to take effect.
    this->update_tab_visibility(Config::get_instance()->get_disabled_media_tabs());
    connect(Config::get_instance(), &Config::disabled_media_tabs_changed, this, [this](QStringList disabled) {
        this->update_tab_visibility(disabled);
    });
}

void MediaPage::update_tab_visibility(QStringList disabled)
{
    for (QWidget *tab : this->tabs) {
        int index = this->indexOf(tab);
        if (index >= 0)
            this->setTabVisible(index, !disabled.contains(tab->property("media_tab_name").toString()));
    }
    this->resize_tabs();  // fewer/more visible tabs changes what an equal share of the width looks like
}

void MediaPage::resizeEvent(QResizeEvent *event)
{
    QTabWidget::resizeEvent(event);
    this->resize_tabs();
}

void MediaPage::resize_tabs()
{
    int visible_count = 0;
    for (int i = 0; i < this->tabBar()->count(); i++)
        if (this->tabBar()->isTabVisible(i))
            visible_count++;
    if (visible_count == 0 || this->width() <= 0)
        return;

    int tab_width = this->width() / visible_count;
    this->tabBar()->setStyleSheet(QString("QTabBar::tab { min-width: %1px; max-width: %1px; }").arg(tab_width));
    for (QLabel *icon_label : this->tab_icons)
        icon_label->setFixedWidth(tab_width);
}

BluetoothPlayerTab::BluetoothPlayerTab(Arbiter &arbiter, QWidget *parent)
    : QWidget(parent)
    , arbiter(arbiter)
{
    QVBoxLayout *layout = new QVBoxLayout(this);

    layout->addWidget(this->track_widget());
    layout->addWidget(this->controls_widget());
}

QWidget *BluetoothPlayerTab::track_widget()
{
    // Matches the rest of the app's now-playing look (centered art, big
    // title, smaller "Artist — Album" subtitle - see DAB/Jellyfin) rather
    // than the old stacked label-per-field form this used to be (see
    // conversation - "update the bluetooth page to match the rest").
    BluezQt::MediaPlayerPtr media_player = this->arbiter.system().bluetooth.get_media_player().second;
    AAHandler *aa_handler = this->arbiter.android_auto().handler;

    QWidget *widget = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(widget);
    layout->setAlignment(Qt::AlignHCenter);

    int art_size = 240 * this->arbiter.layout().scale;
    QLabel *album_art = new QLabel(widget);
    album_art->setFixedSize(art_size, art_size);
    album_art->setAlignment(Qt::AlignCenter);
    layout->addWidget(album_art, 0, Qt::AlignHCenter);

    QLabel *title = new QLabel((media_player != nullptr) ? media_player->track().title() : QString(), widget);
    title->setAlignment(Qt::AlignCenter);
    title->setFont(this->arbiter.forge().font(24));
    layout->addWidget(title);

    QLabel *subtitle = new QLabel(widget);
    subtitle->setAlignment(Qt::AlignCenter);
    subtitle->setFont(this->arbiter.forge().font(16));
    subtitle->setText(compose_track_subtitle(
        (media_player != nullptr) ? media_player->track().artist() : QString(),
        (media_player != nullptr) ? media_player->track().album() : QString()));
    layout->addWidget(subtitle);

    connect(&this->arbiter.system().bluetooth, &Bluetooth::media_player_track_changed, [title, subtitle](BluezQt::MediaPlayerTrack track){
        title->setText(track.title());
        subtitle->setText(compose_track_subtitle(track.artist(), track.album()));
    });
    connect(aa_handler, &AAHandler::aa_media_metadata_update, [title, subtitle, album_art, art_size](const aasdk::proto::messages::MediaInfoChannelMetadataData& metadata){
        title->setText(QString::fromStdString(metadata.track_name()));
        subtitle->setText(compose_track_subtitle(
            metadata.has_artist_name() ? QString::fromStdString(metadata.artist_name()) : QString(),
            metadata.has_album_name() ? QString::fromStdString(metadata.album_name()) : QString()));
        if (metadata.has_album_art()) {
            QImage art;
            art.loadFromData(QByteArray::fromStdString(metadata.album_art()));
            album_art->setPixmap(QPixmap::fromImage(art).scaled(art_size, art_size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
    });

    return widget;
}

QWidget *BluetoothPlayerTab::controls_widget()
{
    BluezQt::MediaPlayerPtr media_player = this->arbiter.system().bluetooth.get_media_player().second;

    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    QPushButton *previous_button = new QPushButton(widget);
    previous_button->setFlat(true);
    this->arbiter.forge().iconize("skip_previous", previous_button, 56);
    connect(previous_button, &QPushButton::clicked, [this]{
        BluezQt::MediaPlayerPtr media_player = this->arbiter.system().bluetooth.get_media_player().second;
        if (media_player != nullptr)
            media_player->previous()->waitForFinished();
    });
    layout->addWidget(previous_button);

    QPushButton *play_button = new QPushButton(widget);
    play_button->setFlat(true);
    play_button->setCheckable(true);
    bool status = (media_player != nullptr) ? media_player->status() == BluezQt::MediaPlayer::Status::Playing : false;
    play_button->setChecked(status);
    this->arbiter.forge().iconize("play", "pause", play_button, 56);
    connect(play_button, &QPushButton::clicked, [this, play_button](bool checked = false){
        play_button->setChecked(!checked);

        BluezQt::MediaPlayerPtr media_player = this->arbiter.system().bluetooth.get_media_player().second;
        if (media_player != nullptr) {
            if (checked)
                media_player->play()->waitForFinished();
            else
                media_player->pause()->waitForFinished();
        }
    });
    connect(&this->arbiter.system().bluetooth, &Bluetooth::media_player_status_changed, [play_button](BluezQt::MediaPlayer::Status status){
        play_button->setChecked(status == BluezQt::MediaPlayer::Status::Playing);
    });
    layout->addWidget(play_button);

    QPushButton *forward_button = new QPushButton(widget);
    forward_button->setFlat(true);
    this->arbiter.forge().iconize("skip_next", forward_button, 56);
    connect(forward_button, &QPushButton::clicked, [this]{
        BluezQt::MediaPlayerPtr media_player = this->arbiter.system().bluetooth.get_media_player().second;
        if (media_player != nullptr)
            media_player->next()->waitForFinished();
    });
    layout->addWidget(forward_button);

    return widget;
}

QMap<QString, QFileInfo> RadioPlayerTab::get_plugins()
{
    QMap<QString, QFileInfo> plugins;
    for (auto plugin : Session::plugin_dir("radio").entryInfoList(QDir::Files)) {
        if (QLibrary::isLibrary(plugin.absoluteFilePath()))
            plugins[Session::fmt_plugin(plugin.baseName())] = plugin;
    }

    return plugins;
}

RadioPlayerTab::RadioPlayerTab(Arbiter &arbiter, QWidget *parent)
    : QWidget(parent)
    , arbiter(arbiter)
    , config(Config::get_instance())
    , plugins(RadioPlayerTab::get_plugins())
    , loader()
    , tuner(new Tuner(this->arbiter))
    , plugin_selector(new Selector(this->plugins.keys(), this->config->get_radio_plugin(), this->arbiter.forge().font(14), this->arbiter, nullptr, "unloader"))
    , play_button(new QPushButton())
{
    this->play_button->setFlat(true);
    this->play_button->setCheckable(true);
    this->arbiter.forge().iconize("play", "stop", this->play_button, 48);
    connect(this->play_button, &QPushButton::clicked, [this](bool checked){
        if (RadioPlugin *plugin = qobject_cast<RadioPlugin *>(this->loader.instance())) {
            if (checked)
                plugin->play();
            else
                plugin->stop();
        }
        else {
            this->play_button->setChecked(false);
        }
    });

    this->tuner->setValue(this->config->get_radio_station());

    auto layout = new QVBoxLayout(this);
    layout->addStretch(1);
    layout->addWidget(this->tuner_widget(), 1);
    layout->addWidget(this->controls_widget(), 3);
    layout->addStretch(1);

    this->load_plugin();
}

RadioPlayerTab::~RadioPlayerTab()
{
    this->loader.unload();
}

void RadioPlayerTab::load_plugin()
{
    if (this->loader.isLoaded())
        this->loader.unload();

    this->play_button->setChecked(false);

    auto key = this->plugin_selector->get_current();
    if (!key.isNull()) {
        this->loader.setFileName(this->plugins[key].absoluteFilePath());
        if (RadioPlugin *plugin = qobject_cast<RadioPlugin *>(this->loader.instance())) {
            plugin->freq(this->tuner->value() * 100000);
        }
    }
    this->config->set_radio_plugin(key);
}

QWidget *RadioPlayerTab::dialog_body()
{
    auto widget = new QWidget(this);
    auto layout = new QVBoxLayout(widget);

    layout->addStretch();
    layout->addWidget(this->plugin_selector, 0, Qt::AlignCenter);
    layout->addStretch();

    return widget;
}

QWidget *RadioPlayerTab::tuner_widget()
{
    auto widget = new QWidget(this);
    auto layout = new QHBoxLayout(widget);
    layout->setSpacing(0);

    auto dialog = new Dialog(this->arbiter, true, this->window());
    dialog->set_body(this->dialog_body());

    auto load_button = new QPushButton("load");
    connect(load_button, &QPushButton::clicked, [this]{ this->load_plugin(); });
    dialog->set_button(load_button);

    auto settings_button = new QPushButton();
    settings_button->setFlat(true);
    this->arbiter.forge().iconize("settings", settings_button, 24);
    connect(settings_button, &QPushButton::clicked, [dialog]{ dialog->open(); });

    auto station = new QLabel(QString::number(this->tuner->sliderPosition() / 10.0, 'f', 1));
    station->setFont(this->arbiter.forge().font(36, true));
    connect(this->tuner, &Tuner::valueChanged, [this, station](int freq){
        this->config->set_radio_station(freq);
        station->setText(QString::number(freq / 10.0, 'f', 1));
        if (RadioPlugin *plugin = qobject_cast<RadioPlugin *>(this->loader.instance()))
            plugin->freq(freq * 100000);
    });

    // auto info = new QLabel("station info");
    // info->setWordWrap(true);

    layout->addStretch(2);
    layout->addWidget(settings_button);
    layout->addWidget(station, 2);
    // layout->addWidget(info, 3);
    layout->addWidget(this->play_button, 3);
    layout->addStretch(2);

    return widget;
}

QWidget *RadioPlayerTab::controls_widget()
{
    auto widget = new QWidget();
    auto layout = new QHBoxLayout(widget);

    auto prev_station = new QPushButton();
    prev_station->setFlat(true);
    this->arbiter.forge().iconize("chevron_left", prev_station, 56);
    connect(prev_station, &QPushButton::clicked, [tuner = this->tuner]{
        tuner->setSliderPosition(tuner->sliderPosition() - 1);
    });

    auto next_station = new QPushButton();
    next_station->setFlat(true);
    this->arbiter.forge().iconize("chevron_right", next_station, 56);
    connect(next_station, &QPushButton::clicked, [tuner = this->tuner]{
        tuner->setSliderPosition(tuner->sliderPosition() + 1);
    });

    layout->addStretch(1);
    layout->addWidget(prev_station);
    layout->addWidget(this->tuner, 4);
    layout->addWidget(next_station);
    layout->addStretch(1);

    return widget;
}

QMap<QString, QFileInfo> DabPlayerTab::get_plugins()
{
    QMap<QString, QFileInfo> plugins;
    for (auto plugin : Session::plugin_dir("dab").entryInfoList(QDir::Files)) {
        if (QLibrary::isLibrary(plugin.absoluteFilePath()))
            plugins[Session::fmt_plugin(plugin.baseName())] = plugin;
    }

    return plugins;
}

DabPlayerTab::DabPlayerTab(Arbiter &arbiter, QWidget *parent)
    : QWidget(parent)
    , arbiter(arbiter)
    , config(Config::get_instance())
    , plugins(DabPlayerTab::get_plugins())
    , loader()
    , poll_timer(new QTimer(this))
    , plugin_selector(new Selector(this->plugins.keys(), this->config->get_dab_plugin(), this->arbiter.forge().font(14), this->arbiter, nullptr, "unloader"))
    , status_label(new QLabel(this))
    , now_playing_label(new QLabel(this))
    , services_area(new QScrollArea(this))
    , services_container(new QWidget(this->services_area))
    , letter_index(new QWidget(this))
    , stop_button(new QPushButton("stop", this))
{
    this->status_label->setAlignment(Qt::AlignCenter);
    this->now_playing_label->setAlignment(Qt::AlignCenter);

    // A big-tile grid rather than the old flat list - DAB routinely turns up
    // 100+ stations once a scan's run for a while, and a thin list of text
    // rows is exactly the kind of small, precise-tap-required UI that's
    // unusable while actually driving. Letter-grouped since that's the one
    // grouping that needs no extra data - welle-cli's own API only ever
    // gives this app a station's id and label, nothing like a genre.
    new QVBoxLayout(this->services_container);
    this->services_area->setWidget(this->services_container);
    this->services_area->setWidgetResizable(true);
    this->services_area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->services_area->setFrameShape(QFrame::NoFrame);
    Session::Forge::to_touch_scroller(this->services_area);

    // A-Z jump strip - with 100+ stations grouped into 20-odd letter
    // sections, scrolling past all of them to get to, say, "S" is exactly
    // the kind of fiddly, attention-hungry interaction this whole tile grid
    // was meant to get away from. Rebuilt alongside the grid itself in
    // rebuild_services() - only letters actually present get a button.
    new QVBoxLayout(this->letter_index);
    this->letter_index->layout()->setContentsMargins(0, 0, 0, 0);
    this->letter_index->setFixedWidth(50);

    connect(this->stop_button, &QPushButton::clicked, [this]{
        if (DabPlugin *plugin = qobject_cast<DabPlugin *>(this->loader.instance()))
            plugin->stop();
    });

    // There's no manual tuning here - channel is a DAB implementation detail
    // the plugin resolves for itself (see welle_io), and the station list
    // fills in as the scan finds things, so this just polls the plugin's
    // already-synchronous getters to reflect whatever it currently knows.
    connect(this->poll_timer, &QTimer::timeout, [this]{ this->refresh(); });
    this->poll_timer->start(1000);

    QWidget *services_row = new QWidget(this);
    QHBoxLayout *services_row_layout = new QHBoxLayout(services_row);
    services_row_layout->setContentsMargins(0, 0, 0, 0);
    services_row_layout->addWidget(this->services_area, 1);
    services_row_layout->addWidget(this->letter_index);

    auto layout = new QVBoxLayout(this);
    layout->addWidget(this->header_widget());
    layout->addWidget(this->status_label);
    layout->addWidget(this->now_playing_label);
    layout->addWidget(services_row, 1);
    layout->addWidget(this->stop_button, 0, Qt::AlignCenter);

    this->load_plugin();
}

DabPlayerTab::~DabPlayerTab()
{
    this->loader.unload();
}

void DabPlayerTab::load_plugin()
{
    if (this->loader.isLoaded())
        this->loader.unload();

    this->rebuild_services({});
    this->status_label->setText(QString());
    this->now_playing_label->setText(QString());

    auto key = this->plugin_selector->get_current();
    if (!key.isNull())
        this->loader.setFileName(this->plugins[key].absoluteFilePath());
    this->config->set_dab_plugin(key);
}

void DabPlayerTab::refresh()
{
    DabPlugin *plugin = qobject_cast<DabPlugin *>(this->loader.instance());
    if (!plugin) return;

    this->status_label->setText(plugin->scanning() ? "Scanning for stations…" : QString());

    QList<DabService> services = plugin->services();

    // Rebuilding the whole grid on every poll tick (once a second, the whole
    // time a scan runs) would mean losing scroll position every second
    // while someone's trying to actually tap something - only worth doing
    // when the station list has actually changed.
    QStringList signature_parts;
    for (const DabService &service : services)
        signature_parts << (service.id + ":" + service.label);
    QString signature = signature_parts.join('\n');
    if (signature != this->services_signature) {
        this->services_signature = signature;
        this->rebuild_services(services);
    }

    QString now_playing = plugin->now_playing();
    this->now_playing_label->setText(now_playing.isEmpty() ? QString() : QString("Now playing: %1").arg(now_playing));

    // Highlighting can change (tapping a station) independently of the
    // station list itself, so this always runs, whether or not the grid was
    // just rebuilt above.
    for (auto it = this->tiles.constBegin(); it != this->tiles.constEnd(); ++it)
        it.value()->setChecked(!now_playing.isEmpty() && it.value()->text() == now_playing);
}

void DabPlayerTab::rebuild_services(QList<DabService> services)
{
    QLayout *container_layout = this->services_container->layout();
    QLayoutItem *child;
    while ((child = container_layout->takeAt(0)) != nullptr) {
        delete child->widget();
        delete child;
    }
    this->tiles.clear();

    std::sort(services.begin(), services.end(), [](const DabService &a, const DabService &b) {
        return QString::compare(a.label, b.label, Qt::CaseInsensitive) < 0;
    });

    QMap<QString, QList<DabService>> groups;
    for (const DabService &service : services) {
        QString letter = "#";
        if (!service.label.isEmpty() && service.label.at(0).isLetter())
            letter = service.label.at(0).toUpper();
        groups[letter].append(service);
    }

    QLayout *index_layout = this->letter_index->layout();
    QLayoutItem *index_child;
    while ((index_child = index_layout->takeAt(0)) != nullptr) {
        delete index_child->widget();
        delete index_child;
    }

    const int columns = columns_for_width(this->services_area, 340, 5);
    for (auto group = groups.constBegin(); group != groups.constEnd(); ++group) {
        QLabel *header = new QLabel(group.key(), this->services_container);
        header->setFont(this->arbiter.forge().font(20));
        container_layout->addWidget(header);

        QWidget *grid_widget = new QWidget(this->services_container);
        QGridLayout *grid = new QGridLayout(grid_widget);
        int i = 0;
        for (const DabService &service : group.value()) {
            QPushButton *tile = this->service_tile(service);
            grid->addWidget(tile, i / columns, i % columns);
            this->tiles[service.id] = tile;
            i++;
        }
        container_layout->addWidget(grid_widget);

        QPushButton *jump = new QPushButton(group.key(), this->letter_index);
        jump->setFlat(true);
        jump->setFixedHeight(36);
        connect(jump, &QPushButton::clicked, [this, header] { this->services_area->ensureWidgetVisible(header, 0, 0); });
        index_layout->addWidget(jump);
    }
    static_cast<QVBoxLayout *>(index_layout)->addStretch();

    static_cast<QVBoxLayout *>(container_layout)->addStretch();
}

QPushButton *DabPlayerTab::service_tile(DabService service)
{
    QPushButton *tile = new QPushButton(service.label, this->services_container);
    tile->setCheckable(true);
    tile->setFixedSize(340, 100);
    tile->setFont(this->arbiter.forge().font(16));

    QString id = service.id;
    QString label = service.label;
    connect(tile, &QPushButton::clicked, [this, id, label] {
        if (DabPlugin *plugin = qobject_cast<DabPlugin *>(this->loader.instance()))
            plugin->play(id);

        this->arbiter.system().recently_played.record({"DAB", id, label, QString(), 0});
    });

    return tile;
}

void DabPlayerTab::play_external(QString service_id)
{
    if (DabPlugin *plugin = qobject_cast<DabPlugin *>(this->loader.instance()))
        plugin->play(service_id);
}

QWidget *DabPlayerTab::dialog_body()
{
    auto widget = new QWidget(this);
    auto layout = new QVBoxLayout(widget);

    layout->addStretch();
    layout->addWidget(this->plugin_selector, 0, Qt::AlignCenter);
    layout->addStretch();

    return widget;
}

QWidget *DabPlayerTab::header_widget()
{
    auto widget = new QWidget(this);
    auto layout = new QHBoxLayout(widget);

    auto dialog = new Dialog(this->arbiter, true, this->window());
    dialog->set_body(this->dialog_body());

    auto load_button = new QPushButton("load");
    connect(load_button, &QPushButton::clicked, [this]{ this->load_plugin(); });
    dialog->set_button(load_button);

    auto settings_button = new QPushButton();
    settings_button->setFlat(true);
    this->arbiter.forge().iconize("settings", settings_button, 24);
    connect(settings_button, &QPushButton::clicked, [dialog]{ dialog->open(); });

    auto rescan_button = new QPushButton();
    rescan_button->setFlat(true);
    this->arbiter.forge().iconize("refresh", rescan_button, 24);
    connect(rescan_button, &QPushButton::clicked, [this]{
        if (DabPlugin *plugin = qobject_cast<DabPlugin *>(this->loader.instance()))
            plugin->rescan();
    });

    layout->addWidget(settings_button);
    layout->addStretch();
    layout->addWidget(rescan_button);

    return widget;
}

LocalPlayerTab::LocalPlayerTab(Arbiter &arbiter, QWidget *parent)
    : QWidget(parent)
    , arbiter(arbiter)
    , config(Config::get_instance())
    , player(new QMediaPlayer(this))
    , browser_area(new QScrollArea(this))
    , browser_container(new QWidget(this->browser_area))
    , path_label(new QLabel(this))
    , home_button(nullptr)
    , search_input(nullptr)
{
    QMediaPlaylist *playlist = new QMediaPlaylist(this);
    // Playback mode is set for real by controls_widget()'s shuffle/repeat
    // buttons (defaulting to Repeat All, same as this used to hardcode).
    this->player->setPlaylist(playlist);

    // Registers with AAHandler so AA starting playback can pause this (and
    // this starting playback can pause AA) - see controls_widget() and
    // AAHandler::mediaPlaybackUpdate. Physical media buttons also check
    // active_media_source there to decide whether to route to AA or here.
    this->arbiter.android_auto().handler->local_player = this->player;

    new QVBoxLayout(this->browser_container);
    this->browser_area->setWidget(this->browser_container);
    this->browser_area->setWidgetResizable(true);
    this->browser_area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->browser_area->setFrameShape(QFrame::NoFrame);
    Session::Forge::to_touch_scroller(this->browser_area);

    connect(this->player->playlist(), &QMediaPlaylist::currentIndexChanged, [this](int idx) {
        QString playing = idx < 0 ? QString() : this->player->playlist()->media(idx).canonicalUrl().toLocalFile();
        for (auto it = this->track_tiles.constBegin(); it != this->track_tiles.constEnd(); ++it)
            it.value()->setChecked(it.key() == playing);
    });

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(this->header_widget());
    layout->addWidget(this->browser_area, 1);
    layout->addWidget(this->seek_widget());
    layout->addWidget(this->controls_widget());

    // Deferred rather than called directly here - this runs during the
    // whole app's construction, before the main window's first show(), so
    // browser_area has no real geometry yet and columns_for_width() would
    // permanently bake in 1 column (confirmed live). Letting the event
    // loop turn over once first means the window's actually been shown and
    // laid out by the time this runs.
    QTimer::singleShot(0, this, [this] { this->navigate(this->current_path.isEmpty() ? this->config->get_media_home() : this->current_path); });
    // Backstop for the above - see ResizeWatcher's own comment.
    new ResizeWatcher(this->browser_area->viewport(), [this] {
        this->navigate(this->current_path.isEmpty() ? this->config->get_media_home() : this->current_path);
    }, this);
}

QWidget *LocalPlayerTab::header_widget()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    this->home_button = new QPushButton(widget);
    this->home_button->setFlat(true);
    this->home_button->setCheckable(true);
    this->arbiter.forge().iconize("playlist_add", "playlist_add_check", this->home_button, 24);
    connect(this->home_button, &QPushButton::clicked, [this](bool checked = false) {
        this->config->set_media_home(checked ? this->current_path : QDir().absolutePath());
    });
    layout->addWidget(this->home_button);

    this->path_label->setAlignment(Qt::AlignCenter);
    layout->addWidget(this->path_label, 1);

    this->search_input = new QLineEdit(widget);
    this->search_input->setContextMenuPolicy(Qt::NoContextMenu);
    this->search_input->setFont(this->arbiter.forge().font(16));
    this->search_input->setAlignment(Qt::AlignCenter);
    this->search_input->setPlaceholderText("Search Local");
    this->search_input->setFixedWidth(300 * this->arbiter.layout().scale);
    connect(this->search_input, &QLineEdit::returnPressed, [this] { this->search(); });
    layout->addWidget(this->search_input);

    QPushButton *search_button = new QPushButton(widget);
    search_button->setFlat(true);
    this->arbiter.forge().iconize("search", search_button, 24);
    connect(search_button, &QPushButton::clicked, [this] { this->search(); });
    layout->addWidget(search_button);

    return widget;
}

void LocalPlayerTab::navigate(QString path)
{
    this->current_path = path;
    this->path_label->setText(path);
    this->home_button->setChecked(this->config->get_media_home() == path);
    this->populate(path);
}

void LocalPlayerTab::populate(QString path)
{
    this->track_tiles.clear();

    QLayout *container_layout = this->browser_container->layout();
    QLayoutItem *child;
    while ((child = container_layout->takeAt(0)) != nullptr) {
        delete child->widget();
        delete child;
    }

    QWidget *grid_widget = new QWidget(this->browser_container);
    QGridLayout *grid = new QGridLayout(grid_widget);
    grid->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    const int columns = columns_for_width(this->browser_area, 180 * this->arbiter.layout().scale, 9);
    int i = 0;

    QDir dir(path);
    if (!dir.isRoot()) {
        QToolButton *up = this->arbiter.forge().media_tile("↲ Back", QString());
        connect(up, &QToolButton::clicked, [this, path] {
            QDir parent(path);
            parent.cdUp();
            this->navigate(parent.absolutePath());
        });
        grid->addWidget(up, 0, 0);
        i++;
    }

    // The Jellyfin sync destination is deliberately never shown as a
    // browsable subfolder itself - its audio gets folded straight into the
    // library root's own track listing below instead (see conversation -
    // synced tracks should show up "as local", not live behind a folder no
    // one taps into).
    QString jellyfin_dir = QDir(this->config->get_jellyfin_offline_dir()).absolutePath();

    QFileInfoList dirs = dir.entryInfoList(QDir::AllDirs | QDir::NoDotAndDotDot | QDir::Readable, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &info : dirs) {
        if (info.absoluteFilePath() == jellyfin_dir)
            continue;
        QToolButton *tile = this->arbiter.forge().media_tile(info.fileName(), QString());
        QString subpath = info.absoluteFilePath();
        connect(tile, &QToolButton::clicked, [this, subpath] { this->navigate(subpath); });
        grid->addWidget(tile, i / columns, i % columns);
        i++;
    }

    QStringList track_names = dir.entryList(QStringList() << "*.flac" << "*.m4a" << "*.mp3", QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase);
    QList<QPair<QString, QString>> tracks;  // (path, display title)
    for (const QString &name : track_names)
        tracks.append({dir.absoluteFilePath(name), QFileInfo(name).completeBaseName()});

    // Only at the library root - showing a synced track again inside every
    // subfolder it doesn't actually belong to would be more confusing than
    // helpful.
    if (QDir(path).absolutePath() == QDir(this->config->get_media_home()).absolutePath())
        tracks.append(this->arbiter.system().jellyfin.offline_audio_tracks());

    std::sort(tracks.begin(), tracks.end(), [](const QPair<QString, QString> &a, const QPair<QString, QString> &b) {
        return QString::compare(a.second, b.second, Qt::CaseInsensitive) < 0;
    });

    QStringList track_paths;
    for (const auto &track : tracks)
        track_paths.append(track.first);

    for (int t = 0; t < tracks.size(); t++) {
        QToolButton *tile = this->build_track_tile(tracks[t].first, tracks[t].second, track_paths, t);
        grid->addWidget(tile, i / columns, i % columns);
        i++;
    }

    container_layout->addWidget(grid_widget);
    static_cast<QVBoxLayout *>(container_layout)->addStretch();
}

QToolButton *LocalPlayerTab::build_track_tile(QString track_path, QString title, QStringList siblings, int index)
{
    QToolButton *tile = this->arbiter.forge().media_tile(title, QString(), local_track_art(track_path));
    tile->setCheckable(true);
    tile->setChecked(this->player->playlist()->currentMedia().canonicalUrl().toLocalFile() == track_path);
    this->track_tiles[track_path] = tile;

    // Only touches the active playlist here, when a track is actually
    // chosen to play - browsing/searching doesn't rebuild it, so switching
    // views doesn't stop whatever's currently playing. Rebuilt from
    // whatever's currently listed (a folder's tracks, or a search's
    // matches) so next/prev walk through that same set.
    connect(tile, &QToolButton::clicked, [this, siblings, index, track_path, title] {
        this->player->playlist()->clear();
        for (const QString &p : siblings)
            this->player->playlist()->addMedia(QMediaContent(QUrl::fromLocalFile(p)));
        this->player->playlist()->setCurrentIndex(index);
        this->player->play();

        this->arbiter.system().recently_played.record({"Local", track_path, title, QString(), 0});
    });

    return tile;
}

void LocalPlayerTab::search()
{
    QString query = this->search_input->text().trimmed();
    if (query.isEmpty())
        return;

    this->search_query = query;
    this->populate_search_results();
}

void LocalPlayerTab::populate_search_results()
{
    this->track_tiles.clear();

    QLayout *container_layout = this->browser_container->layout();
    QLayoutItem *child;
    while ((child = container_layout->takeAt(0)) != nullptr) {
        delete child->widget();
        delete child;
    }

    this->path_label->setText(QString("Search: \"%1\"").arg(this->search_query));

    // Always searches the whole library from media_home down, regardless of
    // which folder browsing was left in - matches how DashTube/Jellyfin's
    // search works (the whole library, not just whatever's currently in
    // view), and is a lot more useful than a folder-scoped search would be.
    // Synced Jellyfin tracks are searched by their real name the same way
    // populate() lists them, not by their id-named file.
    QList<QPair<QString, QString>> tracks;  // (path, display title)
    QDirIterator it(this->config->get_media_home(), QStringList() << "*.flac" << "*.m4a" << "*.mp3",
                     QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
    while (it.hasNext() && tracks.size() < 300) {
        QString path = it.next();
        QString title = QFileInfo(path).completeBaseName();
        if (title.contains(this->search_query, Qt::CaseInsensitive))
            tracks.append({path, title});
    }
    for (const auto &track : this->arbiter.system().jellyfin.offline_audio_tracks()) {
        if (tracks.size() >= 300)
            break;
        if (track.second.contains(this->search_query, Qt::CaseInsensitive))
            tracks.append(track);
    }
    std::sort(tracks.begin(), tracks.end(), [](const QPair<QString, QString> &a, const QPair<QString, QString> &b) {
        return QString::compare(a.second, b.second, Qt::CaseInsensitive) < 0;
    });

    QStringList track_paths;
    for (const auto &track : tracks)
        track_paths.append(track.first);

    QWidget *grid_widget = new QWidget(this->browser_container);
    QGridLayout *grid = new QGridLayout(grid_widget);
    grid->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    const int columns = columns_for_width(this->browser_area, 180 * this->arbiter.layout().scale, 9);
    int i = 0;

    QToolButton *clear = this->arbiter.forge().media_tile("✕ Clear search", QString());
    QString return_path = this->current_path;
    connect(clear, &QToolButton::clicked, [this, return_path] {
        this->search_query.clear();
        this->search_input->clear();
        this->navigate(return_path);
    });
    grid->addWidget(clear, 0, 0);
    i++;

    for (int t = 0; t < tracks.size(); t++) {
        QToolButton *tile = this->build_track_tile(tracks[t].first, tracks[t].second, track_paths, t);
        grid->addWidget(tile, i / columns, i % columns);
        i++;
    }

    container_layout->addWidget(grid_widget);
    static_cast<QVBoxLayout *>(container_layout)->addStretch();
}

void LocalPlayerTab::play_external(QString path)
{
    QDir dir(QFileInfo(path).absolutePath());
    QStringList track_names = dir.entryList(QStringList() << "*.flac" << "*.m4a" << "*.mp3", QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase);

    QStringList track_paths;
    int index = 0;
    for (const QString &name : track_names) {
        QString p = dir.absoluteFilePath(name);
        if (p == path)
            index = track_paths.size();
        track_paths.append(p);
    }

    this->player->playlist()->clear();
    for (const QString &p : track_paths)
        this->player->playlist()->addMedia(QMediaContent(QUrl::fromLocalFile(p)));
    this->player->playlist()->setCurrentIndex(index);
    this->player->play();

    // So the folder grid reflects what's now playing once the user actually
    // looks at this tab, same as tapping there normally would.
    this->search_query.clear();
    this->navigate(dir.absolutePath());
}

QWidget *LocalPlayerTab::seek_widget()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    ClickableSlider *slider = new ClickableSlider(Qt::Orientation::Horizontal, widget);
    slider->setTracking(false);
    slider->setRange(0, 0);
    QLabel *value = new QLabel(LocalPlayerTab::durationFmt(slider->value()), widget);
    connect(slider, &QSlider::sliderReleased,
            [player = this->player, slider]() { player->setPosition(slider->sliderPosition()); });
    connect(slider, &QSlider::sliderMoved,
            [value](int position) { value->setText(LocalPlayerTab::durationFmt(position)); });
    connect(this->player, &QMediaPlayer::durationChanged, [slider](qint64 duration) {
        slider->setValue(0);
        slider->setRange(0, duration);
    });
    connect(this->player, &QMediaPlayer::positionChanged, [slider, value](qint64 position) {
        if (!slider->isSliderDown()) {
            slider->setValue(position);
            value->setText(LocalPlayerTab::durationFmt(position));
        }
    });

    layout->addStretch(4);
    layout->addWidget(slider, 28);
    layout->addWidget(value, 3);
    layout->addStretch(1);

    return widget;
}

QWidget *LocalPlayerTab::controls_widget()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    // QMediaPlaylist::PlaybackMode is one exclusive enum value, not two
    // independent flags, so shuffle+repeat's 2x3 combinations get resolved
    // down to it here rather than each button owning a mode of its own.
    // Repeat-One always wins over shuffle while it's active (matching every
    // other player's convention - shuffling a single repeated track makes
    // no sense); the repeat button's own tri-state (off/all/one) is tracked
    // via a plain dynamic property since setCheckable only ever gives a
    // button two icon states, not three.
    auto playlist = this->player->playlist();
    QPushButton *shuffle_button = new QPushButton(widget);
    QPushButton *repeat_button = new QPushButton(widget);
    auto apply_mode = [playlist, shuffle_button, repeat_button, this] {
        int repeat_state = repeat_button->property("repeat_state").toInt();
        if (repeat_state == 2)
            playlist->setPlaybackMode(QMediaPlaylist::CurrentItemInLoop);
        else if (shuffle_button->isChecked())
            playlist->setPlaybackMode(QMediaPlaylist::Random);
        else if (repeat_state == 1)
            playlist->setPlaybackMode(QMediaPlaylist::Loop);
        else
            playlist->setPlaybackMode(QMediaPlaylist::Sequential);
    };

    // The app-wide stylesheet's checked-highlight rule is QPushButton:
    // !flat:checked - deliberately excludes flat buttons, so a flat
    // checkable button like this one never visually shows its checked
    // state on its own (confirmed live). Scoped override rather than
    // touching that global rule, since this is the first icon-only flat
    // toggle that actually needs a colour change rather than an icon swap
    // (compare home_button/play_button, which swap between two different
    // icons instead).
    shuffle_button->setFlat(true);
    shuffle_button->setCheckable(true);
    shuffle_button->setStyleSheet("QPushButton:checked { background-color: palette(base); border-radius: 8px; }");
    this->arbiter.forge().iconize("shuffle", shuffle_button, 32);
    connect(shuffle_button, &QPushButton::clicked, apply_mode);
    layout->addWidget(shuffle_button);

    repeat_button->setFlat(true);
    repeat_button->setCheckable(true);
    repeat_button->setStyleSheet("QPushButton:checked { background-color: palette(base); border-radius: 8px; }");
    // Defaults to Repeat All, not Off - matches this playlist's previous
    // hardcoded always-Loop behaviour, so adding these buttons doesn't
    // silently change what a queue does before anyone touches them.
    repeat_button->setProperty("repeat_state", 1);
    repeat_button->setChecked(true);
    this->arbiter.forge().iconize("repeat", repeat_button, 32);
    connect(repeat_button, &QPushButton::clicked, [repeat_button, apply_mode, this] {
        int next_state = (repeat_button->property("repeat_state").toInt() + 1) % 3;
        repeat_button->setProperty("repeat_state", next_state);
        repeat_button->setChecked(next_state != 0);
        this->arbiter.forge().iconize(next_state == 2 ? "repeat_one" : "repeat", repeat_button, 32);
        apply_mode();
    });
    apply_mode();

    QPushButton *previous_button = new QPushButton(widget);
    previous_button->setFlat(true);
    this->arbiter.forge().iconize("skip_previous", previous_button, 56);
    connect(previous_button, &QPushButton::clicked, [player = this->player]() {
        if (player->playlist()->currentIndex() < 0) player->playlist()->setCurrentIndex(0);
        player->playlist()->previous();
        player->play();
    });
    layout->addWidget(previous_button);

    QPushButton *play_button = new QPushButton(widget);
    play_button->setFlat(true);
    play_button->setCheckable(true);
    play_button->setChecked(false);
    this->arbiter.forge().iconize("play", "pause", play_button, 56);
    connect(play_button, &QPushButton::clicked, [player = this->player, play_button](bool checked = false) {
        play_button->setChecked(!checked);
        if (checked)
            player->play();
        else
            player->pause();
    });
    connect(this->player, &QMediaPlayer::stateChanged,
            [play_button, aa_handler = this->arbiter.android_auto().handler](QMediaPlayer::State state) {
                play_button->setChecked(state == QMediaPlayer::PlayingState);
                // Claim media focus and pause AA whenever this actually
                // starts playing, regardless of which button/track-click
                // triggered it - one central point instead of duplicating
                // this in every place that calls player->play().
                if (state == QMediaPlayer::PlayingState) {
                    aa_handler->active_media_source = AAHandler::ActiveMediaSource::Local;
                    aa_handler->injectButtonPressHelper(aasdk::proto::enums::ButtonCode::PAUSE, Action::ActionState::Triggered);
                }
            });
    layout->addWidget(play_button);

    QPushButton *forward_button = new QPushButton(widget);
    forward_button->setFlat(true);
    this->arbiter.forge().iconize("skip_next", forward_button, 56);
    connect(forward_button, &QPushButton::clicked, [player = this->player]() {
        player->playlist()->next();
        player->play();
    });
    layout->addWidget(forward_button);
    layout->addWidget(repeat_button);

    return widget;
}

QString LocalPlayerTab::durationFmt(int total_ms)
{
    int mins = (total_ms / (1000 * 60)) % 60;
    int secs = (total_ms / 1000) % 60;

    return QString("%1:%2").arg(mins, 2, 10, QChar('0')).arg(secs, 2, 10, QChar('0'));
}

JellyfinTab::JellyfinTab(Arbiter &arbiter, QWidget *parent)
    : QWidget(parent)
    , arbiter(arbiter)
    , config(Config::get_instance())
    , player(new QMediaPlayer(this))
    , content_stack(new QStackedWidget(this))
    , browser_area(new QScrollArea(this))
    , browser_container(new QWidget(this->browser_area))
    , letter_index(new QWidget(this))
    , video_scene(new QGraphicsScene(this))
    , video_item(new QGraphicsVideoItem())
    , video_widget(new DashcamVideoView(this->video_scene, this->video_item, this))
    , rear_video_widget(new DashcamVideoView(this->video_scene, this->video_item, nullptr))
    , breadcrumb_label(new QLabel("Jellyfin", this))
    , status_label(new QLabel(this))
    , username_input(nullptr)
    , password_input(nullptr)
{
    this->status_label->setAlignment(Qt::AlignCenter);

    new QVBoxLayout(this->browser_container);
    this->browser_area->setWidget(this->browser_container);
    this->browser_area->setWidgetResizable(true);
    this->browser_area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->browser_area->setFrameShape(QFrame::NoFrame);
    Session::Forge::to_touch_scroller(this->browser_area);

    // A-Z jump strip, same pattern as DabPlayerTab's - built alongside
    // populate() below since only letters actually present get a button.
    new QVBoxLayout(this->letter_index);
    this->letter_index->layout()->setContentsMargins(0, 0, 0, 0);
    this->letter_index->setFixedWidth(50);

    QWidget *browse_row = new QWidget(this);
    QHBoxLayout *browse_row_layout = new QHBoxLayout(browse_row);
    browse_row_layout->setContentsMargins(0, 0, 0, 0);
    browse_row_layout->addWidget(this->browser_area, 1);
    browse_row_layout->addWidget(this->letter_index);

    this->video_scene->setBackgroundBrush(Qt::black);
    this->video_scene->addItem(this->video_item);
    this->player->setVideoOutput(this->video_item);  // harmless for audio-only playback, just renders nothing

    connect(this->player, &QMediaPlayer::videoAvailableChanged, this->video_widget, [this](bool) {
        this->video_widget->fitInView(this->video_item, Qt::KeepAspectRatioByExpanding);
    });
    // videoAvailableChanged alone isn't enough here - video_widget lives in a
    // QStackedWidget and only gets given real geometry once it becomes the
    // current widget, which can happen after that signal fires. Both of
    // these re-run the fit once the widget's actual final size/native video
    // size are known, whichever lands second.
    connect(this->video_item, &QGraphicsVideoItem::nativeSizeChanged, this->video_widget, [this](const QSizeF &) {
        this->video_widget->fitInView(this->video_item, Qt::KeepAspectRatioByExpanding);
    });
    connect(this->content_stack, &QStackedWidget::currentChanged, this->video_widget, [this](int index) {
        if (this->content_stack->widget(index) == this->video_widget)
            this->video_widget->fitInView(this->video_item, Qt::KeepAspectRatioByExpanding);
    });

    this->content_stack->addWidget(browse_row);
    this->content_stack->addWidget(this->video_widget);
    this->arbiter.system().rear_display.register_view("Jellyfin", this->rear_video_widget);

    QMediaPlaylist *playlist = new QMediaPlaylist(this->player);
    // Playback mode is set for real by controls_widget()'s shuffle/repeat
    // buttons (defaulting to Repeat All, same as this used to hardcode).
    this->player->setPlaylist(playlist);

    connect(&this->arbiter.system().jellyfin, &Jellyfin::items_ready, this, [this](QString parentId, QList<Jellyfin::Item> items) {
        // A response for a level the user has since navigated away from -
        // ignore it rather than showing stale data.
        QString expected = this->nav_stack.isEmpty() ? QString() : this->nav_stack.last().id;
        if (parentId != expected)
            return;

        this->populate(items);
        this->status_label->setText(QString());
    });

    connect(&this->arbiter.system().jellyfin, &Jellyfin::auth_required, this, [this] {
        this->status_label->setText("Please log in again (see settings)");
    });

    connect(&this->arbiter.system().jellyfin, &Jellyfin::sync_progress, this, [this](int done, int total, QString name) {
        this->status_label->setText(QString("Syncing %1/%2: %3").arg(done).arg(total).arg(name));
    });

    connect(&this->arbiter.system().jellyfin, &Jellyfin::sync_finished, this, [this](int downloaded, int failed) {
        this->status_label->setText(failed > 0
            ? QString("Synced %1 item(s), %2 failed/skipped").arg(downloaded).arg(failed)
            : QString("Synced %1 item(s)").arg(downloaded));
        QTimer::singleShot(4000, this, [this] { this->status_label->setText(QString()); });
    });

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(this->header_widget());
    layout->addWidget(this->status_label);
    layout->addWidget(this->content_stack, 1);
    layout->addWidget(this->seek_widget());
    layout->addWidget(this->controls_widget());

    if (this->arbiter.system().jellyfin.is_authenticated())
        this->arbiter.system().jellyfin.browse(QString());
    else
        this->status_label->setText("Not logged in - see settings");

    // See ResizeWatcher's own comment - covers a browse() response (e.g.
    // already cached) landing before the window's real geometry does.
    new ResizeWatcher(this->browser_area->viewport(), [this] { this->populate(this->current_items); }, this);
}

void JellyfinTab::navigate(QString parentId, QString label, bool push)
{
    if (push) {
        Jellyfin::Item crumb;
        crumb.id = parentId;
        crumb.name = label;
        this->nav_stack.append(crumb);
    } else if (!this->nav_stack.isEmpty()) {
        this->nav_stack.removeLast();
    }

    QStringList crumbs = {"Jellyfin"};
    for (const Jellyfin::Item &crumb : this->nav_stack)
        crumbs.append(crumb.name);
    this->breadcrumb_label->setText(crumbs.join(" › "));

    QString target = this->nav_stack.isEmpty() ? QString() : this->nav_stack.last().id;
    this->status_label->setText("Loading…");
    this->arbiter.system().jellyfin.browse(target);
}

void JellyfinTab::populate(QList<Jellyfin::Item> items)
{
    // Sorted once, up front - both what current_items/play_from() index
    // against and what the letter grouping below walks, so a tile's index
    // always means the same item in both places.
    std::sort(items.begin(), items.end(), [](const Jellyfin::Item &a, const Jellyfin::Item &b) {
        return QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
    });
    this->current_items = items;

    QLayout *container_layout = this->browser_container->layout();
    QLayoutItem *child;
    while ((child = container_layout->takeAt(0)) != nullptr) {
        delete child->widget();
        delete child;
    }

    const int columns = columns_for_width(this->browser_area, 180 * this->arbiter.layout().scale, 9);

    if (!this->nav_stack.isEmpty()) {
        QWidget *back_widget = new QWidget(this->browser_container);
        QGridLayout *back_grid = new QGridLayout(back_widget);
        QToolButton *up = this->arbiter.forge().media_tile("↲ Back", QString());
        connect(up, &QToolButton::clicked, [this] { this->navigate(QString(), QString(), false); });
        back_grid->addWidget(up, 0, 0);
        container_layout->addWidget(back_widget);
    }

    // A-Z jump strip, same pattern as DabPlayerTab's own - only letters
    // actually present get a button, rebuilt alongside the grid itself
    // since which letters exist changes with whatever's currently browsed.
    QLayout *index_layout = this->letter_index->layout();
    QLayoutItem *index_child;
    while ((index_child = index_layout->takeAt(0)) != nullptr) {
        delete index_child->widget();
        delete index_child;
    }

    QMap<QString, QList<int>> groups;  // first letter -> indices into items/current_items
    for (int index = 0; index < items.size(); index++) {
        QString letter = "#";
        if (!items[index].name.isEmpty() && items[index].name.at(0).isLetter())
            letter = items[index].name.at(0).toUpper();
        groups[letter].append(index);
    }

    for (auto group = groups.constBegin(); group != groups.constEnd(); ++group) {
        QLabel *header = new QLabel(group.key(), this->browser_container);
        header->setFont(this->arbiter.forge().font(20));
        container_layout->addWidget(header);

        QWidget *grid_widget = new QWidget(this->browser_container);
        QGridLayout *grid = new QGridLayout(grid_widget);
        grid->setAlignment(Qt::AlignLeft | Qt::AlignTop);  // media_tile()'s poster-shaped tiles are narrower than the content area can fill edge-to-edge - pack left rather than stretching big gaps between columns
        int i = 0;
        for (int index : group.value()) {
            const Jellyfin::Item &item = items[index];

            QToolButton *tile = this->arbiter.forge().media_tile(item.name, this->arbiter.system().jellyfin.image_url(item.id).toString());

            if (item.type == Jellyfin::ItemType::Container) {
                connect(tile, &QToolButton::clicked, [this, item] { this->navigate(item.id, item.name, true); });
                grid->addWidget(tile, i / columns, i % columns);
            } else {
                connect(tile, &QToolButton::clicked, [this, index] { this->play_from(index); });

                // The star overlays the tile's own top-right corner rather than
                // sitting in a separate row - QGridLayout allows two widgets in
                // the same cell as long as their alignment flags don't both
                // claim the same space, which is exactly what a small corner
                // badge over a big tile needs.
                QWidget *cell = new QWidget(grid_widget);
                QGridLayout *cell_layout = new QGridLayout(cell);
                cell_layout->setContentsMargins(0, 0, 0, 0);
                cell_layout->addWidget(tile, 0, 0);

                QPushButton *star = new QPushButton(cell);
                star->setFlat(true);
                star->setCheckable(true);
                star->setChecked(item.isFavorite);
                this->arbiter.forge().iconize("favorite_border", "favorite", star, 20);
                QString item_id = item.id;
                connect(star, &QPushButton::clicked, [this, item_id](bool checked) {
                    this->arbiter.system().jellyfin.toggle_favorite(item_id, checked);
                });
                cell_layout->addWidget(star, 0, 0, Qt::AlignTop | Qt::AlignRight);

                grid->addWidget(cell, i / columns, i % columns);
            }
            i++;
        }
        container_layout->addWidget(grid_widget);

        QPushButton *jump = new QPushButton(group.key(), this->letter_index);
        jump->setFlat(true);
        jump->setFixedHeight(36);
        connect(jump, &QPushButton::clicked, [this, header] { this->browser_area->ensureWidgetVisible(header, 0, 0); });
        index_layout->addWidget(jump);
    }
    static_cast<QVBoxLayout *>(index_layout)->addStretch();

    static_cast<QVBoxLayout *>(container_layout)->addStretch();
}

void JellyfinTab::play_from(int index)
{
    if (index < 0 || index >= this->current_items.size())
        return;

    const Jellyfin::Item &target = this->current_items[index];
    if (target.type == Jellyfin::ItemType::Container)
        return;

    QMediaPlaylist *playlist = this->player->playlist();
    playlist->clear();

    // Playlist only ever spans one item type at a time - a video queued
    // straight after an audio track (e.g. Favourites mixing both) would
    // otherwise dump straight from a movie into an mp3.
    int start_position = -1;
    for (const Jellyfin::Item &item : this->current_items) {
        if (item.type != target.type)
            continue;

        if (item.id == target.id)
            start_position = playlist->mediaCount();

        // Plays from disk transparently when an item has been synced for
        // offline playback - this is the only place that distinction is
        // made, no separate "offline mode" toggle anywhere.
        QString cached = this->arbiter.system().jellyfin.cached_path(item.id);
        QUrl url = cached.isEmpty() ? this->arbiter.system().jellyfin.stream_url(item.id, item.type) : QUrl::fromLocalFile(cached);
        playlist->addMedia(QMediaContent(url));
    }

    if (start_position >= 0)
        playlist->setCurrentIndex(start_position);

    this->content_stack->setCurrentWidget(target.type == Jellyfin::ItemType::Video ? (QWidget *)this->video_widget : (QWidget *)this->browser_area);
    if (target.type == Jellyfin::ItemType::Video)
        this->arbiter.system().rear_display.show_view("Jellyfin");
    this->player->play();

    this->arbiter.system().recently_played.record(
        {"Jellyfin", target.id, target.name, this->arbiter.system().jellyfin.image_url(target.id).toString(), static_cast<int>(target.type)});
}

void JellyfinTab::play_external(QString item_id, Jellyfin::ItemType type)
{
    QMediaPlaylist *playlist = this->player->playlist();
    playlist->clear();

    QString cached = this->arbiter.system().jellyfin.cached_path(item_id);
    QUrl url = cached.isEmpty() ? this->arbiter.system().jellyfin.stream_url(item_id, type) : QUrl::fromLocalFile(cached);
    playlist->addMedia(QMediaContent(url));
    playlist->setCurrentIndex(0);

    this->content_stack->setCurrentWidget(type == Jellyfin::ItemType::Video ? (QWidget *)this->video_widget : (QWidget *)this->browser_area);
    if (type == Jellyfin::ItemType::Video)
        this->arbiter.system().rear_display.show_view("Jellyfin");
    this->player->play();
}

QWidget *JellyfinTab::header_widget()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    // A Dialog (its own top-level QDialog window, same as every other
    // settings popup in the app) used to hold this, but the on-screen
    // keyboard's key events never reached a QLineEdit living in a separate
    // top-level window from the main one - confirmed live, typing and even
    // backspace silently went nowhere, while the exact same keyboard works
    // fine on e.g. DashTube's search bar, which lives directly in the main
    // window. Login needs actual typing, so this is a page in content_stack
    // instead - same window as everything else, same window the keyboard is
    // known to work in.
    QWidget *settings_view = this->settings_widget();
    this->content_stack->addWidget(settings_view);

    QPushButton *settings_button = new QPushButton(widget);
    settings_button->setFlat(true);
    this->arbiter.forge().iconize("settings", settings_button, 24);
    connect(settings_button, &QPushButton::clicked, [this, settings_view] {
        this->content_stack->setCurrentWidget(settings_view);
    });

    this->breadcrumb_label->setAlignment(Qt::AlignCenter);

    // Shown while video is playing (leaving pauses it rather than letting a
    // movie run on behind a view the user can't see) or while the settings
    // page is up (its only way back, now that it's not a dialog with its
    // own cancel button).
    QPushButton *back_button = new QPushButton(widget);
    back_button->setFlat(true);
    this->arbiter.forge().iconize("arrow_left", back_button, 24);
    back_button->hide();
    connect(back_button, &QPushButton::clicked, [this] {
        if (this->content_stack->currentWidget() == this->video_widget)
            this->player->pause();
        this->content_stack->setCurrentWidget(this->browser_area);
    });
    connect(this->content_stack, &QStackedWidget::currentChanged, [this, back_button, settings_view](int index) {
        QWidget *current = this->content_stack->widget(index);
        back_button->setVisible(current == this->video_widget || current == settings_view);
    });

    // Same visibility rule as back_button - only makes sense while video is
    // showing. Drives the app's existing fullscreen mechanism (the same one
    // behind the rail's long-press gesture and the "Toggle Fullscreen"
    // action), which hides the icon rail/sidebar for more screen width -
    // nothing video-specific needed here.
    QPushButton *fullscreen_button = new QPushButton(widget);
    fullscreen_button->setFlat(true);
    fullscreen_button->setCheckable(true);
    fullscreen_button->setChecked(this->arbiter.layout().fullscreen.enabled);
    this->arbiter.forge().iconize("fullscreen", "fullscreen_exit", fullscreen_button, 24);
    fullscreen_button->hide();
    connect(fullscreen_button, &QPushButton::clicked, [this](bool checked) { this->arbiter.set_fullscreen(checked); });
    connect(&this->arbiter, &Arbiter::fullscreen_changed, fullscreen_button, [fullscreen_button](bool enabled) {
        fullscreen_button->setChecked(enabled);
    });
    connect(this->content_stack, &QStackedWidget::currentChanged, [this, fullscreen_button](int index) {
        fullscreen_button->setVisible(this->content_stack->widget(index) == this->video_widget);
    });

    layout->addWidget(back_button);
    layout->addWidget(settings_button);
    layout->addWidget(this->breadcrumb_label, 1);
    layout->addWidget(fullscreen_button);

    return widget;
}

QWidget *JellyfinTab::settings_widget()
{
    // A full content_stack page now rather than a small popup (see
    // header_widget()'s comment), so the actual form is capped to a
    // sensible width and centered in it rather than stretching edge to edge.
    QWidget *widget = new QWidget(this);
    QWidget *form = new QWidget(widget);
    form->setMaximumWidth(500 * this->arbiter.layout().scale);
    QVBoxLayout *layout = new QVBoxLayout(form);

    QHBoxLayout *center = new QHBoxLayout();
    center->addStretch(1);
    center->addWidget(form);
    center->addStretch(1);

    QVBoxLayout *outer = new QVBoxLayout(widget);
    outer->addStretch(1);
    outer->addLayout(center);
    outer->addStretch(1);

    QLineEdit *server_input = new QLineEdit(this->config->get_jellyfin_server_url(), form);
    server_input->setContextMenuPolicy(Qt::NoContextMenu);
    server_input->setFont(this->arbiter.forge().font(16));
    server_input->setAlignment(Qt::AlignCenter);
    server_input->setPlaceholderText("Server URL");
    connect(server_input, &QLineEdit::textEdited, [this](QString text) { this->config->set_jellyfin_server_url(text); });
    layout->addWidget(server_input);

    this->username_input = new QLineEdit(form);
    this->username_input->setContextMenuPolicy(Qt::NoContextMenu);
    this->username_input->setFont(this->arbiter.forge().font(16));
    this->username_input->setAlignment(Qt::AlignCenter);
    this->username_input->setPlaceholderText("Username");
    layout->addWidget(this->username_input);

    this->password_input = new QLineEdit(form);
    this->password_input->setContextMenuPolicy(Qt::NoContextMenu);
    this->password_input->setFont(this->arbiter.forge().font(16));
    this->password_input->setAlignment(Qt::AlignCenter);
    this->password_input->setEchoMode(QLineEdit::Password);
    this->password_input->setPlaceholderText("Password");
    layout->addWidget(this->password_input);

    QLabel *login_status = new QLabel(form);
    login_status->setAlignment(Qt::AlignCenter);
    layout->addWidget(login_status);

    QPushButton *login_button = new QPushButton("Log in", form);
    connect(login_button, &QPushButton::clicked, [this, login_status] {
        login_status->setText("Logging in…");
        this->arbiter.system().jellyfin.authenticate(this->config->get_jellyfin_server_url(), this->username_input->text(), this->password_input->text());
    });
    layout->addWidget(login_button);

    connect(&this->arbiter.system().jellyfin, &Jellyfin::auth_finished, this, [this, login_status](bool success, QString error) {
        login_status->setText(success ? "Logged in" : error);
        if (success) {
            this->navigate(QString(), QString(), false);
            this->content_stack->setCurrentWidget(this->browser_area);
        }
    });

    layout->addWidget(Session::Forge::br());

    QPushButton *sync_button = new QPushButton("Sync favourites now", form);
    connect(sync_button, &QPushButton::clicked, [this, sync_button] {
        sync_button->setEnabled(false);
        this->arbiter.system().jellyfin.sync_favorites();
    });
    connect(&this->arbiter.system().jellyfin, &Jellyfin::sync_finished, this, [sync_button](int, int) {
        sync_button->setEnabled(true);
    });
    layout->addWidget(sync_button);

    QLabel *sync_note = new QLabel("Favourites also sync automatically every 30 minutes", form);
    sync_note->setAlignment(Qt::AlignCenter);
    sync_note->setWordWrap(true);
    layout->addWidget(sync_note);

    return widget;
}

QWidget *JellyfinTab::seek_widget()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    ClickableSlider *slider = new ClickableSlider(Qt::Orientation::Horizontal, widget);
    slider->setTracking(false);
    slider->setRange(0, 0);
    QLabel *value = new QLabel(LocalPlayerTab::durationFmt(slider->value()), widget);
    // Scrubbing a network video stream doesn't actually work on this device:
    // isSeekable() reports true and setPosition() returns normally, but the
    // position never moves - confirmed live, the hardware h264 decode
    // pipeline silently drops the flushing seek. Restarting the stream at
    // the target offset (Jellyfin's StartTimeTicks) was tried as a
    // workaround but requires transcoding, which this device's Qt
    // Multimedia/GStreamer pipeline then stalls on indefinitely (confirmed
    // the server-side transcode itself produces valid output via curl - the
    // hang is client-side). Left as a plain setPosition() call, which is a
    // no-op for video same as before - audio and local-file playback (both
    // of which do seek correctly) go through this exact same call.
    connect(slider, &QSlider::sliderReleased, [player = this->player, slider]() { player->setPosition(slider->sliderPosition()); });
    connect(slider, &QSlider::sliderMoved,
            [value](int position) { value->setText(LocalPlayerTab::durationFmt(position)); });
    connect(this->player, &QMediaPlayer::durationChanged, [slider](qint64 duration) {
        slider->setValue(0);
        slider->setRange(0, duration);
    });
    connect(this->player, &QMediaPlayer::positionChanged, [slider, value](qint64 position) {
        if (!slider->isSliderDown()) {
            slider->setValue(position);
            value->setText(LocalPlayerTab::durationFmt(position));
        }
    });

    layout->addStretch(4);
    layout->addWidget(slider, 28);
    layout->addWidget(value, 3);
    layout->addStretch(1);

    return widget;
}

QWidget *JellyfinTab::controls_widget()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    // See LocalPlayerTab::controls_widget() for why this resolves down to
    // one PlaybackMode enum instead of two independent flags.
    auto playlist = this->player->playlist();
    QPushButton *shuffle_button = new QPushButton(widget);
    QPushButton *repeat_button = new QPushButton(widget);
    auto apply_mode = [playlist, shuffle_button, repeat_button] {
        int repeat_state = repeat_button->property("repeat_state").toInt();
        if (repeat_state == 2)
            playlist->setPlaybackMode(QMediaPlaylist::CurrentItemInLoop);
        else if (shuffle_button->isChecked())
            playlist->setPlaybackMode(QMediaPlaylist::Random);
        else if (repeat_state == 1)
            playlist->setPlaybackMode(QMediaPlaylist::Loop);
        else
            playlist->setPlaybackMode(QMediaPlaylist::Sequential);
    };

    // The app-wide stylesheet's checked-highlight rule is QPushButton:
    // !flat:checked - deliberately excludes flat buttons, so a flat
    // checkable button like this one never visually shows its checked
    // state on its own (confirmed live). Scoped override rather than
    // touching that global rule, since this is the first icon-only flat
    // toggle that actually needs a colour change rather than an icon swap
    // (compare home_button/play_button, which swap between two different
    // icons instead).
    shuffle_button->setFlat(true);
    shuffle_button->setCheckable(true);
    shuffle_button->setStyleSheet("QPushButton:checked { background-color: palette(base); border-radius: 8px; }");
    this->arbiter.forge().iconize("shuffle", shuffle_button, 32);
    connect(shuffle_button, &QPushButton::clicked, apply_mode);
    layout->addWidget(shuffle_button);

    repeat_button->setFlat(true);
    repeat_button->setCheckable(true);
    repeat_button->setStyleSheet("QPushButton:checked { background-color: palette(base); border-radius: 8px; }");
    // Defaults to Repeat All - matches this playlist's previous hardcoded
    // always-Loop behaviour.
    repeat_button->setProperty("repeat_state", 1);
    repeat_button->setChecked(true);
    this->arbiter.forge().iconize("repeat", repeat_button, 32);
    connect(repeat_button, &QPushButton::clicked, [repeat_button, apply_mode, this] {
        int next_state = (repeat_button->property("repeat_state").toInt() + 1) % 3;
        repeat_button->setProperty("repeat_state", next_state);
        repeat_button->setChecked(next_state != 0);
        this->arbiter.forge().iconize(next_state == 2 ? "repeat_one" : "repeat", repeat_button, 32);
        apply_mode();
    });
    apply_mode();

    QPushButton *previous_button = new QPushButton(widget);
    previous_button->setFlat(true);
    this->arbiter.forge().iconize("skip_previous", previous_button, 56);
    connect(previous_button, &QPushButton::clicked, [player = this->player]() {
        if (player->playlist()->currentIndex() < 0) player->playlist()->setCurrentIndex(0);
        player->playlist()->previous();
        player->play();
    });
    layout->addWidget(previous_button);

    QPushButton *play_button = new QPushButton(widget);
    play_button->setFlat(true);
    play_button->setCheckable(true);
    play_button->setChecked(false);
    this->arbiter.forge().iconize("play", "pause", play_button, 56);
    connect(play_button, &QPushButton::clicked, [player = this->player, play_button](bool checked = false) {
        play_button->setChecked(!checked);
        if (checked)
            player->play();
        else
            player->pause();
    });
    connect(this->player, &QMediaPlayer::stateChanged, [play_button](QMediaPlayer::State state) {
        play_button->setChecked(state == QMediaPlayer::PlayingState);
    });
    layout->addWidget(play_button);

    QPushButton *forward_button = new QPushButton(widget);
    forward_button->setFlat(true);
    this->arbiter.forge().iconize("skip_next", forward_button, 56);
    connect(forward_button, &QPushButton::clicked, [player = this->player]() {
        player->playlist()->next();
        player->play();
    });
    layout->addWidget(forward_button);
    layout->addWidget(repeat_button);

    return widget;
}

YouTubeTab::YouTubeTab(Arbiter &arbiter, QWidget *parent)
    : QWidget(parent)
    , arbiter(arbiter)
    , player(new QMediaPlayer(this))
    , content_stack(new QStackedWidget(this))
    , search_input(nullptr)
    , results_area(new QScrollArea(this))
    , results_container(new QWidget(this->results_area))
    , video_scene(new QGraphicsScene(this))
    , video_item(new QGraphicsVideoItem())
    , video_widget(new DashcamVideoView(this->video_scene, this->video_item, this))
    , rear_video_widget(new DashcamVideoView(this->video_scene, this->video_item, nullptr))
    , status_label(new QLabel(this))
{
    this->status_label->setAlignment(Qt::AlignCenter);

    new QVBoxLayout(this->results_container);
    this->results_area->setWidget(this->results_container);
    this->results_area->setWidgetResizable(true);
    this->results_area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->results_area->setFrameShape(QFrame::NoFrame);
    Session::Forge::to_touch_scroller(this->results_area);

    this->video_scene->setBackgroundBrush(Qt::black);
    this->video_scene->addItem(this->video_item);
    this->player->setVideoOutput(this->video_item);

    // See the equivalent trio of connects in JellyfinTab for why all three -
    // video_widget only gets real geometry once it's the stack's current
    // widget, so a single videoAvailableChanged-triggered fit isn't enough.
    connect(this->player, &QMediaPlayer::videoAvailableChanged, this->video_widget, [this](bool) {
        this->video_widget->fitInView(this->video_item, Qt::KeepAspectRatioByExpanding);
    });
    connect(this->video_item, &QGraphicsVideoItem::nativeSizeChanged, this->video_widget, [this](const QSizeF &) {
        this->video_widget->fitInView(this->video_item, Qt::KeepAspectRatioByExpanding);
    });
    connect(this->content_stack, &QStackedWidget::currentChanged, this->video_widget, [this](int index) {
        if (this->content_stack->widget(index) == this->video_widget)
            this->video_widget->fitInView(this->video_item, Qt::KeepAspectRatioByExpanding);
    });

    this->content_stack->addWidget(this->results_area);
    this->content_stack->addWidget(this->video_widget);
    this->arbiter.system().rear_display.register_view("DashTube", this->rear_video_widget);

    connect(&this->arbiter.system().youtube, &YouTube::search_finished, this,
            [this](QString, QList<YouTube::Video> results, QString error) {
                if (!error.isEmpty()) {
                    this->status_label->setText(error);
                    return;
                }
                this->populate(results);
                this->status_label->setText(results.isEmpty() ? "No results" : QString());
            });

    connect(&this->arbiter.system().youtube, &YouTube::download_progress, this, [this](QString, int percent) {
        this->status_label->setText(QString("Downloading %1%…").arg(percent));
    });

    connect(&this->arbiter.system().youtube, &YouTube::ready_to_play, this, [this](QString, QString localPath) {
        this->status_label->setText(QString());
        this->player->setMedia(QMediaContent(QUrl::fromLocalFile(localPath)));
        this->content_stack->setCurrentWidget(this->video_widget);
        this->arbiter.system().rear_display.show_view("DashTube");
        this->player->play();
    });

    connect(&this->arbiter.system().youtube, &YouTube::download_failed, this, [this](QString, QString error) {
        this->status_label->setText(error);
    });

    // Un-favouriting while looking at the favourites list should drop the
    // row immediately rather than leaving a stale star until the list is
    // reopened.
    connect(&this->arbiter.system().youtube, &YouTube::favorite_toggled, this, [this](QString, bool) {
        if (this->showing_favorites)
            this->show_favorites();
    });

    if (!this->arbiter.system().youtube.is_available())
        this->status_label->setText("yt-dlp not installed");

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(this->header_widget());
    layout->addWidget(this->status_label);
    layout->addWidget(this->content_stack, 1);
    layout->addWidget(this->seek_widget());
    layout->addWidget(this->controls_widget());

    // See ResizeWatcher's own comment.
    new ResizeWatcher(this->results_area->viewport(), [this] { this->populate(this->current_results); }, this);
}

void YouTubeTab::search()
{
    QString query = this->search_input->text().trimmed();
    if (query.isEmpty())
        return;

    this->showing_favorites = false;
    this->status_label->setText("Searching…");
    this->arbiter.system().youtube.search(query);
}

void YouTubeTab::show_favorites()
{
    this->showing_favorites = true;
    QList<YouTube::Video> favorites = this->arbiter.system().youtube.favorites();
    this->populate(favorites);
    this->status_label->setText(favorites.isEmpty() ? "No favourites yet" : QString());
}

void YouTubeTab::populate(QList<YouTube::Video> results)
{
    this->current_results = results;

    QLayout *container_layout = this->results_container->layout();
    QLayoutItem *child;
    while ((child = container_layout->takeAt(0)) != nullptr) {
        delete child->widget();
        delete child;
    }

    QWidget *grid_widget = new QWidget(this->results_container);
    QGridLayout *grid = new QGridLayout(grid_widget);
    grid->setAlignment(Qt::AlignLeft | Qt::AlignTop);  // see the equivalent comment in JellyfinTab::populate()
    const int columns = columns_for_width(this->results_area, 180 * this->arbiter.layout().scale, 9);

    for (int i = 0; i < results.size(); i++) {
        const YouTube::Video &video = results[i];

        QString text = video.uploader.isEmpty() ? video.title : QString("%1 — %2").arg(video.title, video.uploader);
        if (video.durationSecs > 0)
            text += QString("  (%1)").arg(LocalPlayerTab::durationFmt(video.durationSecs * 1000));

        // i.ytimg.com/vi/<id>/mqdefault.jpg is a stable, well-known YouTube
        // thumbnail URL that exists for every video regardless of what
        // yt-dlp's search response happens to report - no need to depend on
        // its thumbnails array.
        QToolButton *tile = this->arbiter.forge().media_tile(text, QString("https://i.ytimg.com/vi/%1/mqdefault.jpg").arg(video.id));
        connect(tile, &QToolButton::clicked, [this, i] { this->play_from(i); });

        // Star overlays the tile's top-right corner - see the equivalent
        // comment in JellyfinTab::populate().
        QWidget *cell = new QWidget(grid_widget);
        QGridLayout *cell_layout = new QGridLayout(cell);
        cell_layout->setContentsMargins(0, 0, 0, 0);
        cell_layout->addWidget(tile, 0, 0);

        QPushButton *star = new QPushButton(cell);
        star->setFlat(true);
        star->setCheckable(true);
        star->setChecked(this->arbiter.system().youtube.is_favorite(video.id));
        this->arbiter.forge().iconize("favorite_border", "favorite", star, 20);
        YouTube::Video video_copy = video;
        connect(star, &QPushButton::clicked, [this, video_copy](bool checked) {
            this->arbiter.system().youtube.set_favorite(video_copy.id, checked, video_copy);
        });
        cell_layout->addWidget(star, 0, 0, Qt::AlignTop | Qt::AlignRight);

        grid->addWidget(cell, i / columns, i % columns);
    }

    container_layout->addWidget(grid_widget);
    static_cast<QVBoxLayout *>(container_layout)->addStretch();
}

void YouTubeTab::play_from(int index)
{
    if (index < 0 || index >= this->current_results.size())
        return;

    const YouTube::Video &video = this->current_results[index];

    this->now_playing_index = index;
    this->status_label->setText("Downloading…");
    this->arbiter.system().youtube.play(video.id);

    this->arbiter.system().recently_played.record(
        {"DashTube", video.id, video.title, QString("https://i.ytimg.com/vi/%1/mqdefault.jpg").arg(video.id), 0});
}

void YouTubeTab::play_external(QString video_id)
{
    this->now_playing_index = -1;
    this->status_label->setText("Downloading…");
    this->arbiter.system().youtube.play(video_id);
}

QWidget *YouTubeTab::header_widget()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    // Same idea as JellyfinTab's back button - only relevant once a video is
    // playing, pauses and swaps back to the results list.
    QPushButton *back_button = new QPushButton(widget);
    back_button->setFlat(true);
    this->arbiter.forge().iconize("arrow_left", back_button, 24);
    back_button->hide();
    connect(back_button, &QPushButton::clicked, [this] {
        this->player->pause();
        this->content_stack->setCurrentWidget(this->results_area);
    });
    connect(this->content_stack, &QStackedWidget::currentChanged, [this, back_button](int index) {
        back_button->setVisible(this->content_stack->widget(index) == this->video_widget);
    });
    layout->addWidget(back_button);

    this->search_input = new QLineEdit(widget);
    this->search_input->setContextMenuPolicy(Qt::NoContextMenu);
    this->search_input->setFont(this->arbiter.forge().font(16));
    this->search_input->setAlignment(Qt::AlignCenter);
    this->search_input->setPlaceholderText("Search DashTube");
    connect(this->search_input, &QLineEdit::returnPressed, [this] { this->search(); });
    layout->addWidget(this->search_input, 1);

    QPushButton *search_button = new QPushButton(widget);
    search_button->setFlat(true);
    this->arbiter.forge().iconize("search", search_button, 24);
    connect(search_button, &QPushButton::clicked, [this] { this->search(); });
    layout->addWidget(search_button);

    QPushButton *favorites_button = new QPushButton(widget);
    favorites_button->setFlat(true);
    this->arbiter.forge().iconize("favorite_border", favorites_button, 24);
    connect(favorites_button, &QPushButton::clicked, [this] { this->show_favorites(); });
    layout->addWidget(favorites_button);

    // Same fullscreen mechanism as JellyfinTab - see there for why it's
    // just arbiter.set_fullscreen() and not anything video-specific.
    QPushButton *fullscreen_button = new QPushButton(widget);
    fullscreen_button->setFlat(true);
    fullscreen_button->setCheckable(true);
    fullscreen_button->setChecked(this->arbiter.layout().fullscreen.enabled);
    this->arbiter.forge().iconize("fullscreen", "fullscreen_exit", fullscreen_button, 24);
    fullscreen_button->hide();
    connect(fullscreen_button, &QPushButton::clicked, [this](bool checked) { this->arbiter.set_fullscreen(checked); });
    connect(&this->arbiter, &Arbiter::fullscreen_changed, fullscreen_button, [fullscreen_button](bool enabled) {
        fullscreen_button->setChecked(enabled);
    });
    connect(this->content_stack, &QStackedWidget::currentChanged, [this, fullscreen_button](int index) {
        fullscreen_button->setVisible(this->content_stack->widget(index) == this->video_widget);
    });
    layout->addWidget(fullscreen_button);

    return widget;
}

QWidget *YouTubeTab::seek_widget()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    ClickableSlider *slider = new ClickableSlider(Qt::Orientation::Horizontal, widget);
    slider->setTracking(false);
    slider->setRange(0, 0);
    QLabel *value = new QLabel(LocalPlayerTab::durationFmt(slider->value()), widget);
    // Unlike JellyfinTab, this always seeks a genuinely local file (already
    // downloaded before playback ever starts), so plain setPosition() here
    // is the proven-reliable path, not the broken network-stream case.
    connect(slider, &QSlider::sliderReleased, [player = this->player, slider]() { player->setPosition(slider->sliderPosition()); });
    connect(slider, &QSlider::sliderMoved,
            [value](int position) { value->setText(LocalPlayerTab::durationFmt(position)); });
    connect(this->player, &QMediaPlayer::durationChanged, [slider](qint64 duration) {
        slider->setValue(0);
        slider->setRange(0, duration);
    });
    connect(this->player, &QMediaPlayer::positionChanged, [slider, value](qint64 position) {
        if (!slider->isSliderDown()) {
            slider->setValue(position);
            value->setText(LocalPlayerTab::durationFmt(position));
        }
    });

    layout->addStretch(4);
    layout->addWidget(slider, 28);
    layout->addWidget(value, 3);
    layout->addStretch(1);

    return widget;
}

QWidget *YouTubeTab::controls_widget()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    // Prev/next step through the current search results by index rather
    // than a QMediaPlaylist - each result needs its own on-demand download,
    // there's no queue of ready-to-play URLs to walk like Jellyfin has.
    QPushButton *previous_button = new QPushButton(widget);
    previous_button->setFlat(true);
    this->arbiter.forge().iconize("skip_previous", previous_button, 56);
    connect(previous_button, &QPushButton::clicked, [this] { this->play_from(this->now_playing_index - 1); });
    layout->addWidget(previous_button);

    QPushButton *play_button = new QPushButton(widget);
    play_button->setFlat(true);
    play_button->setCheckable(true);
    play_button->setChecked(false);
    this->arbiter.forge().iconize("play", "pause", play_button, 56);
    connect(play_button, &QPushButton::clicked, [player = this->player, play_button](bool checked = false) {
        play_button->setChecked(!checked);
        if (checked)
            player->play();
        else
            player->pause();
    });
    connect(this->player, &QMediaPlayer::stateChanged, [play_button](QMediaPlayer::State state) {
        play_button->setChecked(state == QMediaPlayer::PlayingState);
    });
    layout->addWidget(play_button);

    QPushButton *forward_button = new QPushButton(widget);
    forward_button->setFlat(true);
    this->arbiter.forge().iconize("skip_next", forward_button, 56);
    connect(forward_button, &QPushButton::clicked, [this] { this->play_from(this->now_playing_index + 1); });
    layout->addWidget(forward_button);

    return widget;
}

RecentTab::RecentTab(Arbiter &arbiter, QTabWidget *media_page, LocalPlayerTab *local_tab, JellyfinTab *jellyfin_tab,
                      YouTubeTab *dashtube_tab, DabPlayerTab *dab_tab, QWidget *parent)
    : QWidget(parent)
    , arbiter(arbiter)
    , media_page(media_page)
    , local_tab(local_tab)
    , jellyfin_tab(jellyfin_tab)
    , dashtube_tab(dashtube_tab)
    , dab_tab(dab_tab)
    , area(new QScrollArea(this))
    , container(new QWidget(this->area))
{
    new QVBoxLayout(this->container);
    this->area->setWidget(this->container);
    this->area->setWidgetResizable(true);
    this->area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->area->setFrameShape(QFrame::NoFrame);
    Session::Forge::to_touch_scroller(this->area);

    connect(&this->arbiter.system().recently_played, &RecentlyPlayed::changed, this, [this] { this->populate(); });

    QLabel *title = new QLabel("Recent", this);
    title->setAlignment(Qt::AlignCenter);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(title);
    layout->addWidget(this->area, 1);

    // Deferred for the same reason as LocalPlayerTab's initial navigate()
    // call - see its comment.
    QTimer::singleShot(0, this, [this] { this->populate(); });
    // Backstop for the above - see ResizeWatcher's own comment.
    new ResizeWatcher(this->area->viewport(), [this] { this->populate(); }, this);
}

void RecentTab::populate()
{
    QLayout *container_layout = this->container->layout();
    QLayoutItem *child;
    while ((child = container_layout->takeAt(0)) != nullptr) {
        delete child->widget();
        delete child;
    }

    QWidget *grid_widget = new QWidget(this->container);
    QGridLayout *grid = new QGridLayout(grid_widget);
    grid->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    const int columns = columns_for_width(this->area, 180 * this->arbiter.layout().scale, 9);
    int i = 0;

    const QList<RecentlyPlayed::Entry> &entries = this->arbiter.system().recently_played.entries();
    for (const RecentlyPlayed::Entry &entry : entries) {
        QPixmap local_pixmap;
        if (entry.source == "Local")
            local_pixmap = local_track_art(entry.id);

        QToolButton *tile = this->arbiter.forge().media_tile(entry.title, entry.image_url, local_pixmap);
        connect(tile, &QToolButton::clicked, [this, entry] { this->play(entry); });
        grid->addWidget(tile, i / columns, i % columns);
        i++;
    }

    if (entries.isEmpty()) {
        QLabel *empty = new QLabel("Nothing played yet", grid_widget);
        grid->addWidget(empty, 0, 0);
    }

    container_layout->addWidget(grid_widget);
    static_cast<QVBoxLayout *>(container_layout)->addStretch();
}

void RecentTab::play(const RecentlyPlayed::Entry &entry)
{
    if (entry.source == "Local") {
        this->local_tab->play_external(entry.id);
        this->media_page->setCurrentWidget(this->local_tab);
    } else if (entry.source == "Jellyfin") {
        this->jellyfin_tab->play_external(entry.id, static_cast<Jellyfin::ItemType>(entry.variant));
        this->media_page->setCurrentWidget(this->jellyfin_tab);
    } else if (entry.source == "DashTube") {
        this->dashtube_tab->play_external(entry.id);
        this->media_page->setCurrentWidget(this->dashtube_tab);
    } else if (entry.source == "DAB") {
        this->dab_tab->play_external(entry.id);
        this->media_page->setCurrentWidget(this->dab_tab);
    }
}

DashcamVideoView::DashcamVideoView(QGraphicsScene *scene, QGraphicsVideoItem *item, QWidget *parent)
    : QGraphicsView(scene, parent)
    , item(item)
{
    this->setFrameShape(QFrame::NoFrame);
    this->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void DashcamVideoView::resizeEvent(QResizeEvent *event)
{
    QGraphicsView::resizeEvent(event);
    // KeepAspectRatioByExpanding rather than KeepAspectRatio - a dashcam
    // review view should fill the space it's given (cropping any overflow)
    // rather than letterbox, which is what "not taking the full window" was
    // asking for.
    this->fitInView(this->item, Qt::KeepAspectRatioByExpanding);
}

void DashcamVideoView::showEvent(QShowEvent *event)
{
    QGraphicsView::showEvent(event);
    // Covers becoming visible without a resize - e.g. a rear-display mirror
    // view living in RearDisplay's own QStackedWidget, switched to via
    // setCurrentWidget() rather than ever being resized. Harmless/redundant
    // alongside the front views' other fit triggers (videoAvailableChanged,
    // nativeSizeChanged, content_stack's currentChanged) - fitInView is
    // idempotent given the same size/item.
    this->fitInView(this->item, Qt::KeepAspectRatioByExpanding);
}

const QString DashcamTab::FOOTAGE_DIR = "/home/dash/dashcam-footage";

DashcamTab::DashcamTab(Arbiter &arbiter, QWidget *parent)
    : QWidget(parent)
    , arbiter(arbiter)
{
    this->player = new QMediaPlayer(this);
    this->video_item = new QGraphicsVideoItem();
    this->video_scene = new QGraphicsScene(this);
    this->video_scene->setBackgroundBrush(Qt::black);
    this->video_scene->addItem(this->video_item);
    this->video_widget = new DashcamVideoView(this->video_scene, this->video_item, this);
    this->rear_video_widget = new DashcamVideoView(this->video_scene, this->video_item, nullptr);
    this->player->setVideoOutput(this->video_item);
    this->arbiter.system().rear_display.register_view("Dashcam", this->rear_video_widget);

    connect(this->player, &QMediaPlayer::videoAvailableChanged, this->video_widget, [this](bool) {
        this->video_widget->fitInView(this->video_item, Qt::KeepAspectRatioByExpanding);
    });

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addWidget(this->video_widget, 3);
    layout->addWidget(this->clips_widget(), 2);
    layout->addWidget(this->seek_widget());
    layout->addWidget(this->controls_widget());
}

QWidget *DashcamTab::clips_widget()
{
    QListWidget *clips = new QListWidget(this);
    Session::Forge::to_touch_scroller(clips);
    this->populate_clips(clips);

    connect(clips, &QListWidget::itemClicked, [this](QListWidgetItem *item) {
        QString path = item->data(Qt::UserRole).toString();
        this->player->setMedia(QMediaContent(QUrl::fromLocalFile(path)));
        this->arbiter.system().rear_display.show_view("Dashcam");
        this->player->play();
    });

    return clips;
}

void DashcamTab::populate_clips(QListWidget *clips_widget)
{
    // Dashcam records continuously (see dash-extras/dashcam/) - newest
    // first, since that's almost always what you actually want to review.
    QFileInfoList clips = QDir(DashcamTab::FOOTAGE_DIR).entryInfoList(QStringList() << "dashcam_*.mp4", QDir::Files | QDir::Readable, QDir::Time);
    for (const QFileInfo &clip : clips) {
        QListWidgetItem *item = new QListWidgetItem(clip.fileName(), clips_widget);
        item->setData(Qt::UserRole, QVariant(clip.absoluteFilePath()));
    }
}

QWidget *DashcamTab::seek_widget()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    ClickableSlider *slider = new ClickableSlider(Qt::Orientation::Horizontal, widget);
    slider->setTracking(false);
    slider->setRange(0, 0);
    QLabel *value = new QLabel(LocalPlayerTab::durationFmt(slider->value()), widget);
    connect(slider, &QSlider::sliderReleased,
            [player = this->player, slider]() { player->setPosition(slider->sliderPosition()); });
    connect(slider, &QSlider::sliderMoved,
            [value](int position) { value->setText(LocalPlayerTab::durationFmt(position)); });
    connect(this->player, &QMediaPlayer::durationChanged, [slider](qint64 duration) {
        slider->setValue(0);
        slider->setRange(0, duration);
    });
    connect(this->player, &QMediaPlayer::positionChanged, [slider, value](qint64 position) {
        if (!slider->isSliderDown()) {
            slider->setValue(position);
            value->setText(LocalPlayerTab::durationFmt(position));
        }
    });

    layout->addStretch(4);
    layout->addWidget(slider, 28);
    layout->addWidget(value, 3);
    layout->addStretch(1);

    return widget;
}

QWidget *DashcamTab::controls_widget()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    QPushButton *play_button = new QPushButton(widget);
    play_button->setFlat(true);
    play_button->setCheckable(true);
    play_button->setChecked(false);
    this->arbiter.forge().iconize("play", "pause", play_button, 56);
    connect(play_button, &QPushButton::clicked, [player = this->player, play_button](bool checked = false) {
        play_button->setChecked(!checked);
        if (checked)
            player->play();
        else
            player->pause();
    });
    connect(this->player, &QMediaPlayer::stateChanged,
            [play_button](QMediaPlayer::State state) { play_button->setChecked(state == QMediaPlayer::PlayingState); });
    layout->addWidget(play_button);

    return widget;
}
