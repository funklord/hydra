// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWebEngineUrlRequestInterceptor>
#include <QSet>
#include <QString>

class policy_engine;

// The shared request sensor (architecture doc §7.3/§10). Consults the
// policy_engine to block ads/trackers, block scripts from non-allowed origins,
// block images per site, and strip the Referer header per site. A small seed
// ad-host list stands in for the EasyList import / filter-evolution loop.
class request_interceptor : public QWebEngineUrlRequestInterceptor {
	Q_OBJECT
public:
	explicit request_interceptor(policy_engine *engine, QObject *parent = nullptr);

	void interceptRequest(QWebEngineUrlRequestInfo &info) override;

private:
	bool is_ad_host(const QString &host) const;

	policy_engine *m_engine;
	QSet<QString>  m_ad_hosts;
};
