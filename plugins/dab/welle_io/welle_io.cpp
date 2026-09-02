#include <csignal>

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include "welle_io.hpp"

namespace {

// Band III DAB/DAB+ channels - the fixed set welle-cli accepts via its
// `-c`/`/channel` argument. There's no continuous "frequency" the way FM has
// one; each channel carries a whole ensemble of stations, so a full scan
// means dwelling on every channel long enough for welle-cli to decode
// whatever ensemble is there, rather than tuning to a single number.
const QStringList BAND3_CHANNELS = {
    "5A", "5B", "5C", "5D",
    "6A", "6B", "6C", "6D",
    "7A", "7B", "7C", "7D",
    "8A", "8B", "8C", "8D",
    "9A", "9B", "9C", "9D",
    "10A", "10B", "10C", "10D",
    "11A", "11B", "11C", "11D",
    "12A", "12B", "12C", "12D",
    "13A", "13B", "13C", "13D", "13E", "13F",
};

const quint16 WEB_PORT = 7979;
const int CHANNEL_DWELL_TICKS = 5;      // ~5s per channel while scanning, long enough for FIC decode
const int PLAYBACK_WAIT_TICKS = 25;     // give a freshly (re)tuned playback backend this long to start writing real audio
const qint64 PLAYBACK_READY_BYTES = 400000;  // ~2s of 48kHz/16-bit stereo PCM - enough headroom for ffplay to start smoothly

}

// welle-cli offers two relevant modes here, and they don't mix reliably:
// `-w <port>` runs an HTTP control API (GET /mux.json for the ensemble's
// service list, POST /channel to retune) that this plugin uses for
// scanning - solid and well-tested. `-D` dumps every programme on the
// currently tuned channel straight to a growing WAV file per station.
// Combining both (`-Dw`) was tried first for actual playback (stream a
// station over HTTP via /mp3/<sid>, which welle-cli transcodes to MP3 on
// the fly) but proved unreliable in practice - the embedded HTTP server
// appears to serialize connections, and requests would routinely stall for
// a stalled `-mp3/` stream. Plain `-D` (no `-w`) dumps cleanly and
// immediately. So this plugin switches modes: `-w` while scanning/idle,
// `-D` (killing and restarting the `-w` instance) once a station is
// actually selected - ffplay then just plays the local, steadily-growing
// WAV file directly, sidestepping the HTTP path entirely for real playback.
WelleIo::WelleIo()
    : server()
    , in_playback_mode(false)
    , dump_dir(QDir::tempPath() + "/dash-welle-io")
    , player_pid(0)
    , network()
    , poll_timer()
    , scan_queue(BAND3_CHANNELS)
    , is_scanning(true)
    , dwell_ticks(0)
    , wait_ticks(0)
{
    this->start_discovery();

    connect(&this->poll_timer, &QTimer::timeout, this, &WelleIo::tick);
    this->poll_timer.start(1000);

    this->tune(this->scan_queue.takeFirst());
}

WelleIo::~WelleIo()
{
    this->stop_player();
    this->server.terminate();
    this->server.waitForFinished(1000);
    QDir(this->dump_dir).removeRecursively();
}

QList<DabService> WelleIo::services()
{
    return this->aggregated_services;
}

bool WelleIo::scanning()
{
    return this->is_scanning;
}

void WelleIo::rescan()
{
    this->scan_queue = BAND3_CHANNELS;
    this->aggregated_services.clear();
    this->is_scanning = true;
    this->start_discovery();
    this->tune(this->scan_queue.takeFirst());
}

void WelleIo::play(QString service_id)
{
    // service_id is "<channel>:<sid>" (see tick_discovery()) - a DAB SId is
    // only guaranteed unique within its own ensemble, so two different
    // channels can legitimately reuse the same SId for two unrelated
    // stations. The channel has to travel with the id or a band-wide
    // aggregate list can't tell those apart.
    int sep = service_id.indexOf(':');
    if (sep < 0)
        return;
    QString channel = service_id.left(sep);

    QString label;
    for (const DabService &existing : this->aggregated_services) {
        if (existing.id == service_id) {
            label = existing.label;
            break;
        }
    }
    if (label.isEmpty())
        return;

    this->stop_player();
    this->playing_service_id.clear();
    this->is_scanning = false;

    if (this->in_playback_mode && channel == this->current_channel) {
        // Already dumping this channel's whole ensemble - the target
        // station's WAV file exists already, just point ffplay at it.
        this->pending_service_id.clear();
        this->begin_stream(service_id, label);
    } else {
        this->pending_service_id = service_id;
        this->pending_label = label;
        this->wait_ticks = 0;
        this->start_playback_backend(channel);
    }
}

void WelleIo::stop()
{
    this->stop_player();
    this->playing_service_id.clear();
    this->pending_service_id.clear();
}

void WelleIo::stop_player()
{
    if (this->player_pid > 0)
        ::kill((pid_t)this->player_pid, SIGTERM);
    this->player_pid = 0;
}

