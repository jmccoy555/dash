#pragma once

#include <QList>
#include <QObject>
#include <QSettings>
#include <QString>

class Arbiter;

// A small shared history of what's been played, across every Media tab that
// has a real "play this specific thing" concept (Local, Jellyfin, DashTube,
// DAB) - Bluetooth is deliberately left out, since dash doesn't control the
// phone's queue and so has nothing to actually replay. Feeds the Recent tab.
class RecentlyPlayed : public QObject {
    Q_OBJECT

   public:
    struct Entry {
        QString source;     // "Local" | "Jellyfin" | "DashTube" | "DAB" - which tab this came from, and how RecentTab::play() dispatches it
        QString id;          // source-specific: file path / Jellyfin item id / YouTube video id / DAB service id
        QString title;
        QString image_url;   // Jellyfin/DashTube cover art - empty for Local (embedded art, looked up directly by id/path) and DAB (no art)
        int variant = 0;     // source-specific extra - Jellyfin::ItemType as int, unused by other sources
    };

    RecentlyPlayed(Arbiter &arbiter);

    // Moves an existing entry (matched on source+id) to the front instead of
    // duplicating it, so replaying something already in the list just
    // freshens its position rather than listing it twice.
    void record(Entry entry);
    const QList<Entry> &entries() const { return this->entries_; }

   private:
    static const int MAX_ENTRIES = 30;

    Arbiter &arbiter;
    QSettings settings;
    QList<Entry> entries_;

    void load();
    void save();

   signals:
    void changed();
};
