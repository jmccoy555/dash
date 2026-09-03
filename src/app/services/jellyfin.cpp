#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStorageInfo>
#include <QUrlQuery>
#include <QUuid>

#include "DashLog.hpp"

#include "app/config.hpp"
#include "app/services/jellyfin.hpp"

// Jellyfin's REST API (Emby-compatible). All authenticated calls carry an
// `Authorization: MediaBrowser Client="...", Device="...", DeviceId="...",
// Version="...", Token="..."` header (the older X-Emby-Authorization header
// is deprecated and can be disabled server-side on newer Jellyfin
// versions). QMediaPlayer can't set custom headers though, so the one
// exception is the streaming URL handed to it directly - that uses the
// `api_key` query parameter Jellyfin documents specifically for this case.
Jellyfin::Jellyfin(Arbiter &arbiter)
    : QObject(qApp)
    , arbiter(arbiter)
{
    Config *config = Config::get_instance();
    this->access_token = config->get_jellyfin_access_token();
    this->user_id = config->get_jellyfin_user_id();
    this->device_id = config->get_jellyfin_device_id();

    if (this->device_id.isEmpty()) {
        this->device_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        config->set_jellyfin_device_id(this->device_id);
    }

    // Deferred rather than called directly here - Jellyfin is constructed
    // very early in app bootstrap, before the event loop is running (see
    // net()'s comment for the crash that taught us that lesson).
    QTimer::singleShot(0, this, [this] {
        if (this->is_authenticated())
            this->schedule_auto_sync();
    });
}

QNetworkAccessManager *Jellyfin::net()
{
    // Constructed lazily on first actual use rather than in the constructor -
    // Jellyfin lives as a value member of Session::System, built very early
    // in app bootstrap (before the event loop is running), and a
    // QNetworkAccessManager built that early crashed the whole app on this
    // device. Deferring construction to the first real request sidesteps it.
    if (!this->network)
        this->network = new QNetworkAccessManager(this);
    return this->network;
}

QString Jellyfin::auth_header(bool with_token) const
{
    QString header = QString("MediaBrowser Client=\"dash\", Device=\"dash\", DeviceId=\"%1\", Version=\"1.0.0\"").arg(this->device_id);
    if (with_token)
        header += QString(", Token=\"%1\"").arg(this->access_token);
    return header;
}

bool Jellyfin::check_auth_error(QNetworkReply *reply)
{
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 401)
        return false;

    DASH_LOG(warning) << "[Jellyfin] token rejected (401), clearing it";

    this->access_token.clear();
    this->user_id.clear();

    Config *config = Config::get_instance();
    config->set_jellyfin_access_token(QString());
    config->set_jellyfin_user_id(QString());

    emit auth_required();
    return true;
}

Jellyfin::Item Jellyfin::parse_item(QJsonObject object) const
{
    Item item;
    item.id = object["Id"].toString();
    item.name = object["Name"].toString();
    item.parentId = object["ParentId"].toString();
    QString jf_type = object["Type"].toString();
    if (jf_type == "Audio")
        item.type = ItemType::Audio;
    else if (jf_type == "Movie" || jf_type == "Episode")
        item.type = ItemType::Video;
    else
        item.type = ItemType::Container;
    item.isFavorite = object["UserData"].toObject()["IsFavorite"].toBool();
    item.runtimeMs = object["RunTimeTicks"].toVariant().toLongLong() / 10000;
    item.sizeBytes = object["Size"].toVariant().toLongLong();
    item.indexNumber = object.contains("IndexNumber") ? object["IndexNumber"].toInt() : -1;
    return item;
}

void Jellyfin::authenticate(QString server_url, QString username, QString password)
{
    while (server_url.endsWith('/'))
        server_url.chop(1);
    // Users will naturally paste this straight from their browser's address
    // bar, which for Jellyfin is the SPA path (".../web/" or ".../web") -
    // strip it back to the actual API origin rather than making them edit
    // the URL by hand.
    if (server_url.endsWith("/web", Qt::CaseInsensitive))
        server_url.chop(4);

    Config *config = Config::get_instance();
    config->set_jellyfin_server_url(server_url);

    QNetworkRequest request(QUrl(server_url + "/Users/AuthenticateByName"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", this->auth_header(false).toUtf8());

    QJsonObject body;
    body["Username"] = username;
    body["Pw"] = password;

    QNetworkReply *reply = this->net()->post(request, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            emit auth_finished(false, reply->errorString());
            return;
        }

        QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        QString token = root["AccessToken"].toString();
        QString user_id = root["User"].toObject()["Id"].toString();
        if (token.isEmpty() || user_id.isEmpty()) {
            emit auth_finished(false, "Unexpected response from server");
            return;
        }

        this->access_token = token;
        this->user_id = user_id;

        Config *config = Config::get_instance();
        config->set_jellyfin_access_token(token);
        config->set_jellyfin_user_id(user_id);

        DASH_LOG(info) << "[Jellyfin] authenticated";
        emit auth_finished(true, QString());
        this->schedule_auto_sync();
    });
}

