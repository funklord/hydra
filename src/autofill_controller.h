// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "keepass_protocol.h"

#include <QHash>
#include <QObject>
#include <QString>

class keepass_bridge;
class policy_engine;

// The object injected pages talk to (architecture doc §13.2), and the gate
// between an untrusted page and the vault.
//
// Everything a page can reach goes through here, so this is where §13.3's
// rules are enforced rather than trusted to the content script:
//
//  * The page does not choose the origin it is asking about. The shell sets it
//    from the real URL of the view, and a request naming anything else is
//    refused — otherwise a cross-origin iframe could ask for the top page's
//    credentials, or a lookalike could ask for the real site's.
//  * Autofill is a PolicyEngine feature, so the per-site tri-state and the
//    global default govern it exactly like JavaScript or cookies, and a
//    policy-blocked site is never filled.
//  * HTTPS-only fill is on by default; filling a password over plain HTTP puts
//    it on the wire.
//  * Nothing is auto-submitted, and credentials are held only for the fill
//    that asked for them.
class autofill_controller : public QObject {
	Q_OBJECT
public:
	autofill_controller(keepass_bridge *bridge, policy_engine *policy,
	                     QObject *parent = nullptr);

	// Called by the shell on navigation. The page never sets this.
	void set_page_origin(const QString &origin);
	QString page_origin() const { return m_origin; }

	void set_https_only(bool on) { m_https_only = on; }

	// Why a fill would be refused right now; empty when it would proceed.
	QString blocked_reason(const QString &origin) const;

public slots:
	// --- Reachable from injected page scripts. Treat every argument as hostile.
	void request_credentials(const QString &origin);

	// The shell's answer to `choice_needed`: which of the offered entries to
	// fill, by index, or a negative number to fill nothing. Called from the
	// picker, never from a page -- it is not a slot for that reason.
	void choose(int index);

	// Hand the controller a reply as if KeePassXC had sent one.
	//
	// A test seam, and named so nobody mistakes it for anything else. The
	// decision this class makes -- fill one, ask about several, say so about
	// none -- is worth checking without a paired vault, and the path that
	// normally reaches it needs a live bridge, a socket and a human confirming
	// a dialog. Not a slot, so no page can reach it; `try_keepass` covers the
	// same code arriving from a real KeePassXC.
	void offer_for_test(const QList<credential> &entries);

signals:
	// Delivered back to the page: a JSON array of {name, login, password}.
	// **At most one entry**, and only ever the one that is going to be filled.
	void credentials_ready(const QString &json);
	// A human-readable refusal, for the key icon's tooltip.
	void refused(const QString &reason);
	// More than one entry matched, so a person has to say which. Carries the
	// names and logins **without the passwords**: the picker shows what is
	// needed to tell two accounts apart and no more, and the password is
	// still held here until `choose` says where it is going.
	void choice_needed(const QStringList &labels);

private:
	void on_logins(int tag, const QList<credential> &entries);
	void deliver(const credential &c);

	keepass_bridge *m_bridge = nullptr;
	policy_engine  *m_policy = nullptr;
	QString m_origin;
	bool    m_https_only = true;
	int     m_next_tag = 1;
	int     m_pending  = 0;

	// Entries waiting on a person to choose between them. Held **here** rather
	// than handed to the page to sort out: the old arrangement sent every
	// matching credential across the boundary and the script then refused to
	// fill any of them, so a vault holding three logins for a site put three
	// passwords into the page and used none. Cleared on navigation, on choice,
	// and on refusal -- §13.3's "held only for the fill that asked".
	QList<credential> m_waiting;
	// The origin the waiting entries were fetched for. A choice that arrives
	// after the page has moved is answered with nothing rather than with a
	// password meant for somewhere else.
	QString m_waiting_origin;
	// That a question was outstanding when the page moved -- **not** the
	// answers to it, which are gone by then. Two rules meet here and the suite
	// found the seam: credentials must not survive a navigation, and a user who
	// clicked something deserves to be told why nothing happened. Dropping the
	// passwords satisfies the first; one bool satisfies the second.
	bool m_choice_went_stale = false;
};
