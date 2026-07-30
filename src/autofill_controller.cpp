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
	if (m_bridge)
		connect(m_bridge, &keepass_bridge::logins, this, &autofill_controller::on_logins);
}

void autofill_controller::set_page_origin(const QString &origin) {
	m_origin  = origin;
	m_pending = 0;   // a navigation abandons any fill in flight
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
	const QString why = blocked_reason(origin);
	if (!why.isEmpty()) {
		emit refused(why);
		return;
	}
	m_pending = m_next_tag++;
	m_bridge->request_logins(m_origin, m_pending);
}

void autofill_controller::on_logins(int tag, const QList<credential> &entries) {
	if (tag != m_pending || m_pending == 0)
		return;   // a stale reply, e.g. from before a navigation
	m_pending = 0;

	QJsonArray arr;
	for (const credential &c : entries) {
		QJsonObject o;
		o.insert("name", c.name);
		o.insert("login", c.login);
		o.insert("password", c.password);
		arr.append(o);
	}
	emit credentials_ready(QString::fromUtf8(
		QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}
