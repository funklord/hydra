// SPDX-License-Identifier: GPL-3.0-or-later
#include "autofill_controller.h"
#include "keepass_bridge.h"
#include "policy_engine.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

autofill_controller::autofill_controller(keepass_bridge *bridge, policy_engine *policy,
                                          QObject *parent)
  : QObject(parent), m_bridge(bridge), m_policy(policy) {
	if (m_bridge) {
		connect(m_bridge, &keepass_bridge::logins, this, &autofill_controller::on_logins);
		connect(m_bridge, &keepass_bridge::login_saved, this,
		        [this](int tag, bool ok, const QString &message) {
			if (tag != m_save_tag || m_save_tag == 0)
				return;
			m_save_tag = 0;
			emit save_finished(ok, ok ? QStringLiteral("Saved to KeePassXC.")
			                          : message);
		});
		connect(m_bridge, &keepass_bridge::password_generated, this,
		        [this](int tag, const QString &password) {
			if (tag != m_generate_tag || m_generate_tag == 0)
				return;
			m_generate_tag = 0;
			if (password.isEmpty()) {
				emit refused("KeePassXC did not return a password.");
				return;
			}
			emit generated_password(password);
		});
	}
}

void autofill_controller::set_page_origin(const QString &origin) {
	m_origin  = origin;
	m_pending = 0;   // a navigation abandons any fill in flight
	// And anything waiting on a person, for the same reason: a picker still on
	// screen when the page moves is offering to fill a form that is gone. The
	// entries go; the fact that a question was open stays, so an answer that
	// arrives afterwards can be explained rather than ignored.
	if (!m_waiting.isEmpty())
		m_choice_went_stale = true;
	m_waiting.clear();
	m_waiting_origin.clear();
	// An unanswered save prompt goes with the page that raised it. Storing a
	// credential after navigating would attach it to whichever site is open
	// when the user finally clicks, which is not the one they typed it into.
	m_offered = credential{};
	m_offered_origin.clear();
}

QString autofill_controller::blocked_reason(const QString &origin) const {
	// Origin gate first: a page asking about anything other than where it
	// actually is has no legitimate reason to (§13.3).
	if (origin.isEmpty() || origin != m_origin)
		return "Origin mismatch — the page asked about a different site than "
		       "the one it is on.";

	const QUrl url(origin);
	const QString host = url.host();
	if (host.isEmpty())
		return "No usable origin.";

	if (m_https_only && url.scheme() != "https")
		return "Autofill is limited to HTTPS; this page is not secure.";

	if (m_policy && !m_policy->is_allowed(policy::feature::autofill, host))
		return "Autofill is blocked for this site by policy.";

	if (!m_bridge || !m_bridge->connected())
		return "KeePassXC is not connected.";
	if (!m_bridge->associated())
		return "Not paired with KeePassXC yet.";

	return QString();
}

void autofill_controller::request_credentials(const QString &origin) {
	// Before the gate, deliberately. A page with a login form that autofill is
	// blocked on is exactly the page where the user needs to see the affordance
	// and read the reason; announcing only the allowed ones would hide the key
	// precisely when it has something to say.
	emit requested();
	const QString why = blocked_reason(origin);
	if (!why.isEmpty()) {
		emit refused(why);
		return;
	}
	m_pending = m_next_tag++;
	m_bridge->request_logins(m_origin, m_pending);
}

void autofill_controller::offer_to_save(const QString &origin,
                                        const QString &login,
                                        const QString &password) {
	// The same gate as a fill, and for a stronger reason: a fill hands a
	// credential to a page that may already know it, while a save writes into
	// the vault. A page must not be able to put an entry under another site's
	// name.
	const QString why = blocked_reason(origin);
	if (!why.isEmpty()) {
		emit refused(why);
		return;
	}
	if (password.isEmpty())
		return;   // nothing to store; not worth a prompt

	m_offered        = credential{ QString(), login, password };
	m_offered_origin = m_origin;
	emit save_offered(login, QUrl(m_origin).host());
}

