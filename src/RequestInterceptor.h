#pragma once

#include <QWebEngineUrlRequestInterceptor>
#include <QSet>
#include <QString>

class PolicyEngine;

// The shared request sensor (architecture doc §7.3/§10). Consults the
// PolicyEngine to block ads/trackers, block scripts from non-allowed origins,
// block images per site, and strip the Referer header per site. A small seed
// ad-host list stands in for the EasyList import / filter-evolution loop.
class RequestInterceptor : public QWebEngineUrlRequestInterceptor {
    Q_OBJECT
public:
    explicit RequestInterceptor(PolicyEngine* engine, QObject* parent = nullptr);

    void interceptRequest(QWebEngineUrlRequestInfo& info) override;

private:
    bool isAdHost(const QString& host) const;

    PolicyEngine*  engine_;
    QSet<QString>  adHosts_;
};
