#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QString>
#include <QtGlobal>

// Per-tab zoom, persisted so a page a person zoomed comes back zoomed after a
// restart -- which it did not, because the map lived only in memory.
//
// Keyed by node id, the same key the runtime map uses, so the on-disk form is a
// plain object of {id: factor}. **1.0 is the absence of a setting, not a
// setting** -- the runtime map already drops it (a stored 100% would grow an
// entry for every tab ever looked at), and these guard it again so a
// hand-edited or older file cannot reintroduce it. Nothing else about a tab is
// here: the tree file owns what a tab *is*, and this owns only how it is drawn.
namespace zoom_store {

inline QByteArray to_json(const QHash<QString, double> &zoom) {
	QJsonObject o;
	for (auto it = zoom.constBegin(); it != zoom.constEnd(); ++it)
		if (it.value() > 0.0 && !qFuzzyCompare(it.value(), 1.0))
			o.insert(it.key(), it.value());
	return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

// `ok` separates an empty map from a file that did not parse, which are the
// same VALUE and must not be the same outcome: a store read as empty is
// overwritten by the first save, so a zoom.json nobody can parse costs every
// zoom in it. The caller hands the answer to `keep_or_disown`, which stops
// writing to the file rather than replacing it.
inline QHash<QString, double> from_json(const QByteArray &bytes,
                                         bool *ok = nullptr) {
	QHash<QString, double> zoom;
	QJsonParseError err{};
	const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
	if (ok)
		*ok = err.error == QJsonParseError::NoError && doc.isObject();
	const QJsonObject o = doc.object();
	for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
		const double f = it.value().toDouble();
		if (f > 0.0 && !qFuzzyCompare(f, 1.0))
			zoom.insert(it.key(), f);
	}
	return zoom;
}

}  // namespace zoom_store
