// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

// "Something got through here" — a report a person files in one click, and the
// evidence that was on screen when they filed it.
//
// **The point is that it costs nothing to file.** The three teaching tools this
// project already has — the element picker, filter evolution, the consent-rule
// editor — all require knowing *what* went wrong and acting precisely on it.
// The hard part of writing a filter rule is not the rule; it is being back in
// the moment where the thing happened, with the traffic that caused it still in
// front of you. This records that moment so the diagnosis can come later, or
// from somebody else, or from a model.
//
// **It captures nothing new.** `filter_signals` is already accumulating, per
// site, the ad-shaped requests that got through and the whole corpus a
// candidate rule gets simulated against. A report is a *marker* on evidence
// that exists, which is why this is a small class rather than a subsystem.
//
// **Stored where the rest of the per-site state is**, as an INI beside the tree
// file, because a record of what somebody found annoying is a record of where
// they have been. It belongs with the policy they can already read and revoke,
// not in an analytics store they cannot see. `clear_host` and `clear_all` exist
// for that reason and are reachable from the UI.
struct annoyance_report {
	QDateTime   when;
	QString     host;
	QString     page;        // the address that was open
	QStringList suspects;    // ad-shaped requests that got through, at that moment
	int         observed = 0;  // how many requests the page had made in total
	// What the person did next, if anything: "recorded", "zapped", "evolved",
	// "consent". Kept because a report nobody acted on is the interesting kind
	// -- it is the one where the tools on offer did not fit the problem.
	QString     outcome;
};

class annoyance_log {
public:
	void add(const annoyance_report &r);

	QList<annoyance_report> all() const { return m_reports; }
	QList<annoyance_report> for_host(const QString &host) const;
	int  count_for(const QString &host) const;
	bool is_empty() const { return m_reports.isEmpty(); }

	// Record what came of the most recent report for a host. Separate from
	// `add` because the outcome is known only after a dialog the person may
	// dismiss, and a report filed is worth keeping whether or not they followed
	// through.
	void set_outcome(const QString &host, const QString &outcome);

	void clear_host(const QString &host);
	void clear_all() { m_reports.clear(); }

	bool load(const QString &path);
	bool save(const QString &path) const;

private:
	QList<annoyance_report> m_reports;
};
