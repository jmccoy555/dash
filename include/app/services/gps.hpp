#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

class Arbiter;
class QTcpSocket;

// Feeds Android Auto's LOCATION sensor (openauto's SensorService, reached
// through AAHandler/IAndroidAutoInterface::setLocation()) from an external
// GPS - this box has no GNSS hardware of its own, but a modem/GPS box
// elsewhere on the car's network might have a clearer view of the sky than
// a phone stuck in a cupholder. Speaks gpsd's own JSON-over-TCP protocol
// (the standard way to get a fix off a Linux box, conventionally port 2947)
// rather than any specific modem's own protocol, since that's how a GNSS
// source is normally exposed on a router/gateway box (e.g. OpenWrt) - point
// this at wherever gpsd is already running, nothing modem-specific needed
// here.
class Gps : public QObject {
    Q_OBJECT

   public:
    Gps(Arbiter &arbiter);

    // (Re)connects using the given host/port - empty host disables it.
    // Called once at startup with whatever Config already has, and again
    // whenever the Settings page's fields are edited.
    void configure(QString host, int port);

    // Separate off switch from host/port - see Config::get_gps_enabled().
    void set_enabled(bool enabled);

    QString status() const { return this->status_; }

   private:
    void connect_socket();
    void handle_line(QByteArray line);

    Arbiter &arbiter;
    QTcpSocket *socket;
    QString host;
    int port = 0;
    bool enabled = true;
    QByteArray buffer;
    QString status_;  // human-readable, for a status label in Settings - "Not configured" / "Connecting…" / "Fix: 51.50740, -0.12780"

    // Logging only - this class had zero log output at all until a "no
    // speed in Waze" report turned into hours of guesswork with no way to
    // check, after the fact, whether dash had actually been receiving GPS
    // data during a given drive. logged_first_fix_ resets on every (re)
    // connect so the log shows one line per fix-acquired event rather than
    // one per second; last_fix_time_ backs a periodic heartbeat that catches
    // the "socket still connected but data silently stopped arriving" case,
    // which a plain connect/disconnect log wouldn't show at all.
    bool logged_first_fix_ = false;
    qint64 last_fix_time_ = 0;
    QTimer heartbeat_;  // periodic silent-stall check - see last_fix_time_'s comment

   signals:
    void status_changed(QString status);
};
