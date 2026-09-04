#include <QApplication>

#include "app/services/recently_played.hpp"

RecentlyPlayed::RecentlyPlayed(Arbiter &arbiter)
    : QObject(qApp)
    , arbiter(arbiter)
    , settings()
{
    this->load();
}

void RecentlyPlayed::load()
{
    this->entries_.clear();
    int size = this->settings.beginReadArray("Recent/entries");
    for (int i = 0; i < size; i++) {
        this->settings.setArrayIndex(i);
        Entry entry;
        entry.source = this->settings.value("source").toString();
        entry.id = this->settings.value("id").toString();
        entry.title = this->settings.value("title").toString();
        entry.image_url = this->settings.value("image_url").toString();
        entry.variant = this->settings.value("variant").toInt();
        this->entries_.append(entry);
    }
    this->settings.endArray();
}

void RecentlyPlayed::save()
{
    this->settings.beginWriteArray("Recent/entries");
    for (int i = 0; i < this->entries_.size(); i++) {
        this->settings.setArrayIndex(i);
        this->settings.setValue("source", this->entries_[i].source);
        this->settings.setValue("id", this->entries_[i].id);
        this->settings.setValue("title", this->entries_[i].title);
        this->settings.setValue("image_url", this->entries_[i].image_url);
        this->settings.setValue("variant", this->entries_[i].variant);
    }
    this->settings.endArray();
}

void RecentlyPlayed::record(Entry entry)
{
    for (int i = 0; i < this->entries_.size(); i++) {
        if (this->entries_[i].source == entry.source && this->entries_[i].id == entry.id) {
            this->entries_.removeAt(i);
            break;
        }
    }

    this->entries_.prepend(entry);
    while (this->entries_.size() > MAX_ENTRIES)
        this->entries_.removeLast();

    this->save();
    emit this->changed();
}
