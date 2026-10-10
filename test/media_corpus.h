// The labelled corpus in `test/fixture/media/`, and scoring a reading of it
// task by task. Shared by `test_media`, which scores the rules offline, and
// `try_media_model`, which scores a real model -- the measurement the
// holder's "signal if the model is too weak" rests on, since a check can
// catch an invented name but not a real name picked wrongly.
#pragma once

#include "media_evidence.h"
#include "media_interpretation.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

struct corpus_case {
	QString        name;
	media_evidence evidence;
	QStringList    kind, artist, title, tracklist;
	bool           is_set = false;
	QString        why;
};

inline QList<corpus_case> load_corpus(const QString &dir) {
	QList<corpus_case> out;
	QFile lf(QDir(dir).filePath("labels.json"));
	if (!lf.open(QIODevice::ReadOnly))
		return out;
	const QJsonObject labels = QJsonDocument::fromJson(lf.readAll()).object();
	const auto list = [](const QJsonValue &v) {
		QStringList l;
		for (const QJsonValue &s : v.toArray())
			l << s.toString();
		return l;
	};
	for (auto it = labels.begin(); it != labels.end(); ++it) {
		if (it.key().startsWith('_'))
			continue;
		QFile f(QDir(dir).filePath(it.key() + ".json"));
		if (!f.open(QIODevice::ReadOnly))
			continue;
		const QJsonObject l = it.value().toObject();
		corpus_case c;
		c.name      = it.key();
		c.evidence  = media_evidence::from_json(f.readAll());
		c.kind      = list(l.value("kind"));
		c.artist    = list(l.value("artist"));
		c.title     = list(l.value("title"));
		c.tracklist = list(l.value("tracklist"));
		c.is_set    = l.value("is_set").toBool();
		c.why       = l.value("why").toString();
		out << c;
	}
	return out;
}

// Right or wrong on each task, against the answers the label accepts. Case
// and runs of whitespace aside, since those are not what a person would call
// a different name.
inline QMap<QString, bool> score_case(const media_reading &r, const corpus_case &c) {
	const auto same = [](const QString &a, const QStringList &accepted) {
		for (const QString &b : accepted)
			if (a.simplified().toCaseFolded() == b.simplified().toCaseFolded())
				return true;
		return false;
	};
	QMap<QString, bool> out;
	out["kind"]      = same(media_reading::name_of(r.what), c.kind);
	out["artist"]    = same(r.artist, c.artist);
	out["title"]     = same(r.title, c.title);
	out["is_set"]    = r.is_set == c.is_set;
	out["tracklist"] = same(r.tracklist, c.tracklist);
	return out;
}

// Per task, how many of the corpus were right.
struct corpus_score {
	QMap<QString, int> right;
	int total = 0;
	double share(const QString &task) const {
		return total ? double(right.value(task)) / total : 0;
	}
	// Under three quarters right is outside the comfort zone: wrong often
	// enough that a person would stop trusting it.
	static constexpr double k_comfort = 0.75;
	QStringList outside() const {
		QStringList out;
		for (auto it = right.cbegin(); it != right.cend(); ++it)
			if (share(it.key()) < k_comfort)
				out << it.key();
		return out;
	}
};
