#include "autofill_controller.h"
#include "keepass_bridge.h"
#include "policy_engine.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

int autofill_controller::s_next_tag = 1;

QByteArray autofill_controller::secret_digest(const QString &login,
                                              const QString &password) const {
	static const QByteArray key = [] {
		QByteArray k(32, '\0');
		QRandomGenerator::system()->generate(k.begin(), k.end());
		return k;
	}();
	return QMessageAuthenticationCode::hash(
	  login.toUtf8() + '\0' + password.toUtf8(), key, QCryptographicHash::Sha256);
}

autofill_controller::autofill_controller(keepass_bridge *bridge, policy_engine *policy,
                                          QObject *parent)
  : QObject(parent), m_bridge(bridge), m_policy(policy) {
	if (m_bridge) {
		connect(m_bridge, &keepass_bridge::logins, this, &autofill_controller::on_logins);
		connect(m_bridge, &keepass_bridge::logins_failed, this,
		        [this](int tag, const QString &reason) {
			if (tag != m_pending || m_pending == 0)
				return;
			m_pending = 0;
			emit refused(reason);
		});
		connect(m_bridge, &keepass_bridge::login_saved, this,
		        [this](int tag, bool ok, const QString &message) {
			if (tag != m_save_tag || m_save_tag == 0)
				return;
			m_save_tag = 0;
			if (ok && !m_saving_secret.isEmpty())
				m_known_secrets.insert(m_saving_secret);
			m_saving_secret.clear();
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
	// And what the vault was known to hold, which was known about the page
	// that has gone. Keeping it would offer to update an entry belonging to
	// somewhere else the moment a login happens to share a username.
	m_known.clear();
	m_known_secrets.clear();
	m_known_origin.clear();
}

QString autofill_controller::blocked_reason(const QString &origin) const {
	// Origin gate first: a page asking about anything other than where it
	// actually is has no legitimate reason to (sec 13.3).
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
	m_identifier_only = false;
	m_pending = s_next_tag++;
	m_bridge->request_logins(m_origin, m_pending);
}

void autofill_controller::request_identifier(const QString &origin) {
	request_credentials(origin);
	// After, because request_credentials clears it; only a request that went
	// out is marked.
	if (m_pending)
		m_identifier_only = true;
}

void autofill_controller::note_login(const QString &origin, const QString &login) {
	if (origin.isEmpty() || origin != m_origin || login.isEmpty() || login.size() > 512)
		return;
	m_step_login  = login;
	m_step_origin = origin;
	m_step_at     = QDateTime::currentMSecsSinceEpoch();
}

bool autofill_controller::step_applies() const {
	return !m_step_login.isEmpty() && m_step_origin == m_origin &&
	       QDateTime::currentMSecsSinceEpoch() - m_step_at < 10 * 60 * 1000;
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
	// **One question at a time.** A submit button in a form reports twice --
	// the script watches the click and the submit both, since either can be
	// the only one a page fires -- and the prompt is modal, so the second
	// offer arrives inside the first prompt's loop and opened a second prompt
	// over it. Every ordinary login asked twice. The open prompt's answer is
	// the answer; a different login arriving meanwhile waits for the next
	// submit rather than replacing what the person is reading.
	if (!m_offered.password.isEmpty())
		return;
	// A step-two page has a password and no username field; whose password
	// it is was said on step one.
	const QString who = login.isEmpty() && step_applies() ? m_step_login : login;
	// Nothing new: the vault already has exactly this for this site.
	if (m_known_origin == m_origin &&
	    m_known_secrets.contains(secret_digest(who, password)))
		return;

	m_offered        = credential{ QString(), who, password, QString() };
	m_offered_origin = m_origin;
	emit save_offered(who, QUrl(m_origin).host());
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
	// An update where the vault already holds this login for this site, an add
	// otherwise. `uuid_for_login` answers empty for anything it cannot place,
	// which is the old behaviour -- so the only case that moves is the one
	// that was quietly duplicating entries.
	const QString uuid = m_known_origin == origin
	                       ? keepass_protocol::uuid_for_login(m_known, offered.login)
	                       : QString();
	m_save_tag = s_next_tag++;
	m_saving_secret = secret_digest(offered.login, offered.password);
	m_bridge->save_login(origin, offered.login, offered.password, uuid, m_save_tag);
}

void autofill_controller::request_generated_password(const QString &origin) {
	const QString why = blocked_reason(origin);
	if (!why.isEmpty()) {
		emit refused(why);
		return;
	}
	if (!m_bridge)
		return;
	m_generate_tag = s_next_tag++;
	m_bridge->generate_password(m_generate_tag);
}

void autofill_controller::offer_for_test(const QList<credential> &entries) {
	m_pending = s_next_tag++;
	on_logins(m_pending, entries);
}

// One entry, on its way to the page. The only path that puts a password across
// the boundary, so it is the only place that builds the payload.
void autofill_controller::deliver(const credential &c) {
	QJsonObject o;
	o.insert("name", c.name);
	o.insert("login", c.login);
	// An identifier-only page is given the login alone (sec 13.3: a
	// password goes only where it is going to be filled).
	o.insert("password", m_identifier_only ? QString() : c.password);
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

	// **What the vault holds for this site, so a later save can update rather
	// than add.** Kept with the passwords stripped: a uuid is an opaque entry
	// id and not a secret, and sec 13.3's rule is about credentials. Taken
	// here rather than at the single-or-many split, because a site with one
	// stored login is exactly the site whose password gets changed.
	m_known.clear();
	m_known_secrets.clear();
	m_known_origin = m_origin;
	for (const credential &c : entries) {
		if (!c.password.isEmpty())
			m_known_secrets.insert(secret_digest(c.login, c.password));
		if (c.uuid.isEmpty())
			continue;
		m_known.push_back(credential{ c.name, c.login, QString(), c.uuid });
	}

	if (entries.isEmpty()) {
		// Said rather than left silent: "nothing stored for this site" and
		// "KeePassXC is not answering" look identical from the page, and the
		// key icon is where the difference belongs.
		emit refused("No credentials stored for this site.");
		return;
	}

	// Step two of a multi-step sign-in, where step one already said who:
	// that login's entry, without asking again -- and **not somebody else's
	// when the vault has none for it.** Filling the site's only entry there
	// would put another account's password under the login just typed.
	if (step_applies() && !m_identifier_only) {
		const credential *only = nullptr;
		int matches = 0;
		for (const credential &c : entries)
			if (c.login == m_step_login) { only = &c; ++matches; }
		if (matches == 1) {
			deliver(*only);
			return;
		}
		if (matches == 0) {
			emit refused(QString("Nothing is stored for %1 on this site.")
			               .arg(m_step_login));
			return;
		}
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
	// credentials delivered for a fill that never happened. sec 13.3 says they are
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