void autofill_controller::confirm_save(bool yes) {
	const credential offered = m_offered;
	const QString origin = m_offered_origin;
	// Taken and cleared first, so a second answer to the same prompt stores
	// nothing -- the same rule `choose` follows, for the same reason.
	m_offered = credential{};
	m_offered_origin.clear();

	if (!yes || offered.password.isEmpty())
		return;
	if (origin.isEmpty() || origin != m_origin) {
		emit save_finished(false, "The page changed before that was saved, so "
		                          "nothing was stored.");
		return;
	}
	if (!m_bridge) {
		emit save_finished(false, "Not connected to KeePassXC.");
		return;
	}
	m_save_tag = m_next_tag++;
	m_bridge->save_login(origin, offered.login, offered.password, QString(),
	                      m_save_tag);
}

void autofill_controller::request_generated_password(const QString &origin) {
	const QString why = blocked_reason(origin);
	if (!why.isEmpty()) {
		emit refused(why);
		return;
	}
	if (!m_bridge)
		return;
	m_generate_tag = m_next_tag++;
	m_bridge->generate_password(m_generate_tag);
}

void autofill_controller::offer_for_test(const QList<credential> &entries) {
	m_pending = m_next_tag++;
	on_logins(m_pending, entries);
}

// One entry, on its way to the page. The only path that puts a password across
// the boundary, so it is the only place that builds the payload.
void autofill_controller::deliver(const credential &c) {
	QJsonObject o;
	o.insert("name", c.name);
	o.insert("login", c.login);
	o.insert("password", c.password);
	QJsonArray arr;
	arr.append(o);
	emit credentials_ready(QString::fromUtf8(
	  QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

void autofill_controller::on_logins(int tag, const QList<credential> &entries) {
	if (tag != m_pending || m_pending == 0)
		return;   // a stale reply, e.g. from before a navigation
	m_pending = 0;
	m_waiting.clear();
	m_waiting_origin.clear();
	m_choice_went_stale = false;

	if (entries.isEmpty()) {
		// Said rather than left silent: "nothing stored for this site" and
		// "KeePassXC is not answering" look identical from the page, and the
		// key icon is where the difference belongs.
		emit refused("No credentials stored for this site.");
		return;
	}

	if (entries.size() == 1) {
		deliver(entries.first());
		return;
	}

	// **More than one, so a person decides -- and the passwords stay here.**
	// This used to hand the page every match and let the injected script sort
	// it out, and the script's answer to more than one was to fill nothing. So
	// a vault with three logins for a site sent three passwords across the
	// boundary and used none of them, which is the worst of both: no fill, and
	// credentials delivered for a fill that never happened. §13.3 says they are
	// held only for the fill that asked.
	m_waiting        = entries;
	m_waiting_origin = m_origin;
	QStringList labels;
	for (const credential &c : entries) {
		// What tells two accounts apart, and nothing else. Never the password:
		// the picker is a window, and a window is a thing people photograph,
		// screen-share and leave open.
		labels << (c.name.isEmpty() ? c.login
		                            : QString("%1 — %2").arg(c.login, c.name));
	}
	emit choice_needed(labels);
}

void autofill_controller::choose(int index) {
	const QList<credential> waiting = m_waiting;
	const QString origin = m_waiting_origin;
	// Taken and cleared first, so no path can answer the same prompt twice --
	// a double-click on the picker, or a second dialog raised while the first
	// was still up, must not fill twice.
	m_waiting.clear();
	m_waiting_origin.clear();

	if (waiting.isEmpty() && m_choice_went_stale) {
		m_choice_went_stale = false;
		emit refused("The page changed while you were choosing, so nothing was "
		             "filled.");
		return;
	}
	if (index < 0 || index >= waiting.size())
		return;   // dismissed, which is a legitimate answer and fills nothing
	// The page may have navigated while the picker was open. Filling then would
	// put one site's password into another's form, which is the exact failure
	// the origin gate exists to prevent -- and the gate cannot see this one,
	// because the request that fetched these entries passed it at the time.
	if (origin.isEmpty() || origin != m_origin) {
		emit refused("The page changed while you were choosing, so nothing was "
		             "filled.");
		return;
	}
	deliver(waiting.at(index));
}
