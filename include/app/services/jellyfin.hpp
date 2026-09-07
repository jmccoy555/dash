#pragma once

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPair>
#include <QString>
#include <QTimer>
#include <QUrl>

class Arbiter;
class QNetworkReply;

class Jellyfin : public QObject {
    Q_OBJECT

   public:
    // Container = Library/Series/Season/Artist/Album/Playlist/Folder - all navigate the same way.
    // Audio/Video are the two playable leaf kinds - Movie and Episode both classify as Video.
    enum class ItemType { Container, Audio, Video };

    struct Item {
        QString id;
        QString name;
        QString parentId;
        ItemType type = ItemType::Container;
        bool isFavorite = false;
        qint64 runtimeMs = 0;
        qint64 sizeBytes = 0;  // only populated where the query asked for Fields=Size - used for the sync free-space check
        int indexNumber = -1;
    };

    Jellyfin(Arbiter &arbiter);

    inline bool is_authenticated() { return !this->access_token.isEmpty(); }

    // username/password are only ever used for this one call, never persisted -
    // only the resulting token/user id are.
    void authenticate(QString server_url, QString username, QString password);
    void browse(QString parentId);  // parentId empty => synthetic root: "Favourites" node + Views
    void toggle_favorite(QString itemId, bool favorite);
    void sync_favorites();

    QUrl stream_url(QString itemId, ItemType type) const;
    QUrl image_url(QString itemId) const;  // Primary image - empty/404 if the item has none, callers should just leave their placeholder in that case
    QString cached_path(QString itemId) const;  // "" if this item hasn't been synced locally

    // Every currently-synced Audio item, as (absolute file path, display
    // name) pairs - lets LocalPlayerTab fold this straight into its own
    // browsing/search so synced tracks show up as themselves instead of
    // living behind a separate "jellyfin" folder full of id-named files
    // no one taps into (see conversation).
    QList<QPair<QString, QString>> offline_audio_tracks() const;

   private:
    QString auth_header(bool with_token) const;
    Item parse_item(QJsonObject object) const;
    bool check_auth_error(QNetworkReply *reply);  // clears token + emits auth_required() on a 401, returns true if it did
    void download_next();
    void schedule_auto_sync();  // starts (or restarts) periodic favourites sync - called after login and, if already logged in, once at startup
    QNetworkAccessManager *net();  // lazily constructed - see constructor comment

    // Downloaded files are named <itemId>.<ext> (see download_next() - a
    // title/artist isn't filesystem-safe and isn't stable across renames),
    // so the real name/type has to be remembered separately for anything
    // that wants to display them as themselves - this is that record,
    // persisted alongside the downloads themselves so it survives restarts.
    QString offline_index_path() const;
    void load_offline_index();
    void save_offline_index() const;
    QJsonObject offline_index;  // itemId -> {"name": ..., "type": "Audio"|"Video"}

    Arbiter &arbiter;
    QNetworkAccessManager *network = nullptr;

    QString access_token;
    QString user_id;
    QString device_id;

    QMap<QString, QList<Item>> browse_cache;  // keyed on parentId ("" = root) - see browse()
    QTimer *sync_timer = nullptr;             // periodic auto-sync - see schedule_auto_sync()

    bool syncing = false;
    QList<Item> sync_queue;
    int sync_total = 0;
    int sync_done = 0;
    int sync_failed = 0;

   signals:
    void auth_finished(bool success, QString error);
    void auth_required();  // token was cleared after a 401 - UI should prompt for fresh credentials
    void items_ready(QString parentId, QList<Jellyfin::Item> items);
    void favorite_toggled(QString itemId, bool favorite);
    void sync_progress(int done, int total, QString current_name);
    void sync_finished(int downloaded, int failed);
};
