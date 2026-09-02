#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>

#include "plugins/dab_plugin.hpp"

class WelleIo : public QObject, DabPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID DabPlugin_iid FILE "welle_io.json")
    Q_INTERFACES(DabPlugin)

   public:
    WelleIo();
    ~WelleIo();

    QList<DabService> services() override;
    bool scanning() override;
    void rescan() override;
    void play(QString service_id) override;
    void stop() override;
    QString now_playing() override;

   private:
    // welle-cli, run in one of two mutually-exclusive modes against the one
    // physical tuner: discovery (-w, HTTP control API) while scanning/idle,
    // playback (-D, dumps every programme on the tuned channel straight to
    // WAV) once a station is actually selected.
    QProcess server;
    bool in_playback_mode;
    QString dump_dir;

    qint64 player_pid;
    QNetworkAccessManager network;
    QTimer poll_timer;

    QStringList scan_queue;
    bool is_scanning;
    int dwell_ticks;

    QString current_channel;
    QString playing_service_id;
    QString pending_service_id;
    QString pending_label;
    int wait_ticks;

    QList<DabService> aggregated_services;

    void stop_player();
    void start_discovery();
    void start_playback_backend(QString channel);
    void begin_stream(QString service_id, QString label);

    void tune(QString channel);
    void tick();
    void tick_discovery();
    void tick_playback();
};