void Jellyfin::browse(QString parentId)
{
    if (!this->is_authenticated())
        return;

    // Stale-while-revalidate: menus felt slow because every navigation,
    // including back into a folder already visited this session, waited on
    // a fresh network round-trip. A cache hit renders instantly here, then
    // the request below still runs and silently replaces it once the
    // server responds, so a folder's contents never drift far out of date.
    // JellyfinTab already ignores an items_ready for a level the user has
    // since navigated away from, so emitting twice for one browse() call is
    // harmless, not a bug.
    auto cached = this->browse_cache.constFind(parentId);
    if (cached != this->browse_cache.constEnd())
        emit items_ready(parentId, cached.value());

    QString server = Config::get_instance()->get_jellyfin_server_url();

    if (parentId.isEmpty()) {
        QNetworkRequest request(QUrl(server + "/Users/" + this->user_id + "/Views"));
        request.setRawHeader("Authorization", this->auth_header(true).toUtf8());

        QNetworkReply *reply = this->net()->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            reply->deleteLater();
            if (this->check_auth_error(reply) || reply->error() != QNetworkReply::NoError)
                return;

            QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
            QList<Item> items;

            Item favorites;
            favorites.id = "favorites";
            favorites.name = "Favourites";
            favorites.type = ItemType::Container;
            items.append(favorites);

            for (const QJsonValue &value : root["Items"].toArray())
                items.append(this->parse_item(value.toObject()));

            this->browse_cache[QString()] = items;
            emit items_ready(QString(), items);
        });
        return;
    }

    QUrl url(server + "/Users/" + this->user_id + "/Items");
    QUrlQuery query;
    if (parentId == "favorites") {
        query.addQueryItem("Filters", "IsFavorite");
        query.addQueryItem("IncludeItemTypes", "Audio,Movie,Episode");
        query.addQueryItem("Recursive", "true");
    } else {
        query.addQueryItem("ParentId", parentId);
        query.addQueryItem("SortBy", "SortName");
        query.addQueryItem("SortOrder", "Ascending");
        query.addQueryItem("Recursive", "false");
    }
    query.addQueryItem("Fields", "Size");
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader("Authorization", this->auth_header(true).toUtf8());

    QNetworkReply *reply = this->net()->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, parentId] {
        reply->deleteLater();
        if (this->check_auth_error(reply) || reply->error() != QNetworkReply::NoError)
            return;

        QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        QList<Item> items;
        for (const QJsonValue &value : root["Items"].toArray())
            items.append(this->parse_item(value.toObject()));

        this->browse_cache[parentId] = items;
        emit items_ready(parentId, items);
    });
}

void Jellyfin::toggle_favorite(QString itemId, bool favorite)
{
    if (!this->is_authenticated())
        return;

    QString server = Config::get_instance()->get_jellyfin_server_url();
    QNetworkRequest request(QUrl(server + "/UserFavoriteItems/" + itemId));
    request.setRawHeader("Authorization", this->auth_header(true).toUtf8());

    QNetworkReply *reply = favorite ? this->net()->post(request, QByteArray()) : this->net()->deleteResource(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, itemId, favorite] {
        reply->deleteLater();
        if (this->check_auth_error(reply) || reply->error() != QNetworkReply::NoError)
            return;

        // Keep cached listings in sync so a revisited (cached) folder shows
        // the right star immediately instead of a stale one until the
        // background revalidate in browse() catches up. The Favourites
        // node's membership itself changes (not just a flag on an existing
        // row), so just drop that one and let it refetch clean.
        for (QList<Item> &items : this->browse_cache) {
            for (Item &item : items) {
                if (item.id == itemId)
                    item.isFavorite = favorite;
            }
        }
        this->browse_cache.remove("favorites");

        emit favorite_toggled(itemId, favorite);
    });
}

QUrl Jellyfin::stream_url(QString itemId, ItemType type) const
{
    QString kind = (type == ItemType::Video) ? "Videos" : "Audio";
    QUrl url(Config::get_instance()->get_jellyfin_server_url() + "/" + kind + "/" + itemId + "/stream");
    QUrlQuery query;
    query.addQueryItem("static", "true");
    query.addQueryItem("api_key", this->access_token);
    url.setQuery(query);
    return url;
}

QString Jellyfin::cached_path(QString itemId) const
{
    QDir dir(Config::get_instance()->get_jellyfin_offline_dir());
    QStringList matches = dir.entryList(QStringList() << (itemId + ".*"), QDir::Files);
    if (matches.isEmpty())
        return QString();
    return dir.absoluteFilePath(matches.first());
}

