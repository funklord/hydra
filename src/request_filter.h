// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QSet>
#include <QString>
#include <QUrl>

class policy_engine;

// The kinds of request every engine can distinguish. Qt WebEngine reports far
// more resource types and Android's WebView reports fewer; this is the subset
// the policy rules actually key off, so both can map onto it.
enum class resource_kind { script, image, other };

// What the shell wants done with a request, as flags rather than an action, so
// a backend can honour the parts it supports.
struct request_decision {
	bool block         = false;
	bool strip_referer = false;
};

// One request, reduced to what a decision needs — plus the full URL, which
// the decision ignores but the media detector (§11.1) reads for extensions
// and manifest paths.
struct request_context {
	QString       request_host;   // host the request goes to
	QString       site_host;      // host of the page making it
	QUrl          url;            // the full request URL
	resource_kind kind = resource_kind::other;
};

// The interceptor is a shared *sensor*, not just a gate (architecture doc §10):
// ad-blocking, media detection, and filter-evolution signal collection all ride
// the same stream of requests. Observers see every request and its decision.
//
// Thread note: on_request() is called from wherever the platform's interceptor
// runs, which on Qt WebEngine is not the UI thread. Implementations must be
// safe to call concurrently and must not touch widgets directly.
class request_observer {
public:
	virtual ~request_observer() = default;
	virtual void on_request(const request_context &ctx, const request_decision &d) = 0;
};

// The platform-neutral half of request interception (architecture doc §7.3,
// §19.5). Deciding what to block is identical on every platform because it is
// just policy plus a host list; only the plumbing that delivers requests
// differs, so that plumbing stays in the per-platform interceptor and this
// stays shared.
//
// Thread note: decide() may be called off the UI thread. It only reads the
// policy engine, which is mutated on the UI thread and tolerates a stale
// snapshot.
class request_filter {
public:
	explicit request_filter(policy_engine *engine);

	request_decision decide(const request_context &ctx) const;

	// Cookie decisions are the same shape: policy plus the first-party host.
	bool allow_cookie(const QString &site_host, bool third_party) const;

	bool is_ad_host(const QString &host) const;

	// Observers are registered on the UI thread before browsing starts and are
	// not removed while requests are in flight.
	// A null here is a construction-order mistake at the call site, and keeping
	// it turns that into undefined behaviour on some later request rather than a
	// mistake where it was made. It has happened once: an observer registered
	// before it was constructed was harmless for as long as the function it
	// landed in touched no members, then segfaulted every live driver the day
	// one did.
	void add_observer(request_observer *o) { if (o) m_observers.push_back(o); }
	void remove_observer(request_observer *o) { m_observers.removeAll(o); }
	void notify(const request_context &ctx, const request_decision &d) const;

private:
	policy_engine *m_engine;
	QSet<QString>  m_ad_hosts;
	QList<request_observer *> m_observers;
};
