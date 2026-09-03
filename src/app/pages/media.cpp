#include <fileref.h>
#include <math.h>
#include <tag.h>
#include <tpropertymap.h>
#include <BluezQt/PendingCall>
#include <QDirIterator>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMediaPlaylist>
#include <QMouseEvent>
#include <QStyle>

#include "app/window.hpp"
#include "app/pages/media.hpp"
#include "plugins/dab_plugin.hpp"
#include "plugins/radio_plugin.hpp"

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
    auto add_tab = [this](QWidget *widget, QString name) {
        this->addTab(widget, name);
        this->tabs.append(widget);
    };

    add_tab(new RadioPlayerTab(this->arbiter, this), "Radio");
    add_tab(new DabPlayerTab(this->arbiter, this), "DAB");
    add_tab(new BluetoothPlayerTab(this->arbiter, this), "Bluetooth");
    add_tab(new LocalPlayerTab(this->arbiter, this), "Local");
    add_tab(new JellyfinTab(this->arbiter, this), "Jellyfin");
    add_tab(new YouTubeTab(this->arbiter, this), "DashTube");
    add_tab(new DashcamTab(this->arbiter, this), "Dashcam");

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
            this->setTabVisible(index, !disabled.contains(this->tabText(index)));
    }
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
    BluezQt::MediaPlayerPtr media_player = this->arbiter.system().bluetooth.get_media_player().second;
    AAHandler *aa_handler = this->arbiter.android_auto().handler;

    QWidget *widget = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(widget);

    QLabel *artist_hdr = new QLabel("Artist", widget);
    QLabel *artist = new QLabel((media_player != nullptr) ? media_player->track().artist() : QString(), widget);
    artist->setIndent(16);
    layout->addWidget(artist_hdr);
    layout->addWidget(artist);

    QLabel *album_hdr = new QLabel("Album", widget);
    QLabel *album = new QLabel((media_player != nullptr) ? media_player->track().album() : QString(), widget);
    album->setIndent(16);
    layout->addWidget(album_hdr);
    layout->addWidget(album);

    QLabel *title_hdr = new QLabel("Title", widget);
    QLabel *title = new QLabel((media_player != nullptr) ? media_player->track().title() : QString(), widget);
    title->setIndent(16);
    layout->addWidget(title_hdr);
    layout->addWidget(title);

    QLabel *albumArt = new QLabel(widget);
    layout->addWidget(albumArt);

    connect(&this->arbiter.system().bluetooth, &Bluetooth::media_player_track_changed, [artist, album, title](BluezQt::MediaPlayerTrack track){
        artist->setText(track.artist());
        album->setText(track.album());
        title->setText(track.title());
    });
    connect(aa_handler, &AAHandler::aa_media_metadata_update, [artist, album, title, albumArt](const aasdk::proto::messages::MediaInfoChannelMetadataData& metadata){
        title->setText(QString::fromStdString(metadata.track_name()));
        if(metadata.has_artist_name()) artist->setText(QString::fromStdString(metadata.artist_name()));
        if(metadata.has_album_name()) album->setText(QString::fromStdString(metadata.album_name()));
        if(metadata.has_album_art()){
            QImage art;
            art.loadFromData(QByteArray::fromStdString(metadata.album_art()));
            albumArt->setPixmap(QPixmap::fromImage(art));
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
    , services_widget(new QListWidget(this))
    , stop_button(new QPushButton("stop", this))
{
    this->status_label->setAlignment(Qt::AlignCenter);
    this->now_playing_label->setAlignment(Qt::AlignCenter);
    Session::Forge::to_touch_scroller(this->services_widget);

    connect(this->stop_button, &QPushButton::clicked, [this]{
        if (DabPlugin *plugin = qobject_cast<DabPlugin *>(this->loader.instance()))
            plugin->stop();
    });

    connect(this->services_widget, &QListWidget::itemClicked, [this](QListWidgetItem *item){
        if (DabPlugin *plugin = qobject_cast<DabPlugin *>(this->loader.instance()))
            plugin->play(item->data(Qt::UserRole).toString());
    });

    // There's no manual tuning here - channel is a DAB implementation detail
    // the plugin resolves for itself (see welle_io), and the station list
    // fills in as the scan finds things, so this just polls the plugin's
    // already-synchronous getters to reflect whatever it currently knows.
    connect(this->poll_timer, &QTimer::timeout, [this]{ this->refresh(); });
    this->poll_timer->start(1000);

    auto layout = new QVBoxLayout(this);
    layout->addWidget(this->header_widget());
    layout->addWidget(this->status_label);
    layout->addWidget(this->now_playing_label);
    layout->addWidget(this->services_widget, 1);
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

    this->services_widget->clear();
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

    QString selected_id;
    if (QListWidgetItem *current = this->services_widget->currentItem())
        selected_id = current->data(Qt::UserRole).toString();

    this->services_widget->clear();
    for (const DabService &service : plugin->services()) {
        QListWidgetItem *item = new QListWidgetItem(service.label, this->services_widget);
        item->setData(Qt::UserRole, service.id);
        if (service.id == selected_id)
            item->setSelected(true);
    }

    QString now_playing = plugin->now_playing();
    this->now_playing_label->setText(now_playing.isEmpty() ? QString() : QString("Now playing: %1").arg(now_playing));
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
{
    this->config = Config::get_instance();

    QMediaPlaylist *playlist = new QMediaPlaylist(this);
    playlist->setPlaybackMode(QMediaPlaylist::Loop);

    this->player = new QMediaPlayer(this);
    this->player->setPlaylist(playlist);

    // Registers with AAHandler so AA starting playback can pause this (and
    // this starting playback can pause AA) - see controls_widget() and
    // AAHandler::mediaPlaybackUpdate. Physical media buttons also check
    // active_media_source there to decide whether to route to AA or here.
    this->arbiter.android_auto().handler->local_player = this->player;

    this->path_label = new QLabel(this->config->get_media_home(), this);

    QVBoxLayout *layout = new QVBoxLayout(this);

    layout->addWidget(this->path_label);
    layout->addWidget(this->playlist_widget());
    layout->addWidget(this->seek_widget());
    layout->addWidget(this->controls_widget());
}

QWidget *LocalPlayerTab::playlist_widget()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    QString root_path(config->get_media_home());

    QPushButton *home_button = new QPushButton(widget);
    home_button->setFlat(true);
    home_button->setCheckable(true);
    home_button->setChecked(this->config->get_media_home() == root_path);
    this->arbiter.forge().iconize("playlist_add", "playlist_add_check", home_button, 32);
    connect(home_button, &QPushButton::clicked, [this](bool checked = false) {
        this->config->set_media_home(checked ? this->path_label->text() : QDir().absolutePath());
    });
    layout->addWidget(home_button, 0, Qt::AlignTop);

    QListWidget *folders = new QListWidget(widget);
    Session::Forge::to_touch_scroller(folders);
    this->populate_dirs(root_path, folders);
    layout->addWidget(folders, 1);

    QListWidget *tracks = new QListWidget(widget);
    Session::Forge::to_touch_scroller(tracks);
    this->populate_tracks(root_path, tracks);
    connect(tracks, &QListWidget::itemClicked, [tracks, player = this->player](QListWidgetItem *item) {
        // Only touches the active playlist here, when a track is actually
        // chosen to play - browsing folders (below) no longer clears or
        // rebuilds it, so navigating to another folder to see what's in it
        // doesn't stop whatever's currently playing (see conversation).
        // Rebuilt from what's currently displayed so next/prev still walk
        // through this folder's tracks in order.
        player->playlist()->clear();
        for (int i = 0; i < tracks->count(); ++i)
            player->playlist()->addMedia(QMediaContent(QUrl::fromLocalFile(tracks->item(i)->data(Qt::UserRole).toString())));
        player->playlist()->setCurrentIndex(tracks->row(item));
        player->play();
    });
    connect(this->player->playlist(), &QMediaPlaylist::currentIndexChanged, [tracks](int idx) {
        if (idx < 0) return;
        tracks->setCurrentRow(idx);
    });
    connect(folders, &QListWidget::itemClicked, [this, folders, tracks, home_button](QListWidgetItem *item) {
        if (!item->isSelected()) return;

        QString current_path(item->data(Qt::UserRole).toString());
        this->path_label->setText(current_path);
        this->populate_tracks(current_path, tracks);
        this->populate_dirs(current_path, folders);

        home_button->setChecked(this->config->get_media_home() == current_path);
    });
    layout->addWidget(tracks, 2);

    return widget;
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

    return widget;
}

QString LocalPlayerTab::durationFmt(int total_ms)
{
    int mins = (total_ms / (1000 * 60)) % 60;
    int secs = (total_ms / 1000) % 60;

    return QString("%1:%2").arg(mins, 2, 10, QChar('0')).arg(secs, 2, 10, QChar('0'));
}

void LocalPlayerTab::populate_dirs(QString path, QListWidget *dirs_widget)
{
    dirs_widget->clear();
    QDir current_dir(path);
    QFileInfoList dirs = current_dir.entryInfoList(QDir::AllDirs | QDir::Readable);
    for (QFileInfo dir : dirs) {
        if (dir.fileName() == ".") continue;

        QListWidgetItem *item = new QListWidgetItem(dir.fileName(), dirs_widget);
        if (dir.fileName() == "..") {
            item->setText("↲");

            if (current_dir.isRoot()) item->setFlags(Qt::NoItemFlags);
        }
        else {
            item->setText(dir.fileName());
        }
        item->setData(Qt::UserRole, QVariant(dir.absoluteFilePath()));
    }
}

void LocalPlayerTab::populate_tracks(QString path, QListWidget *tracks_widget)
{
    // Just populates the browseable list - doesn't touch the active
    // playlist (see the tracks itemClicked handler in playlist_widget(),
    // which is the only place that now does).
    tracks_widget->clear();
    QStringList tracks = QDir(path).entryList(QStringList() << "*.flac" << "*.m4a" << "*.mp3", QDir::Files | QDir::Readable);
    for (QString track : tracks) {
        int lastPoint = track.lastIndexOf(".");
        QString fileNameNoExt = track.left(lastPoint);
        QListWidgetItem *item = new QListWidgetItem(fileNameNoExt, tracks_widget);
        item->setData(Qt::UserRole, QVariant(path + '/' + track));
    }
}

JellyfinTab::JellyfinTab(Arbiter &arbiter, QWidget *parent)
    : QWidget(parent)
    , arbiter(arbiter)
    , config(Config::get_instance())
    , player(new QMediaPlayer(this))
    , content_stack(new QStackedWidget(this))
    , browser_widget(new QListWidget(this))
    , video_scene(new QGraphicsScene(this))
    , video_item(new QGraphicsVideoItem())
    , video_widget(new DashcamVideoView(this->video_scene, this->video_item, this))
    , rear_video_widget(new DashcamVideoView(this->video_scene, this->video_item, nullptr))
    , breadcrumb_label(new QLabel("Jellyfin", this))
    , status_label(new QLabel(this))
    , username_input(nullptr)
    , password_input(nullptr)
{
    Session::Forge::to_touch_scroller(this->browser_widget);
    this->status_label->setAlignment(Qt::AlignCenter);

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

    this->content_stack->addWidget(this->browser_widget);
    this->content_stack->addWidget(this->video_widget);
    this->arbiter.system().rear_display.register_view("Jellyfin", this->rear_video_widget);

    QMediaPlaylist *playlist = new QMediaPlaylist(this->player);
    playlist->setPlaybackMode(QMediaPlaylist::Loop);
    this->player->setPlaylist(playlist);

    connect(this->browser_widget, &QListWidget::itemClicked, [this](QListWidgetItem *list_item) {
        int index = list_item->data(Qt::UserRole).toInt();
        if (index == -1) {
            this->navigate(QString(), QString(), false);
            return;
        }
        if (index < 0 || index >= this->current_items.size())
            return;

        const Jellyfin::Item &item = this->current_items[index];
        if (item.type == Jellyfin::ItemType::Container)
            this->navigate(item.id, item.name, true);
        else
            this->play_from(index);
    });

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
    this->current_items = items;
    this->browser_widget->clear();

    if (!this->nav_stack.isEmpty()) {
        QListWidgetItem *up = new QListWidgetItem("↲", this->browser_widget);
        up->setData(Qt::UserRole, -1);
    }

    for (int i = 0; i < items.size(); i++) {
        const Jellyfin::Item &item = items[i];

        if (item.type == Jellyfin::ItemType::Container) {
            QListWidgetItem *list_item = new QListWidgetItem(item.name, this->browser_widget);
            list_item->setData(Qt::UserRole, i);
            continue;
        }

        // Audio/Video rows get a favourite-toggle star alongside the title -
        // a separate child widget so tapping it doesn't also trigger the
        // row's own itemClicked (which starts playback).
        QWidget *row = new QWidget(this->browser_widget);
        QHBoxLayout *row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(8, 4, 8, 4);

        QLabel *title = new QLabel(item.name, row);
        row_layout->addWidget(title, 1);

        QPushButton *star = new QPushButton(row);
        star->setFlat(true);
        star->setCheckable(true);
        star->setChecked(item.isFavorite);
        this->arbiter.forge().iconize("favorite_border", "favorite", star, 20);
        QString item_id = item.id;
        connect(star, &QPushButton::clicked, [this, item_id](bool checked) {
            this->arbiter.system().jellyfin.toggle_favorite(item_id, checked);
        });
        row_layout->addWidget(star);

        QListWidgetItem *list_item = new QListWidgetItem(this->browser_widget);
        list_item->setData(Qt::UserRole, i);
        list_item->setSizeHint(row->sizeHint());
        this->browser_widget->setItemWidget(list_item, row);
    }
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

    this->content_stack->setCurrentWidget(target.type == Jellyfin::ItemType::Video ? (QWidget *)this->video_widget : (QWidget *)this->browser_widget);
    if (target.type == Jellyfin::ItemType::Video)
        this->arbiter.system().rear_display.show_view("Jellyfin");
    this->player->play();
}

QWidget *JellyfinTab::header_widget()
{
    QWidget *widget = new QWidget(this);
    QHBoxLayout *layout = new QHBoxLayout(widget);

    Dialog *dialog = new Dialog(this->arbiter, true, this->window());
    dialog->set_body(this->settings_dialog_body());

    QPushButton *settings_button = new QPushButton(widget);
    settings_button->setFlat(true);
    this->arbiter.forge().iconize("settings", settings_button, 24);
    connect(settings_button, &QPushButton::clicked, [dialog] { dialog->open(); });

    this->breadcrumb_label->setAlignment(Qt::AlignCenter);

    // Only relevant once a video is playing (browsing is otherwise always
    // showing already) - pauses playback and swaps back to the list rather
    // than leaving a movie running behind a view the user can't see.
    QPushButton *back_button = new QPushButton(widget);
    back_button->setFlat(true);
    this->arbiter.forge().iconize("arrow_left", back_button, 24);
    back_button->hide();
    connect(back_button, &QPushButton::clicked, [this] {
        this->player->pause();
        this->content_stack->setCurrentWidget(this->browser_widget);
    });
    connect(this->content_stack, &QStackedWidget::currentChanged, [this, back_button](int index) {
        back_button->setVisible(this->content_stack->widget(index) == this->video_widget);
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

QWidget *JellyfinTab::settings_dialog_body()
{
    QWidget *widget = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(widget);

    QLineEdit *server_input = new QLineEdit(this->config->get_jellyfin_server_url(), widget);
    server_input->setContextMenuPolicy(Qt::NoContextMenu);
    server_input->setFont(this->arbiter.forge().font(16));
    server_input->setAlignment(Qt::AlignCenter);
    server_input->setPlaceholderText("Server URL");
    connect(server_input, &QLineEdit::textEdited, [this](QString text) { this->config->set_jellyfin_server_url(text); });
    layout->addWidget(server_input);

    this->username_input = new QLineEdit(widget);
    this->username_input->setContextMenuPolicy(Qt::NoContextMenu);
    this->username_input->setFont(this->arbiter.forge().font(16));
    this->username_input->setAlignment(Qt::AlignCenter);
    this->username_input->setPlaceholderText("Username");
    layout->addWidget(this->username_input);

    this->password_input = new QLineEdit(widget);
    this->password_input->setContextMenuPolicy(Qt::NoContextMenu);
    this->password_input->setFont(this->arbiter.forge().font(16));
    this->password_input->setAlignment(Qt::AlignCenter);
    this->password_input->setEchoMode(QLineEdit::Password);
    this->password_input->setPlaceholderText("Password");
    layout->addWidget(this->password_input);

    QLabel *login_status = new QLabel(widget);
    login_status->setAlignment(Qt::AlignCenter);
    layout->addWidget(login_status);

    QPushButton *login_button = new QPushButton("Log in", widget);
    connect(login_button, &QPushButton::clicked, [this, login_status] {
        login_status->setText("Logging in…");
        this->arbiter.system().jellyfin.authenticate(this->config->get_jellyfin_server_url(), this->username_input->text(), this->password_input->text());
    });
    layout->addWidget(login_button);

    connect(&this->arbiter.system().jellyfin, &Jellyfin::auth_finished, this, [this, login_status](bool success, QString error) {
        login_status->setText(success ? "Logged in" : error);
        if (success)
            this->navigate(QString(), QString(), false);
    });

    layout->addWidget(Session::Forge::br());

    QPushButton *sync_button = new QPushButton("Sync favourites now", widget);
    connect(sync_button, &QPushButton::clicked, [this, sync_button] {
        sync_button->setEnabled(false);
        this->arbiter.system().jellyfin.sync_favorites();
    });
    connect(&this->arbiter.system().jellyfin, &Jellyfin::sync_finished, this, [sync_button](int, int) {
        sync_button->setEnabled(true);
    });
    layout->addWidget(sync_button);

    QLabel *sync_note = new QLabel("Favourites also sync automatically every 30 minutes", widget);
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

    return widget;
}

YouTubeTab::YouTubeTab(Arbiter &arbiter, QWidget *parent)
    : QWidget(parent)
    , arbiter(arbiter)
    , player(new QMediaPlayer(this))
    , content_stack(new QStackedWidget(this))
    , search_input(nullptr)
    , results_widget(new QListWidget(this))
    , video_scene(new QGraphicsScene(this))
    , video_item(new QGraphicsVideoItem())
    , video_widget(new DashcamVideoView(this->video_scene, this->video_item, this))
    , rear_video_widget(new DashcamVideoView(this->video_scene, this->video_item, nullptr))
    , status_label(new QLabel(this))
{
    Session::Forge::to_touch_scroller(this->results_widget);
    this->status_label->setAlignment(Qt::AlignCenter);

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

    this->content_stack->addWidget(this->results_widget);
    this->content_stack->addWidget(this->video_widget);
    this->arbiter.system().rear_display.register_view("DashTube", this->rear_video_widget);

    connect(this->results_widget, &QListWidget::itemClicked, [this](QListWidgetItem *item) {
        this->play_from(item->data(Qt::UserRole).toInt());
    });

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
    this->results_widget->clear();

    for (int i = 0; i < results.size(); i++) {
        const YouTube::Video &video = results[i];

        QString text = video.uploader.isEmpty() ? video.title : QString("%1 — %2").arg(video.title, video.uploader);
        if (video.durationSecs > 0)
            text += QString("  (%1)").arg(LocalPlayerTab::durationFmt(video.durationSecs * 1000));

        // A separate child widget for the star, same as JellyfinTab's rows -
        // keeps the tap-to-favourite from also triggering the row's own
        // itemClicked (which starts playback).
        QWidget *row = new QWidget(this->results_widget);
        QHBoxLayout *row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(8, 4, 8, 4);

        QLabel *title = new QLabel(text, row);
        row_layout->addWidget(title, 1);

        QPushButton *star = new QPushButton(row);
        star->setFlat(true);
        star->setCheckable(true);
        star->setChecked(this->arbiter.system().youtube.is_favorite(video.id));
        this->arbiter.forge().iconize("favorite_border", "favorite", star, 20);
        YouTube::Video video_copy = video;
        connect(star, &QPushButton::clicked, [this, video_copy](bool checked) {
            this->arbiter.system().youtube.set_favorite(video_copy.id, checked, video_copy);
        });
        row_layout->addWidget(star);

        QListWidgetItem *item = new QListWidgetItem(this->results_widget);
        item->setData(Qt::UserRole, i);
        item->setSizeHint(row->sizeHint());
        this->results_widget->setItemWidget(item, row);
    }
}

void YouTubeTab::play_from(int index)
{
    if (index < 0 || index >= this->current_results.size())
        return;

    this->now_playing_index = index;
    this->status_label->setText("Downloading…");
    this->arbiter.system().youtube.play(this->current_results[index].id);
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
        this->content_stack->setCurrentWidget(this->results_widget);
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
