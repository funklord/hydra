#include "policy_engine.h"

#include <QFile>
#include <QVariant>
#include <QSettings>
#include <QFileInfo>
#include <QStringList>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

using policy::feature;
using policy::setting;

namespace {

// The words for a setting live in `policy` now, shared with the settings
// bundle so the two files cannot disagree about what "block" is called.
//
// **This was a second copy of that mapping, sitting under that comment.** It
// knew "allow" and "block" and nothing else, so when `ask` was added the two
// disagreed immediately and silently: a stored `ask` read back as `unset`,
// which now means block. Delegating removes the copy rather than teaching it
// the third word, because teaching it would leave the fourth to be found later.

}  // namespace

policy_engine::policy_engine(QObject *parent) : QObject(parent) {
	// Sensible privacy-leaning defaults (architecture doc sec 7.2). Flip
	// JavaScript to block here to run in per-site "whitelist mode".
	set_global_default(feature::javascript,          setting::allow);
	set_global_default(feature::cookies,             setting::allow);
	set_global_default(feature::third_party_cookies, setting::block);
	set_global_default(feature::ads,                 setting::block);
	set_global_default(feature::popups,              setting::block);
	set_global_default(feature::images,              setting::allow);
	set_global_default(feature::autoplay,            setting::block);
	// **Ask, where the capability is real and a prompt can reach somebody.**
	// These were `block`, which is the right thing to do in the absence of an
	// answer and was never meant to be the answer. A blocked camera reaches the
	// page as a `NotAllowedError` and reaches the person as nothing at all, so
	// a video call that Hydra deliberately stopped is indistinguishable from
	// one that is simply broken -- which is exactly how it presented here, and
	// cost a diagnosis before the cause was found to be this line.
	//
	// Privacy-leaning is still the default and this does not weaken it: nothing
	// is granted, the question is merely put to the person it belongs to, once
	// per site, refused by default if the dialog is dismissed.
	set_global_default(feature::geolocation,         setting::ask);
	set_global_default(feature::camera,              setting::ask);
	set_global_default(feature::microphone,          setting::ask);
	// Both were denied by a `default:` arm in the engine backend before they
	// were features at all, so blocking here changed nothing and only made
	// the refusal a decision somebody can see and overrule.
	//
	// **Clipboard reading stays blocked, and not out of caution.** The engine
	// gates it behind `JavascriptCanAccessClipboard` and `JavascriptCanPaste`,
	// neither of which this project enables, so the permission request never
	// arrives -- `ask` here would be a setting offering a prompt that cannot
	// fire. Blocked says the true thing. When those engine settings are turned
	// on this becomes the wrong default and should move with them.
	set_global_default(feature::clipboard_read,      setting::block);
	// Pointer lock asks. It is a real capability the engine does deliver, it is
	// always the result of a deliberate click, and Escape takes it back -- so
	// one prompt per site, remembered, is the whole cost.
	//
	// The plan for this was `allow` plus a transient "press Escape" notice,
	// which is what other browsers do and is better. It is not what shipped
	// here, because the notice is UI that does not exist yet and `allow`
	// without it is a silent grant -- the precise failure this whole change
	// exists to remove, pointed the other way. `ask` until there is something
	// to show.
	set_global_default(feature::pointer_lock,        setting::ask);
	// Asks, like the camera, and for a stronger version of the same reason: a
	// shared screen carries whatever else is on it -- other windows, other
	// people's messages, a password manager left open. Answering it in advance
	// for every site is not something anybody can sensibly do.
	set_global_default(feature::screen_share,        setting::ask);
	// **Notifications are blocked here and raised by whoever can deliver them.**
	//
	// Chromium treats a missing notification presenter as success: the page's
	// notification resolves and goes nowhere. So the honest default depends on
	// something this class cannot see, and must not guess -- the desktop build
	// installs `qtwebengine_notifications` against the session bus and lifts
	// this to `ask` in `main()` when that succeeds, while the Android build has
	// no presenter and leaves it exactly here.
	//
	// Blocked is therefore the floor rather than the answer, and it is the right
	// floor: prompting somebody to grant a capability the browser cannot honour
	// is worse than refusing it, because the refusal is at least true.
	set_global_default(feature::notifications,       setting::block);
	// Allow: the media badge is a headline feature and a browser that noticed
	// nothing until told to would be the wrong default entirely.
	set_global_default(feature::media_detect,        setting::allow);
	// Block means "answer it and get it out of the way", which is what almost
	// everyone wants from a consent banner and is the whole point of the option.
	set_global_default(feature::cookie_notices,      setting::block);
	set_global_default(feature::referer,             setting::allow);
	// Autofill defaults on; the per-site tri-state and the strict origin gate
	// are what actually govern it (sec 13.3).
	set_global_default(feature::autofill,            setting::allow);
	// The helper tier is off until a site is explicitly trusted with it: it is
	// the only feature here that lets generated code cause a request. The DOM
	// half of sec 11.5.1 has no default here because it has no permission -- it is
	// designed and unbuilt, and a default for a capability that does not exist
	// is a setting nothing can honour.
	set_global_default(feature::extractor_fetch,     setting::block);
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

// **Only `allow` allows, which is a change and the point of it.** This read
// `!= block`, so any state that was not a refusal was a grant -- and with
// `ask` in the vocabulary that would have made "consult the person" mean
// "yes" for every caller that cannot consult anybody. Most callers cannot:
// the request filter answering a load has no user in front of it.
//
// So this is the question for code that must decide now, and it fails closed.
// Code that *can* ask calls `effective_setting` and handles `ask` itself.
bool policy_engine::is_allowed(feature f, const QString &host) const {
	return effective_setting(f, host) == setting::allow;
}

// **Unset fails closed.** It used to answer `allow`, which never bit because
// every feature is given an explicit default in the constructor -- but that is
// a property of one function agreeing with another, not a guarantee. A feature
// added without a constructor line would have been granted to every site
// silently, and the failure would look like the feature working.
setting policy_engine::global_default(feature f) const {
	const setting s = policy::get_setting(m_global_defaults, f);
	return s == setting::unset ? setting::block : s;
}

// Stores what it is given. The normalising this used to do -- anything not
// `block` becomes `allow` -- would have thrown `ask` away on the way in, and
// silently: the setter would accept it and the getter would answer `allow`.
void policy_engine::set_global_default(feature f, setting s) {
	m_global_defaults = policy::with_setting(m_global_defaults, f, s);
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

bool policy_engine::load_json(const QString &path) {
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
			set_global_default(f, policy::setting_from_word(it.value().toString()));
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
				r.bits = policy::with_setting(r.bits, f, policy::setting_from_word(it.value().toString()));
		}
		m_rules.push_back(r);
	}
	emit changed();
	return true;
}


