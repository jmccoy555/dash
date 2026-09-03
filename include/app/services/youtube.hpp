#pragma once

#include <QList>
#include <QObject>
#include <QSettings>
#include <QString>

class Arbiter;
class QProcess;

// Search + playback for YouTube without the official (ad-supported) player -
// yt-dlp resolves direct media URLs via YouTube's internal API, the same
// mechanism NewPipe uses, so no ads are ever fetched. Modern YouTube no
// longer serves a single pre-muxed stream for most videos though (video and
// audio come back as separate adaptive tracks), so rather than fighting a
// fragile dual-stream GStreamer pipeline, this downloads+remuxes to a local
// file (yt-dlp + ffmpeg, both already used elsewhere on this device) and
// plays that - same "download then play" shape as DabPlugin's WAV dump.
class YouTube : public QObject {
    Q_OBJECT

   public:
    struct Video {
        QString id;
        QString title;
        QString uploader;
        qint64 durationSecs = 0;
    };

    YouTube(Arbiter &arbiter);
    ~YouTube();

    inline bool is_available() { return !this->executable.isEmpty(); }

    void search(QString query);
    void play(QString videoId);  // plays from cache if already downloaded, else downloads first
    void stop();                 // cancels an in-progress download
    QString cached_path(QString videoId) const;

    // Purely local - there's no YouTube account behind this (yt-dlp needs
    // none), so unlike Jellyfin's favourites there's nothing to sync against.
    // Just remembers what was starred, the same file everything else in the
    // app's settings already lives in.
    QList<Video> favorites() const;
    bool is_favorite(QString videoId) const;
    void set_favorite(QString videoId, bool favorite, Video video);

   private:
    void evict_cache_if_needed();

    Arbiter &arbiter;
    QString executable;  // resolved yt-dlp path - empty if it isn't installed
    mutable QSettings settings;

    QProcess *search_process = nullptr;
    QProcess *download_process = nullptr;
    QString downloading_id;

   signals:
    void search_finished(QString query, QList<YouTube::Video> results, QString error);
    void download_progress(QString videoId, int percent);
    void ready_to_play(QString videoId, QString localPath);
    void download_failed(QString videoId, QString error);
    void favorite_toggled(QString videoId, bool favorite);
};