void Jellyfin::schedule_auto_sync()
{
    if (!this->sync_timer) {
        this->sync_timer = new QTimer(this);
        this->sync_timer->setInterval(30 * 60 * 1000);  // 30 min - a home server + a car that isn't always on doesn't need much tighter than this
        connect(this->sync_timer, &QTimer::timeout, this, &Jellyfin::sync_favorites);
    }
    this->sync_timer->start();
    this->sync_favorites();  // also run one now, rather than waiting a full interval after login/startup
}

void Jellyfin::sync_favorites()
{
    if (!this->is_authenticated() || this->syncing)
        return;

    this->syncing = true;

    QString server = Config::get_instance()->get_jellyfin_server_url();
    QUrl url(server + "/Users/" + this->user_id + "/Items");
    QUrlQuery query;
    query.addQueryItem("Filters", "IsFavorite");
    query.addQueryItem("IncludeItemTypes", "Audio,Movie,Episode");
    query.addQueryItem("Recursive", "true");
    query.addQueryItem("Fields", "Size");
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader("Authorization", this->auth_header(true).toUtf8());

    QNetworkReply *reply = this->net()->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (this->check_auth_error(reply) || reply->error() != QNetworkReply::NoError) {
            this->syncing = false;
            emit sync_finished(0, 0);
            return;
        }

        QDir().mkpath(Config::get_instance()->get_jellyfin_offline_dir());

        QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        this->sync_queue.clear();
        for (const QJsonValue &value : root["Items"].toArray()) {
            Item item = this->parse_item(value.toObject());
            if (this->cached_path(item.id).isEmpty())
                this->sync_queue.append(item);
        }

        this->sync_total = this->sync_queue.size();
        this->sync_done = 0;
        this->sync_failed = 0;

        this->download_next();
    });
}

void Jellyfin::download_next()
{
    if (this->sync_queue.isEmpty()) {
        this->syncing = false;
        DASH_LOG(info) << "[Jellyfin] sync finished, downloaded " << this->sync_done << ", failed " << this->sync_failed;
        emit sync_finished(this->sync_done, this->sync_failed);
        return;
    }

    Item item = this->sync_queue.takeFirst();
    emit sync_progress(this->sync_done, this->sync_total, item.name);

    // Movies/episodes can be multi-GB - unlike audio tracks, a blind
    // download can plausibly fill the disk. Skip (rather than attempt and
    // fail partway) when the server-reported size doesn't comfortably fit.
    // Size is unknown (0) for some libraries/items - nothing to check then,
    // so let it through same as before this existed.
    if (item.sizeBytes > 0) {
        QStorageInfo storage(Config::get_instance()->get_jellyfin_offline_dir());
        if (storage.bytesAvailable() < item.sizeBytes + (item.sizeBytes / 20)) {
            DASH_LOG(warning) << "[Jellyfin] skipping " << item.name.toStdString() << ", not enough free space";
            this->sync_failed++;
            this->download_next();
            return;
        }
    }

    QString server = Config::get_instance()->get_jellyfin_server_url();
    QUrl url(server + "/Items/" + item.id + "/Download");
    QUrlQuery query;
    query.addQueryItem("api_key", this->access_token);
    url.setQuery(query);

    QNetworkRequest request(url);
    QNetworkReply *reply = this->net()->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, item] {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            DASH_LOG(warning) << "[Jellyfin] failed to download " << item.name.toStdString();
            this->sync_failed++;
        } else {
            QByteArray data = reply->readAll();

            // Container isn't known ahead of time, so guess an extension
            // from the response's content type rather than assuming one -
            // good enough for local playback, doesn't need to be exact.
            QString content_type = reply->header(QNetworkRequest::ContentTypeHeader).toString();
            QString ext = (item.type == ItemType::Video) ? "mp4" : "audio";
            if (content_type.contains("matroska"))
                ext = "mkv";
            else if (content_type.contains("webm"))
                ext = "webm";
            else if (content_type.contains("x-msvideo"))
                ext = "avi";
            else if (content_type.contains("mp4"))
                ext = (item.type == ItemType::Video) ? "mp4" : "m4a";
            else if (content_type.contains("flac"))
                ext = "flac";
            else if (content_type.contains("mpeg") || content_type.contains("mp3"))
                ext = "mp3";
            else if (content_type.contains("m4a") || content_type.contains("aac"))
                ext = "m4a";
            else if (content_type.contains("ogg"))
                ext = "ogg";
            else if (content_type.contains("wav"))
                ext = "wav";

            // Filenames are keyed on Id, never on display name - a track or
            // album title can contain '/' or other characters that aren't
            // safe in a flat filename (this bit tonight's DAB work, where a
            // station label with a literal '/' broke welle-cli's own file
            // dump until its parent directory was pre-created). Id is
            // always filesystem-safe and stable across renames.
            QFile file(Config::get_instance()->get_jellyfin_offline_dir() + "/" + item.id + "." + ext);
            if (file.open(QIODevice::WriteOnly) && file.write(data) == data.size())
                this->sync_done++;
            else
                this->sync_failed++;
        }

        this->download_next();
    });
}
