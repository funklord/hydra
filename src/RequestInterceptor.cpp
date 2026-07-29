#include "RequestInterceptor.h"
#include "PolicyEngine.h"

#include <QWebEngineUrlRequestInfo>

using policy::Feature;

RequestInterceptor::RequestInterceptor(PolicyEngine* engine, QObject* parent)
    : QWebEngineUrlRequestInterceptor(parent), engine_(engine) {
    // Seed list only. The filter-evolution loop (step 6) grows this and imports
    // EasyList; kept tiny here on purpose.
    adHosts_ = {
        "doubleclick.net",
        "googlesyndication.com",
        "googleadservices.com",
        "google-analytics.com",
        "adservice.google.com",
        "ads.yahoo.com",
        "adnxs.com",
        "scorecardresearch.com",
        "moatads.com",
        "taboola.com",
        "outbrain.com",
    };
}

bool RequestInterceptor::isAdHost(const QString& host) const {
    for (const QString& ad : adHosts_)
        if (host == ad || host.endsWith("." + ad))
            return true;
    return false;
}

void RequestInterceptor::interceptRequest(QWebEngineUrlRequestInfo& info) {
    if (!engine_)
        return;

    const QString reqHost  = info.requestUrl().host();
    const QString siteHost = info.firstPartyUrl().host();  // the page's site
    const auto    type     = info.resourceType();

    using RT = QWebEngineUrlRequestInfo;

    // Ads / trackers: block known ad hosts unless this site permits ads.
    if (isAdHost(reqHost) && !engine_->isAllowed(Feature::Ads, siteHost)) {
        info.block(true);
        return;
    }

    // Per-origin scripts: block scripts served from a host whose JS is blocked.
    if (type == RT::ResourceTypeScript &&
        !engine_->isAllowed(Feature::JavaScript, reqHost)) {
        info.block(true);
        return;
    }

    // Images: block per site.
    if (type == RT::ResourceTypeImage &&
        !engine_->isAllowed(Feature::Images, siteHost)) {
        info.block(true);
        return;
    }

    // Referer: strip when the site's Referer policy is Block.
    if (!engine_->isAllowed(Feature::Referer, siteHost)) {
        info.setHttpHeader(QByteArray("Referer"), QByteArray());
    }
}
