// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings_bundle.h"
#include "filter_list.h"
#include "policy.h"
#include "policy_engine.h"

#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStringList>
#include <QVariant>

namespace settings_bundle {

namespace {

const char *k_group_head    = "hydra";
const char *k_group_defaults = "defaults";
const char *k_group_sites    = "sites";
const char *k_group_filters  = "filters";
const char *k_group_prefs    = "preferences";

// The application's own settings, opened the same way the dialog opens them:
// by explicit scope rather than by application metadata, so where they live
// does not move if the app or organisation name is ever edited.
QSettings app_settings() {
	return QSettings(QSettings::IniFormat, QSettings::UserScope, "hydra", "hydra");
}

// The line encoding and the words for a setting live in `policy`, so this file
// and the policy file cannot drift apart about what "javascript:block" means.

}  // namespace

int current_format() { return 1; }

QString summary::describe() const {
	if (!ok())
		return error;
	QStringList bits;
	if (defaults)    bits << QString("%1 default%2").arg(defaults).arg(defaults == 1 ? "" : "s");
	if (sites)       bits << QString("%1 site exception%2").arg(sites).arg(sites == 1 ? "" : "s");
	if (filters)     bits << QString("%1 filter rule%2").arg(filters).arg(filters == 1 ? "" : "s");
	if (preferences) bits << QString("%1 preference%2").arg(preferences).arg(preferences == 1 ? "" : "s");
	return bits.isEmpty() ? QStringLiteral("nothing") : bits.join(", ");
}

summary write(const QString &path, const policy_engine *policy_in,
	             const filter_list *filters) {
	summary out;
	if (path.isEmpty()) {
		out.error = "No file name given.";
		return out;
	}

	// A fresh file rather than a merge into whatever was there: an export is a
	// picture of this machine now, and half of an older one underneath it would
	// be a picture of nothing.
	QFile::remove(path);
	QSettings f(path, QSettings::IniFormat);

	f.beginGroup(k_group_head);
	f.setValue("format", current_format());
	f.setValue("program", "Hydra");
	f.endGroup();

	if (policy_in) {
		f.beginGroup(k_group_defaults);
		for (int i = 0; i < policy::feature_count(); ++i) {
			const auto feat = static_cast<policy::feature>(i);
			f.setValue(policy::feature_name(feat),
				          policy::setting_word(policy_in->global_default(feat)));
			++out.defaults;
		}
		f.endGroup();

		f.beginGroup(k_group_sites);
		for (const policy_engine::rule &r : policy_in->rules()) {
			const QString line = policy::settings_to_line(r.bits);
			// A rule that says nothing is not an exception; see the settings
			// dialog, which does not list them either.
			if (line.isEmpty() || r.pattern.isEmpty())
				continue;
			f.setValue(r.pattern, line);
			++out.sites;
		}
		f.endGroup();
	}

	if (filters) {
		f.beginWriteArray(k_group_filters);
		int n = 0;
		for (const filter_rule &r : filters->rules()) {
			if (r.text.trimmed().isEmpty())
				continue;
			f.setArrayIndex(n++);
			f.setValue("rule", r.text);
			if (!r.note.isEmpty())
				f.setValue("note", r.note);
		}
		f.endArray();
		out.filters = n;
	}

	// Everything the settings dialog stores, verbatim and by key, so a
	// preference added later travels without this file being taught about it.
	QSettings app = app_settings();
	f.beginGroup(k_group_prefs);
	for (const QString &key : app.allKeys()) {
		f.setValue(key, app.value(key));
		++out.preferences;
	}
	f.endGroup();

	f.sync();
	if (f.status() != QSettings::NoError || !QFileInfo::exists(path))
		out.error = "Could not write " + path;
	return out;
}

summary read(const QString &path, policy_engine *policy_out, filter_list *filters) {
	summary out;
	if (!QFileInfo::exists(path)) {
		out.error = "No such file: " + path;
		return out;
	}

	QSettings f(path, QSettings::IniFormat);
	if (f.status() != QSettings::NoError) {
		out.error = "That file is not readable as settings.";
		return out;
	}

	const int format = f.value(QString(k_group_head) + "/format", 0).toInt();
	if (format == 0) {
		// Refusing an unmarked file is the point: any INI would otherwise be
		// accepted and silently apply nothing, which looks identical to a
		// successful import of an empty backup.
		out.error = "That does not look like a Hydra settings file.";
		return out;
	}
	if (format > current_format()) {
		out.error = QString("That file is from a newer version (format %1, this "
			                   "build reads %2).")
			              .arg(format)
			              .arg(current_format());
		return out;
	}

	if (policy_out) {
		f.beginGroup(k_group_defaults);
		for (const QString &key : f.allKeys()) {
			const policy::feature feat = policy::feature_from_name(key);
			const policy::setting want = policy::setting_from_word(f.value(key).toString());
			// A global default is always allow or block; "default" there would
			// point at itself, so an unreadable word is skipped rather than
			// guessed at.
			if (feat == policy::feature::count || want == policy::setting::unset)
				continue;
			policy_out->set_global_default(feat, want);
			++out.defaults;
		}
		f.endGroup();

		f.beginGroup(k_group_sites);
		for (const QString &pattern : f.allKeys()) {
			// A comma in an INI value means "list" to QSettings. It quotes what
			// *this* writes, so our own files round-trip -- but a person editing
			// the file by hand will not quote anything, and the whole reason for
			// choosing a readable format is that they will edit it. Taking the
			// value as a list and rejoining reads both spellings.
			const QVariant raw = f.value(pattern);
			const QString joined = raw.typeId() == QMetaType::QStringList
				                         ? raw.toStringList().join(',')
				                         : raw.toString();
			const QStringList parts = joined.split(',', Qt::SkipEmptyParts);
			int applied = 0;
			for (const QString &part : parts) {
				const QStringList kv = part.split(':');
				if (kv.size() != 2)
					continue;
				const policy::feature feat =
					policy::feature_from_name(kv[0].trimmed());
				const policy::setting want = policy::setting_from_word(kv[1]);
				if (feat == policy::feature::count)
					continue;
				policy_out->set_setting(pattern, feat, want);
				++applied;
			}
			if (applied)
				++out.sites;
		}
		f.endGroup();
	}

	if (filters) {
		const int n = f.beginReadArray(k_group_filters);
		for (int i = 0; i < n; ++i) {
			f.setArrayIndex(i);
			filter_rule r;
			if (!filter_list::parse_rule(f.value("rule").toString(), &r))
				continue;
			r.note = f.value("note").toString();
			// `add` already refuses a duplicate, which is what makes reading a
			// backup twice harmless.
			if (!filters->contains(r.text)) {
				filters->add(r);
				++out.filters;
			}
		}
		f.endArray();
	}

	QSettings app = app_settings();
	f.beginGroup(k_group_prefs);
	for (const QString &key : f.allKeys()) {
		app.setValue(key, f.value(key));
		++out.preferences;
	}
	f.endGroup();
	app.sync();

	return out;
}

}  // namespace settings_bundle
