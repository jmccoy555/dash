#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfoList>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include "DashLog.hpp"

#include "app/config.hpp"
#include "app/services/youtube.hpp"

namespace {
const qint64 CACHE_HIGH_WATER = 2LL * 1024 * 1024 * 1024;  // start evicting once the cache exceeds this...
const qint64 CACHE_LOW_WATER = 1LL * 1024 * 1024 * 1024;   // ...and delete oldest files until back under this
}  // namespace

YouTube::YouTube(Arbiter &arbiter)
    : QObject(qApp)
    , arbiter(arbiter)
    , settings()
{
    // Installed into ~/.local/bin via `pip install --user` rather than a
    // system package - dash.service's own PATH (systemd's default, not this
    // user's login shell PATH) wouldn't otherwise see it.
    this->executable = QStandardPaths::findExecutable("yt-dlp", {QDir::homePath() + "/.local/bin"});
    if (this->executable.isEmpty())
        DASH_LOG(warning) << "[YouTube] yt-dlp not found - tab will be unusable until it's installed";
}

YouTube::~YouTube()
{
    if (this->download_process)
        this->download_process->kill();
}

void YouTube::search(QString query)
{
    if (!this->is_available()) {
        emit search_finished(query, {}, "yt-dlp not installed");
        return;
    }

    if (this->search_process) {
        this->search_process->kill();
        this->search_process->deleteLater();
    }

    this->search_process = new QProcess(this);
    connect(this->search_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, query](int code, QProcess::ExitStatus) {
                QProcess *process = this->search_process;
                this->search_process = nullptr;

                if (code != 0) {
                    QString error = QString::fromUtf8(process->readAllStandardError()).trimmed();
                    process->deleteLater();
                    emit search_finished(query, {}, error.isEmpty() ? "Search failed" : error);
                    return;
                }

                QList<Video> results;
                for (const QByteArray &line : process->readAllStandardOutput().split('\n')) {
                    if (line.trimmed().isEmpty())
                        continue;
                    QJsonObject object = QJsonDocument::fromJson(line).object();
                    Video video;
                    video.id = object["id"].toString();
                    video.title = object["title"].toString();
                    video.uploader = object["uploader"].toString();
                    video.durationSecs = object["duration"].toVariant().toLongLong();
                    if (!video.id.isEmpty())
                        results.append(video);
                }

                process->deleteLater();
                emit search_finished(query, results, QString());
            });

    // flat-playlist skips resolving full metadata (and every format) for
    // each of the 20 results, which is what actually makes a search slow -
    // only play() below needs a video's real formats, and only for the one
    // video actually picked.
    this->search_process->start(this->executable,
        {"ytsearch20:" + query, "--flat-playlist", "--dump-json", "--no-warnings"});
}

QString YouTube::cached_path(QString videoId) const
{
    QDir dir(Config::get_instance()->get_youtube_cache_dir());
    QStringList matches = dir.entryList(QStringList() << (videoId + ".*"), QDir::Files);
    if (matches.isEmpty())
        return QString();
    return dir.absoluteFilePath(matches.first());
}

QList<YouTube::Video> YouTube::favorites() const
{
    QList<Video> results;
    int size = this->settings.beginReadArray("Pages/Media/YouTube/favorites");
    for (int i = 0; i < size; i++) {
        this->settings.setArrayIndex(i);
        Video video;
        video.id = this->settings.value("id").toString();
        video.title = this->settings.value("title").toString();
        video.uploader = this->settings.value("uploader").toString();
        video.durationSecs = this->settings.value("duration").toLongLong();
        if (!video.id.isEmpty())
            results.append(video);
    }
    this->settings.endArray();
    return results;
}

bool YouTube::is_favorite(QString videoId) const
{
    for (const Video &video : this->favorites()) {
        if (video.id == videoId)
            return true;
    }
    return false;
}