bool policy_engine::load(const QString &path) {
	// INI first, and the old JSON if that is what is there.
	//
	// The file moved from JSON to INI because everything in it is a value or a
	// line of them, and a key=value file can be read and repaired by a person
	// with no tools. The migration is not a separate step anybody has to run:
	// a JSON file at the old path is read once, and the next save writes INI.
	// Losing somebody's site rules to a format change would be the worst
	// possible way to make a file more readable.
	if (QFileInfo::exists(path)) {
		QSettings f(path, QSettings::IniFormat);
		if (f.status() == QSettings::NoError &&
		    f.value("hydra/kind").toString() == "policy") {
			m_rules.clear();
			f.beginGroup("defaults");
			for (const QString &key : f.allKeys()) {
				const feature fe = policy::feature_from_name(key);
				const setting st = policy::setting_from_word(f.value(key).toString());
				if (fe != feature::count && st != setting::unset)
					set_global_default(fe, st);
			}
			f.endGroup();
			f.beginGroup("sites");
			for (const QString &pattern : f.allKeys()) {
				// A comma means "list" to QSettings, and a hand-edited file will
				// not be quoted; taking it as a list and rejoining reads both.
				const QVariant raw = f.value(pattern);
				const QString line = raw.typeId() == QMetaType::QStringList
				                         ? raw.toStringList().join(',')
				                         : raw.toString();
				rule r;
				r.pattern = pattern;
				r.bits    = policy::settings_from_line(line);
				if (!r.pattern.isEmpty() && r.bits != 0)
					m_rules.push_back(r);
			}
			f.endGroup();
			emit changed();
			return true;
		}
	}

	// The legacy path: same name with .json, or the given file if it is JSON.
	if (load_json(path))
		return true;
	QString legacy = path;
	if (legacy.endsWith(".ini"))
		legacy.chop(4), legacy += ".json";
	return legacy != path && load_json(legacy);
}

bool policy_engine::save(const QString &path) const {
	// **`clear()` rather than removing the file, which is the same intent
	// without the hole.** Both exist to drop keys the previous save wrote and
	// this one does not -- a rule the user deleted, or the JSON this file used
	// to be -- since `setValue` alone only adds and overwrites. The difference
	// is when the old contents stop existing: `QFile::remove` unlinks them
	// immediately, so from there until `sync()` the machine has no policy file
	// at all, and a process killed inside that window comes back with every
	// site on its defaults. `clear()` queues the same erasure inside the
	// QSettings object, and `sync()` then writes the whole result through
	// QSaveFile in one rename -- the old policy is never gone until the new
	// one is there.
	//
	// It also stops lying to the QSettings cache, which keys a shared
	// QConfFile on the path: removing the file behind its back left the
	// cached object describing a file that was not there.
	QSettings f(path, QSettings::IniFormat);
	f.clear();
	f.setValue("hydra/format", 1);
	f.setValue("hydra/kind", "policy");

	f.beginGroup("defaults");
	for (int i = 0; i < policy::feature_count(); ++i) {
		const auto fe = static_cast<feature>(i);
		f.setValue(policy::feature_name(fe),
		            policy::setting_word(global_default(fe)));
	}
	f.endGroup();

	f.beginGroup("sites");
	for (const rule &r : m_rules) {
		const QString line = policy::settings_to_line(r.bits);
		if (line.isEmpty())
			continue;   // a rule that says nothing is not written
		f.setValue(r.pattern, line);
	}
	f.endGroup();

	f.sync();
	return f.status() == QSettings::NoError;
}
