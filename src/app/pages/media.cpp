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
    this->addTab(new RadioPlayerTab(this->arbiter, this), "Radio");
    this->addTab(new DabPlayerTab(this->arbiter, this), "DAB");
    this->addTab(new BluetoothPlayerTab(this->arbiter, this), "Bluetooth");
    this->addTab(new LocalPlayerTab(this->arbiter, this), "Local");
    this->addTab(new DashcamTab(this->arbiter, this), "Dashcam");
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
    this->player->setVideoOutput(this->video_item);

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