void YouTube::set_favorite(QString videoId, bool favorite, Video video)
{
    QList<Video> current = this->favorites();

    bool exists = false;
    for (int i = 0; i < current.size(); i++) {
        if (current[i].id == videoId) {
            exists = true;
            if (!favorite)
                current.removeAt(i);
            break;
        }
    }
    if (favorite && !exists) {
        video.id = videoId;
        current.append(video);
    }

    this->settings.beginWriteArray("Pages/Media/YouTube/favorites");
    for (int i = 0; i < current.size(); i++) {
        this->settings.setArrayIndex(i);
        this->settings.setValue("id", current[i].id);
        this->settings.setValue("title", current[i].title);
        this->settings.setValue("uploader", current[i].uploader);
        this->settings.setValue("duration", current[i].durationSecs);
    }
    this->settings.endArray();

    emit favorite_toggled(videoId, favorite);
}

void YouTube::play(QString videoId)
{
    QString cached = this->cached_path(videoId);
    if (!cached.isEmpty()) {
        emit ready_to_play(videoId, cached);
        return;
    }

    if (!this->is_available()) {
        emit download_failed(videoId, "yt-dlp not installed");
        return;
    }

    if (this->download_process) {
        this->download_process->kill();
        this->download_process->deleteLater();
        this->download_process = nullptr;
    }

    this->evict_cache_if_needed();

    QString cache_dir = Config::get_instance()->get_youtube_cache_dir();
    QDir().mkpath(cache_dir);

    this->downloading_id = videoId;

    this->download_process = new QProcess(this);
    this->download_process->setProcessChannelMode(QProcess::MergedChannels);

    connect(this->download_process, &QProcess::readyRead, this, [this, videoId] {
        static const QRegularExpression percent_pattern("(\\d+(?:\\.\\d+)?)%");
        for (const QByteArray &line : this->download_process->readAll().split('\r')) {
            QRegularExpressionMatch match = percent_pattern.match(line);
            if (match.hasMatch())
                emit download_progress(videoId, (int)match.captured(1).toDouble());
        }
    });

    connect(this->download_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, videoId](int code, QProcess::ExitStatus) {
                this->download_process->deleteLater();
                this->download_process = nullptr;
                this->downloading_id.clear();

                QString path = this->cached_path(videoId);
                if (code == 0 && !path.isEmpty()) {
                    emit ready_to_play(videoId, path);
                } else {
                    DASH_LOG(warning) << "[YouTube] download failed for " << videoId.toStdString();
                    emit download_failed(videoId, "Download failed");
                }
            });

    // Filenames keyed on the video id, never the title - a YouTube title can
    // contain '/' or other characters that aren't safe in a flat filename
    // (the exact issue tonight's DAB work hit with a station label).
    // Prefers avc1/mp4a (H.264 + AAC) over whatever else is offered - this
    // device's hardware decoder is confirmed working for H.264; AV1/VP9
    // would be an untested software-decode path on an embedded ARM board.
    // The merge below is a plain stream copy (same codecs in a new
    // container), not a re-encode, so it's fast regardless of video length.
    this->download_process->start(this->executable,
        {videoId,
         "-f", "bv*[vcodec^=avc1][height<=480]+ba[acodec^=mp4a]/bv*[height<=480]+ba/b[height<=480]",
         "--merge-output-format", "mp4",
         "--newline",
         "--no-warnings",
         "-o", cache_dir + "/" + videoId + ".%(ext)s"});
}

void YouTube::stop()
{
    if (this->download_process) {
        this->download_process->kill();
        this->download_process->deleteLater();
        this->download_process = nullptr;
    }
    this->downloading_id.clear();
}

void YouTube::evict_cache_if_needed()
{
    QDir dir(Config::get_instance()->get_youtube_cache_dir());
    QFileInfoList files = dir.entryInfoList(QDir::Files, QDir::Time | QDir::Reversed);  // oldest first

    qint64 total = 0;
    for (const QFileInfo &file : files)
        total += file.size();

    if (total <= CACHE_HIGH_WATER)
        return;

    // This is just a bounded download cache (re-fetchable any time), not a
    // deliberate offline library like Jellyfin's sync - nothing here needs
    // asking, just needs to not grow forever from casual browsing.
    for (const QFileInfo &file : files) {
        if (total <= CACHE_LOW_WATER)
            break;
        qint64 size = file.size();
        if (QFile::remove(file.absoluteFilePath()))
            total -= size;
    }
}
