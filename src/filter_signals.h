// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "request_filter.h"

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QStringList>

// Passive signal collection for the filter-evolution loop (architecture doc
// sec 12.1): the second interceptor consumer. It logs requests that slipped
// through and match ad-shaped heuristics -- third-party, ad-serving URL shapes,
// high-frequency beacons -- so the AI has real evidence to propose against and
// the dry-run has real URLs to simulate on.
//
// sec 12.1's *user-driven* half -- the "zap this" element picker that captures a
// leaked ad's selector and DOM snippet -- is not here. It needs script
// injection and a QWebChannel bridge into the page, which is plumbing that
// arrives with the password manager in step 7 (sec 13.2). Until then the loop runs
// on passive evidence only, which is enough for network rules but not for
// cosmetic ones.
//
// Thread note: fed from the interceptor thread, read from the UI thread.
class filter_signals : public QObject, public request_observer {
	Q_OBJECT
public:
	explicit filter_signals(QObject *parent = nullptr);

	void on_request(const request_context &ctx, const request_decision &d) override;

	// Requests that got through and look ad-shaped, for this page.
	QStringList suspects_for(const QString &site_host) const;
	// How many of those there are. **Suspects, not everything observed** --
	// this sat directly under `observed_for` and was read as its count by the
	// next caller to arrive, who then showed "N requests seen, M ad-shaped"
	// with N and M always equal. Moved up beside the list it actually counts.
	int count_for(const QString &site_host) const;

	// Everything seen on this page -- the corpus a rule is simulated against.
	QStringList observed_for(const QString &site_host) const;

	void clear_site(const QString &site_host);

	// Heuristics only, deliberately loose: this decides what to *show a human*,
	// not what to block.
	static bool looks_ad_shaped(const QString &url, const QString &request_host,
	                             const QString &site_host);

private:
	mutable QMutex m_lock;
	QHash<QString, QStringList> m_suspects;
	QHash<QString, QStringList> m_observed;
};
