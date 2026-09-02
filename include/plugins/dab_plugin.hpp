#pragma once

#include <QList>
#include <QString>

#include "plugins/plugin.hpp"

struct DabService {
    QString id;
    QString label;
};

class DabPlugin : public Plugin {
   public:
    DabPlugin() { this->settings.beginGroup("Dab"); }
    virtual ~DabPlugin() = default;

    virtual QList<DabService> services() = 0;
    virtual bool scanning() = 0;
    virtual void rescan() = 0;
    virtual void play(QString service_id) = 0;
    virtual void stop() = 0;
    virtual QString now_playing() = 0;
};

#define DabPlugin_iid "openDsh.plugins.DabPlugin"

Q_DECLARE_INTERFACE(DabPlugin, DabPlugin_iid)
