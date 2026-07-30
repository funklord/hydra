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

signals:
	// Delivered back to the page: a JSON array of {name, login, password}.
	void credentials_ready(const QString &json);
	// A human-readable refusal, for the key icon's tooltip.
	void refused(const QString &reason);

private:
	void on_logins(int tag, const QList<credential> &entries);

	keepass_bridge *m_bridge = nullptr;
	policy_engine  *m_policy = nullptr;
	QString m_origin;
	bool    m_https_only = true;
	int     m_next_tag = 1;
	int     m_pending  = 0;
};
