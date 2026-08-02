// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "policy.h"
#include "consent_rules.h"

#include <QObject>
#include <QString>

class policy_engine;

// The "do you want to accept cookies?" banner, answered for you.
//
// It is a blocking option beside ads and popups rather than a setting under
// cookies, because it is a different question: not what a site may *store*, but
// what it may put in front of the page before you are allowed to read it.
// Blocked -- the default -- means the banner is answered and dismissed.
// Allowed means it is left alone and you answer it yourself.
//
// **What it answers with, and why that is not always "reject".** The point of
// the option is to be able to use the page, so the choice is the least
// permissive one the banner offers that gets it out of the way: reject-all
// where there is such a button, otherwise the necessary-only option, otherwise
// accept. A banner with nothing but "Accept" is a banner whose only dismissal
// is acceptance, and pretending otherwise leaves the page unusable, which is
// the thing being fixed.
//
// **And the choice has to stick, which is why this touches cookie policy.** A
// consent choice is itself recorded in a cookie. With cookies blocked for the
// site there is nowhere to record it, so the banner returns on the next load
// and every load after that, and the option appears not to work. So dismissing
// a banner relaxes *first-party* cookies for that host -- never third-party,
// which is the part consent dialogs exist to bargain for -- and writes it as an
// ordinary per-site rule, visible in the shield and revertible there like any
// other. Nothing silent, nothing that cannot be found again.
//
// The security shape is §13.2's, unchanged: the page-side script does detection
// and clicking, and every fact that matters is decided here. The host comes
// from the view's real URL and never from the page, so a frame cannot ask for a
// relaxation on someone else's behalf.
class consent_blocker : public QObject {
	Q_OBJECT
public:
	explicit consent_blocker(policy_engine *policy, QObject *parent = nullptr);

	// The content script, with the rule set substituted in. It is injected into
	// every view and asks `active_now()` before it touches anything, because the
	// per-site setting can change after a view is built and a script that
	// assumed otherwise would keep answering banners on a site the user had just
	// told it to leave alone.
	static QString script_source(const consent_rules &rules);

	// The object name the script expects on the bridge.
	static const char *bridge_name() { return "hydraConsent"; }

	// Called by the shell on navigation. The page never sets this.
	void set_page_host(const QString &host);
	QString page_host() const { return m_host; }

	// Whether the banner should be answered for this host at all.
	bool active_for(const QString &host) const;

	// What was dismissed, most recent first, for the status line and the tests.
	QStringList dismissed() const { return m_dismissed; }

public slots:
	// Whether the page currently shown should have its banner answered. Asked by
	// the script before it acts; the host is the shell's, not the page's.
	bool active_now() const { return !m_host.isEmpty() && active_for(m_host); }

	// --- Reachable from the injected script. Every argument is hostile.
	//
	// `what` is a short description of the banner it acted on and `choice` is
	// which option it took. Neither is trusted for anything but display; the
	// host it applies to is the one the shell set.
	void report_dismissed(const QString &what, const QString &choice);

signals:
	// For the status bar: a page just had its banner answered.
	void acted(const QString &host, const QString &choice);
	// Raised when the dismissal required relaxing first-party cookies, because
	// a policy change the user did not make should be visible when it happens
	// and not only findable afterwards in the shield.
	void relaxed_cookies(const QString &host);

private:
	policy_engine *m_policy = nullptr;
	QString        m_host;
	QStringList    m_dismissed;
};
