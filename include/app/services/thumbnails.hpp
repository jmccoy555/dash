#pragma once

#include <functional>

#include <QList>
#include <QMap>
#include <QObject>
#include <QPixmap>
#include <QString>

class Arbiter;
class QNetworkAccessManager;

// Shared async image fetch + in-memory cache for Jellyfin cover art and
// DashTube thumbnails - the two tabs that need "grid tile with a picture"
// share this rather than each doing their own fetching, since the fetch,
// cache, and safe-callback-after-widget-destroyed handling are identical
// either way.
class Thumbnails : public QObject {
    Q_OBJECT

   public:
    Thumbnails(Arbiter &arbiter);

    // Calls back on the main thread once loaded (immediately, from cache, if
    // this url was already fetched this session). context is whatever
    // widget the callback touches - passing it as the connect() receiver
    // means a reply that finishes after that widget's been destroyed (user
    // navigated away mid-fetch) is safely dropped instead of touching freed
    // memory, same trick used for QNetworkReply::finished elsewhere in this
    // app.
    void load(QString url, QObject *context, std::function<void(QPixmap)> callback);

   private:
    QNetworkAccessManager *net();

    Arbiter &arbiter;
    // A single QNetworkAccessManager caps itself at ~6 concurrent
    // connections per host, which turns "load a few hundred small
    // thumbnails for one big folder" into a slow trickle even though each
    // one individually is fast (confirmed live - ~50ms server-side per
    // image). Round-robining across a small pool of managers multiplies
    // that ceiling, since the connection limit is tracked per-manager, not
    // per-process.
    QList<QNetworkAccessManager *> pool;
    int next = 0;
    QMap<QString, QPixmap> cache;
};
