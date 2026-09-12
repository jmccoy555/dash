#include <algorithm>

#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTimer>

#include "app/arbiter.hpp"
#include "app/config.hpp"
#include "app/services/gps.hpp"

Gps::Gps(Arbiter &arbiter)
    : QObject(qApp)
    , arbiter(arbiter)
    , socket(new QTcpSocket(this))
    , status_("Not configured")
{
    connect(this->socket, &QTcpSocket::connected, this, [this] {
        // Disables Nagle's algorithm - without this, TCP was observed
        // batching gpsd's small once-a-second JSON reports before sending,
        // compounding with delayed-ACK on the other end into a growing lag
        // between the car's real position and what dash was forwarding on
        // (reported as ~300m behind at driving speed).
        this->socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);

        // json:true switches gpsd to newline-delimited JSON reports instead
        // of its older plain-text protocol - every gpsd release in the last
        // decade-plus supports this.
        this->socket->write("?WATCH={\"enable\":true,\"json\":true}\r\n");
        this->status_ = "Connected, waiting for fix…";
        emit this->status_changed(this->status_);
    });

    connect(this->socket, &QTcpSocket::readyRead, this, [this] {
        this->buffer += this->socket->readAll();

        int newline;
        while ((newline = this->buffer.indexOf('\n')) >= 0) {
            QByteArray line = this->buffer.left(newline);
            this->buffer.remove(0, newline + 1);
            this->handle_line(line);
        }
    });

    connect(this->socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        this->status_ = "Connection failed: " + this->socket->errorString();
        emit this->status_changed(this->status_);
    });

    connect(this->socket, &QTcpSocket::disconnected, this, [this] {
        this->status_ = "Disconnected - retrying…";
        emit this->status_changed(this->status_);
        // Stop feeding the last-known fix while disconnected - otherwise
        // Android Auto keeps seeing "live" updates from a source that's
        // actually gone stale, and never falls back to the phone's own GPS.
        this->arbiter.android_auto().handler->clearLocation();
        QTimer::singleShot(5000, this, [this] { this->connect_socket(); });
    });

    this->enabled = Config::get_instance()->get_gps_enabled();
    this->configure(Config::get_instance()->get_gps_host(), Config::get_instance()->get_gps_port());
}

void Gps::configure(QString host, int port)
{
    this->host = host;
    this->port = port;
    this->socket->abort();
    this->buffer.clear();
    this->connect_socket();
}

void Gps::set_enabled(bool enabled)
{
    this->enabled = enabled;
    this->socket->abort();
    this->buffer.clear();
    this->connect_socket();

    // Only ever called from the Settings page at runtime (never during
    // construction, unlike configure()), so arbiter.android_auto() is
    // always safe here - see the dangling-handler note on handle_line().
    if (!enabled)
        this->arbiter.android_auto().handler->clearLocation();
}

void Gps::connect_socket()
{
    if (!this->enabled) {
        this->status_ = "Disabled";
        emit this->status_changed(this->status_);
        return;
    }

    if (this->host.isEmpty() || this->port <= 0) {
        this->status_ = "Not configured";
        emit this->status_changed(this->status_);
        return;
    }

    this->status_ = "Connecting…";
    emit this->status_changed(this->status_);
    this->socket->connectToHost(this->host, this->port);
}

void Gps::handle_line(QByteArray line)
{
    QJsonObject obj = QJsonDocument::fromJson(line).object();
    if (obj.value("class").toString() != "TPV")
        return;

    // gpsd's own fix-quality convention: 0=unknown, 1=no fix, 2=2D, 3=3D -
    // anything below a 2D fix has no usable lat/lon at all.
    int mode = obj.value("mode").toInt();
    if (mode < 2 || !obj.contains("lat") || !obj.contains("lon"))
        return;

    double lat = obj.value("lat").toDouble();
    double lon = obj.value("lon").toDouble();
    // altMSL (mean sea level) is what newer gpsd calls it - older versions
    // just call it alt. Either is close enough for this purpose.
    double altitude = obj.contains("altMSL") ? obj.value("altMSL").toDouble() : obj.value("alt").toDouble(0);
    double speed = obj.value("speed").toDouble(0);    // m/s
    double bearing = obj.value("track").toDouble(0);  // degrees
    double accuracy = std::max(obj.value("epx").toDouble(10), obj.value("epy").toDouble(10));  // metres - epx/epy are gpsd's estimated position errors

    this->status_ = QString("Fix: %1, %2").arg(lat, 0, 'f', 5).arg(lon, 0, 'f', 5);
    emit this->status_changed(this->status_);

    // Session::System is constructed before Session::AndroidAuto (see
    // session.hpp) - arbiter.android_auto() would be dangling if touched
    // from this class's own constructor, but is always safe here since this
    // only ever runs from an async socket callback, long after the whole
    // Session is fully built.
    this->arbiter.android_auto().handler->setLocation(lat, lon, altitude, speed, bearing, accuracy);
}