QString WelleIo::now_playing()
{
    for (const DabService &service : this->aggregated_services) {
        if (service.id == this->playing_service_id)
            return service.label;
    }

    return QString();
}

void WelleIo::start_discovery()
{
    this->stop_player();
    this->server.terminate();
    this->server.waitForFinished(1000);
    QDir(this->dump_dir).removeRecursively();

    this->in_playback_mode = false;
    this->server.setWorkingDirectory(QDir::tempPath());
    this->server.start("welle-cli", {"-w", QString::number(WEB_PORT), "-F", "rtl_sdr"});
}

void WelleIo::start_playback_backend(QString channel)
{
    this->stop_player();
    this->server.terminate();
    this->server.waitForFinished(1000);

    QDir(this->dump_dir).removeRecursively();
    QDir().mkpath(this->dump_dir);

    // welle-cli writes each programme to "<dump_dir>/<label>.wav" via a
    // plain fopen(), which doesn't create intermediate directories - a
    // label containing a literal "/" (e.g. "SMOOTH JAZZ 24/7") makes that
    // open() fail silently and the station never gets dumped at all. Since
    // that's just a path underneath dump_dir as far as the filesystem is
    // concerned, pre-creating it is enough for welle-cli's own fopen() to
    // succeed.
    for (const DabService &service : this->aggregated_services) {
        if (service.label.contains('/'))
            QDir().mkpath(QFileInfo(this->dump_dir + "/" + service.label + ".wav").absolutePath());
    }

    this->in_playback_mode = true;
    this->current_channel = channel;

    this->server.setWorkingDirectory(this->dump_dir);
    this->server.start("welle-cli", {"-c", channel, "-D", "-F", "rtl_sdr"});
}

void WelleIo::begin_stream(QString service_id, QString label)
{
    this->playing_service_id = service_id;
    this->pending_service_id.clear();

    QString path = this->dump_dir + "/" + label + ".wav";
    QProcess::startDetached("ffplay", {"-nodisp", "-autoexit", path}, QString(), &this->player_pid);
}

void WelleIo::tune(QString channel)
{
    this->current_channel = channel;
    this->dwell_ticks = 0;

    QNetworkRequest request(QUrl(QString("http://localhost:%1/channel").arg(WEB_PORT)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "text/plain");
    this->network.post(request, channel.toUtf8());
}

void WelleIo::tick()
{
    if (this->in_playback_mode)
        this->tick_playback();
    else
        this->tick_discovery();
}

void WelleIo::tick_playback()
{
    if (this->pending_service_id.isEmpty())
        return;

    this->wait_ticks++;

    QFileInfo info(this->dump_dir + "/" + this->pending_label + ".wav");
    if (info.exists() && info.size() >= PLAYBACK_READY_BYTES) {
        this->begin_stream(this->pending_service_id, this->pending_label);
    } else if (this->wait_ticks >= PLAYBACK_WAIT_TICKS) {
        // Never got there - give up rather than block forever. The station
        // stays selectable; tapping it again starts a fresh attempt.
        this->pending_service_id.clear();
    }
}

void WelleIo::tick_discovery()
{
    if (!this->is_scanning)
        return;

    this->dwell_ticks++;

    // Captured now rather than read from this->current_channel inside the
    // completion lambda below - a channel advance (further down in this same
    // tick()) can land before this reply comes back, and the reply always
    // describes whatever channel was tuned when it was sent, not whatever's
    // tuned by the time it arrives.
    QString polled_channel = this->current_channel;

    QNetworkRequest request(QUrl(QString("http://localhost:%1/mux.json").arg(WEB_PORT)));
    QNetworkReply *reply = this->network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, polled_channel] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return;

        QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        QJsonArray services_arr = root["services"].toArray();

        for (const QJsonValue &value : services_arr) {
            QJsonObject service = value.toObject();
            // sid comes back as a hex string (e.g. "0xc787"), not a JSON
            // number, and is only unique within this channel's ensemble -
            // prefix it with the channel to get a key that's actually
            // unique across a full-band aggregate.
            QString sid = service["sid"].toString();
            QString label = service["label"].toObject()["label"].toString().trimmed();
            if (sid.isEmpty() || label.isEmpty())
                continue;

            QString id = polled_channel + ":" + sid;

            bool known = false;
            for (DabService &existing : this->aggregated_services) {
                if (existing.id == id) {
                    existing.label = label;
                    known = true;
                    break;
                }
                // Small-scale multiplexes often simulcast the same
                // community station on more than one channel - once a name
                // has been found, later sightings of it elsewhere update
                // nothing and don't add a second row; the first channel
                // found keeps ownership of that name for play().
                if (existing.label == label) {
                    known = true;
                    break;
                }
            }
            if (!known)
                this->aggregated_services.append({id, label});
        }
    });

    if (this->dwell_ticks >= CHANNEL_DWELL_TICKS) {
        if (this->scan_queue.isEmpty())
            this->is_scanning = false;
        else
            this->tune(this->scan_queue.takeFirst());
    }
}
