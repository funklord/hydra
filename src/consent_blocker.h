// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "policy.h"
#include "site_rules.h"

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
// The security shape is sec 13.2's, unchanged: the page-side script does detection
// and clicking, and every fact that matters is decided here. The host comes
// from the view's real URL and never from the page, so a frame cannot ask for a
// relaxation on someone else's behalf.
class consent_blocker : public QObject {
	Q_OBJECT
public:
	explicit consent_blocker(policy_engine *policy, QObject *parent = nullptr);

	// The content script. It is injected into every view and asks C++ two things
	// before it touches anything: whether this page is one to act on, and what
	// the rules are.
	//
	// Both are *asked* rather than substituted in at injection time, and that is
	// what makes a newly learned rule work without rebuilding every view: a
	// script with the rules baked in carries whatever was true when its tab was
	// created, and re-injecting to update it would leave two copies racing a
	// guard flag. Asking also keeps one source of truth, which was the point of
	// substituting them in the first place.
	static QString script_source();

	// The rules in force. The shell owns the file; this owns the answer.
	void set_rules(const site_rules &r) { m_rules = r; }
	const site_rules &rules() const { return m_rules; }

	// The object name the script expects on the bridge.
	static const char *bridge_name() { return "hydraConsent"; }

	// Called by the shell on navigation. The page never sets this.
	void set_page_host(const QString &host);
	QString page_host() const { return m_host; }

	// Whether the banner should be answered for this host at all.
	bool active_for(const QString &host) const;

	// What was dismissed, most recent first, for the status line and the tests.
	QStringList dismissed() const { return m_dismissed; }

	// Banners seen and not answered, newest first, as "host\tlabel\tlabel...".
	// Bounded: a page that reports in a loop must not be able to grow this.
	QStringList unhandled() const { return m_unhandled; }

	// Turn one of those labels into a rule.
	//
	// **A button label is generic and a selector is not**, which is the whole
	// reason this returns what it does. "Godta alle" is Norwegian for accept and
	// works on every Norwegian site, so it belongs in the shipped defaults and is
	// flagged for them; `#accept-btn-42` describes one page. So a rule learned
	// from a label carries no host, which marks it `promote` on insertion, and
	// the maintainer folding `promotable()` into `defaults()` next release is
	// how everyone else gets it.
	site_rule rule_from_label(const QString &label, const QString &as) const;

public slots:
	// Whether the page currently shown should have its banner answered. Asked by
	// the script before it acts; the host is the shell's, not the page's.
	bool active_now() const { return !m_host.isEmpty() && active_for(m_host); }

	// What to match with, for the page currently shown: the generic rules plus
	// that host's own. A page cannot ask about a site it is not on, because the
	// host is the shell's.
	QString rules_json() const { return m_rules.for_host(m_host).to_script_literal(); }

	// --- Reachable from the injected script. Every argument is hostile.
	//
	// `what` is a short description of the banner it acted on and `choice` is
	// which option it took. Neither is trusted for anything but display; the
	// host it applies to is the one the shell set.
	void report_dismissed(const QString &what, const QString &choice);

	// A banner that *was* found and could not be answered: consent-shaped, on
	// screen, and not one button in it matched anything we know. `labels` is
	// what it offered, tab-separated.
	//
	// This is the discovery signal, and it is the same shape sec 12 uses for
	// filters: the system records where it fell short rather than guessing, and
	// a rule is proposed from real evidence afterwards. A banner nobody could
	// answer is exactly the case a new rule has to cover, and the labels it
	// offered are most of the answer already.
	void report_unhandled(const QString &labels);

signals:
	// For the status bar: a page just had its banner answered.
	void acted(const QString &host, const QString &choice);
	// A banner nobody could answer, for whatever offers to teach a rule.
	void found_unanswerable(const QString &host, const QString &labels);
	// Raised when the dismissal required relaxing first-party cookies, because
	// a policy change the user did not make should be visible when it happens
	// and not only findable afterwards in the shield.
	void relaxed_cookies(const QString &host);

private:
	policy_engine *m_policy = nullptr;
	QString        m_host;
	QStringList    m_dismissed;
	QStringList    m_unhandled;
	site_rules  m_rules = site_rules::defaults();
};
