// SPDX-License-Identifier: GPL-3.0-or-later
#include "policy_engine.h"

#include <QFile>
#include <QStringList>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

using policy::feature;
using policy::setting;

namespace {

const char *setting_name(setting s) {
	switch (s) {
		case setting::allow: return "allow";
		case setting::block: return "block";
		default:             return "default";
	}
}

setting setting_from_name(const QString &n) {
	if (n == "allow") return setting::allow;
	if (n == "block") return setting::block;
	return setting::unset;
}

}  // namespace

policy_engine::policy_engine(QObject *parent) : QObject(parent) {
	// Sensible privacy-leaning defaults (architecture doc §7.2). Flip
	// JavaScript to block here to run in per-site "whitelist mode".
	set_global_default(feature::javascript,          setting::allow);
	set_global_default(feature::cookies,             setting::allow);
	set_global_default(feature::third_party_cookies, setting::block);
	set_global_default(feature::ads,                 setting::block);
	set_global_default(feature::popups,              setting::block);
	set_global_default(feature::images,              setting::allow);
	set_global_default(feature::autoplay,            setting::block);
	set_global_default(feature::geolocation,         setting::block);
	set_global_default(feature::camera,              setting::block);
	set_global_default(feature::microphone,          setting::block);
	set_global_default(feature::notifications,       setting::block);
	set_global_default(feature::referer,             setting::allow);
	// Autofill defaults on; the per-site tri-state and the strict origin gate
	// are what actually govern it (§13.3).
	set_global_default(feature::autofill,            setting::allow);
}

QString policy_engine::etld_plus_one(const QString &host) {
	const QStringList labels = host.split('.', Qt::SkipEmptyParts);
	if (labels.size() <= 2)
		return host;
	return labels.mid(labels.size() - 2).join('.');
}

bool policy_engine::match_pattern(const QString &pattern, const QString &host, int &specificity) {
	if (pattern == "*") {
		specificity = 0;
		return true;
	}
	if (pattern.startsWith("*.")) {
		const QString dom = pattern.mid(2);
		if (host == dom || host.endsWith("." + dom)) {
			specificity = dom.count('.') + 1;   // more labels = more specific
			return true;
		}
		return false;
	}
	if (pattern == host) {
		specificity = host.count('.') + 100;    // exact beats any wildcard
		return true;
	}
	return false;
}

policy_engine::rule *policy_engine::find_rule(const QString &pattern) {
	for (rule &r : m_rules)
		if (r.pattern == pattern)
			return &r;
	return nullptr;
}

const policy_engine::rule *policy_engine::find_rule(const QString &pattern) const {
	for (const rule &r : m_rules)
		if (r.pattern == pattern)
			return &r;
	return nullptr;
}

setting policy_engine::effective_setting(feature f, const QString &host) const {
	setting best = setting::unset;
	int best_spec = -1;
	for (const rule &r : m_rules) {
		const setting s = policy::get_setting(r.bits, f);
		if (s == setting::unset)
			continue;
		int spec = -1;
		if (match_pattern(r.pattern, host, spec) && spec > best_spec) {
			best_spec = spec;
			best = s;
		}
	}
	if (best != setting::unset)
		return best;
	return global_default(f);
}

bool policy_engine::is_allowed(feature f, const QString &host) const {
	return effective_setting(f, host) != setting::block;
}

setting policy_engine::global_default(feature f) const {
	const setting s = policy::get_setting(m_global_defaults, f);
	return (s == setting::block) ? setting::block : setting::allow;
}

void policy_engine::set_global_default(feature f, setting s) {
	const setting norm = (s == setting::block) ? setting::block : setting::allow;
	m_global_defaults = policy::with_setting(m_global_defaults, f, norm);
	emit changed();
}

setting policy_engine::setting_for(const QString &pattern, feature f) const {
	const rule *r = find_rule(pattern);
	return r ? policy::get_setting(r->bits, f) : setting::unset;
}

void policy_engine::set_setting(const QString &pattern, feature f, setting s) {
	rule *r = find_rule(pattern);
	if (!r) {
		m_rules.push_back({pattern, 0});
		r = &m_rules.last();
	}
	r->bits = policy::with_setting(r->bits, f, s);
	emit changed();
}

bool policy_engine::load(const QString &path) {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return false;
	const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
	if (!doc.isObject())
		return false;
	const QJsonObject root = doc.object();

	const QJsonObject gd = root.value("globalDefaults").toObject();
	for (auto it = gd.begin(); it != gd.end(); ++it) {
		const feature f = policy::feature_from_name(it.key());
		if (f != feature::count)
			set_global_default(f, setting_from_name(it.value().toString()));
	}

	m_rules.clear();
	const QJsonArray arr = root.value("rules").toArray();
	for (const QJsonValue &v : arr) {
		const QJsonObject o = v.toObject();
		rule r;
		r.pattern = o.value("pattern").toString();
		if (r.pattern.isEmpty())
			continue;
		const QJsonObject settings = o.value("settings").toObject();
		for (auto it = settings.begin(); it != settings.end(); ++it) {
			const feature f = policy::feature_from_name(it.key());
			if (f != feature::count)
				r.bits = policy::with_setting(r.bits, f, setting_from_name(it.value().toString()));
		}
		m_rules.push_back(r);
	}
	emit changed();
	return true;
}

bool policy_engine::save(const QString &path) const {
	QJsonObject gd;
	for (int i = 0; i < policy::feature_count(); ++i) {
		const feature f = static_cast<feature>(i);
		gd.insert(policy::feature_name(f), setting_name(global_default(f)));
	}

	QJsonArray arr;
	for (const rule &r : m_rules) {
		QJsonObject settings;
		for (int i = 0; i < policy::feature_count(); ++i) {
			const feature f = static_cast<feature>(i);
			const setting s = policy::get_setting(r.bits, f);
			if (s != setting::unset)
				settings.insert(policy::feature_name(f), setting_name(s));
		}
		if (settings.isEmpty())
			continue;  // don't persist empty rules
		QJsonObject o;
		o.insert("pattern", r.pattern);
		o.insert("settings", settings);
		arr.append(o);
	}

	QJsonObject root;
	root.insert("globalDefaults", gd);
	root.insert("rules", arr);

	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return false;
	file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	return true;
}
