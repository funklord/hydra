#include "tab_investigation.h"

#include "ad_probe.h"
#include "cosmetic_filters.h"
#include "request_filter.h"
#include "web_view_backend.h"

#include <QTimer>

#include <memory>

namespace {

const char *k_trial_script = "hydra-trial-scriptlets";

}  // namespace

tab_investigation::tab_investigation(web_view_backend *view,
                                     request_filter *filter,
                                     const QString &site_host, QObject *parent)
  : QObject(parent), m_view(view), m_filter(filter), m_site(site_host) {
	// The bridge the shell gave this tab when it made it.
	if (view)
		m_cosmetic = view->findChild<cosmetic_filters *>();
}

tab_investigation::~tab_investigation() {
	clear(m_in_trial);
}

void tab_investigation::look(std::function<void(const QStringList &)> done) {
	if (!m_view) {
		done(QStringList());
		return;
	}
	m_view->run_probe(ad_probe::source(), [done](const QString &json) {
		ad_probe::findings f;
		ad_probe::parse(json, &f);
		done(ad_probe::describe(f));
	});
}

void tab_investigation::trial(const QList<filter_rule> &rules,
                              const QList<scriptlet_call> &calls,
                              std::function<void(const QStringList &)> done) {
	if (!m_view) {
		done(QStringList());
		return;
	}
	QList<filter_rule> cosmetic;
	for (const filter_rule &r : rules)
		if (r.cosmetic)
			cosmetic << r;
	m_cosmetic_trial.replace(cosmetic);
	if (m_cosmetic)
		m_cosmetic->set_trial_list(&m_cosmetic_trial);
	if (m_filter)
		m_filter->set_trial(m_site, rules);
	// Untrusted always: a proposed rule never carries trust, whatever list
	// the site's other rules came from.
	QList<scriptlet_call> untrusted = calls;
	for (scriptlet_call &c : untrusted)
		c.trusted = false;
	const QString src = scriptlets::source_for(untrusted);
	if (src.isEmpty())
		m_view->remove_script(QString::fromLatin1(k_trial_script));
	else
		m_view->inject_main_world_script(QString::fromLatin1(k_trial_script), src);
	m_in_trial = !rules.isEmpty() || !untrusted.isEmpty();

	// Reload, wait for the load, give the page time to settle, and look.
	// Bounded, since a page that never finishes must not stall the
	// investigation: whichever comes first, the load or the deadline, settles
	// it once and drops the connection either way.
	auto fired = std::make_shared<bool>(false);
	auto conn  = std::make_shared<QMetaObject::Connection>();
	auto settle = [this, done, fired, conn] {
		if (*fired)
			return;
		*fired = true;
		QObject::disconnect(*conn);
		QTimer::singleShot(k_settle_ms, this, [this, done] { look(done); });
	};
	*conn = connect(m_view, &web_view_backend::load_finished, this,
	                 [settle](bool) { settle(); });
	QTimer::singleShot(15000, this, settle);
	m_view->reload();
}

void tab_investigation::ask(const QString &kind, const QString &question,
                            std::function<void(const QString &)> done) {
	if (m_asker)
		m_asker(kind, question, std::move(done));
	else
		done(QStringLiteral("nobody to ask"));
}

void tab_investigation::end() {
	clear(m_in_trial);
}

void tab_investigation::clear(bool reload) {
	if (m_cosmetic)
		m_cosmetic->set_trial_list(nullptr);
	m_cosmetic_trial.replace({});
	if (m_filter)
		m_filter->set_trial(QString(), {});
	if (m_view) {
		m_view->remove_script(QString::fromLatin1(k_trial_script));
		if (reload)
			m_view->reload();
	}
	m_in_trial = false;
}
