// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QSet>
#include <QString>

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

// One request, reduced to what a decision needs.
struct request_context {
	QString       request_host;   // host the request goes to
	QString       site_host;      // host of the page making it
	resource_kind kind = resource_kind::other;
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

private:
	policy_engine *m_engine;
	QSet<QString>  m_ad_hosts;
};
