#include <QApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include "app/services/thumbnails.hpp"

Thumbnails::Thumbnails(Arbiter &arbiter)
    : QObject(qApp)
    , arbiter(arbiter)
{
}

QNetworkAccessManager *Thumbnails::net()
{
    // Lazily constructed - see Jellyfin::net()'s comment for why (this
    // service lives in Session::System too, built before the event loop is
    // running).
    if (!this->network)
        this->network = new QNetworkAccessManager(this);
    return this->network;
}

void Thumbnails::load(QString url, QObject *context, std::function<void(QPixmap)> callback)
{
    if (url.isEmpty())
        return;

    auto cached = this->cache.constFind(url);
    if (cached != this->cache.constEnd()) {
        callback(cached.value());
        return;
    }

    QNetworkRequest request((QUrl(url)));
    QNetworkReply *reply = this->net()->get(request);
    connect(reply, &QNetworkReply::finished, context, [this, reply, url, callback] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return;

        QPixmap pixmap;
        if (!pixmap.loadFromData(reply->readAll()))
            return;

        this->cache[url] = pixmap;
        callback(pixmap);
    });
}
