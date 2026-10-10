#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

class QSettings;

// **How often a model's answers fail their checks, per task**, so the browser
// can say which tasks a model is too weak for. Asked for by the holder on
// 2026-10-10: "we need to signal if the model is too weak and what tasks fall
// outside its comfort zone."
//
// Every checked answer a model gives is recorded against its task -- "kind",
// "artist", "title", "tracklist", "answer" for a reply with no answer in it --
// as kept or rejected. A task becomes **weak** once it has at least
// `k_min_answers` answers and `k_weak_share` or more of them were rejected:
// enough to be a pattern rather than one bad day, and a share a person would
// notice. The counts are per model, since swapping the model is the remedy
// the signal points at, and they survive restarts in the settings.
//
// What this cannot see is an answer that passes its check and is still
// wrong -- a real name from the metadata, the wrong one. The corpus scoring
// in `test/media_corpus.h` measures that, against labels, for a model
// somebody runs it on.
class model_tally {
public:
	static constexpr int    k_min_answers = 5;
	static constexpr double k_weak_share  = 0.3;

	void record(const QString &model, const QString &task, bool kept);

	struct counts {
		int answered = 0;
		int rejected = 0;
	};
	counts of(const QString &model, const QString &task) const;

	// The tasks this model is weak at, worst first.
	QStringList weak_tasks(const QString &model) const;
	// One sentence per weak task, for the person: which, and the numbers.
	QStringList warnings(const QString &model) const;

	void load(QSettings &s);
	void save(QSettings &s) const;

private:
	QHash<QString, QHash<QString, counts>> m_by_model;
};
