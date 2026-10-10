#include "model_tally.h"

#include <QSettings>

#include <algorithm>

void model_tally::record(const QString &model, const QString &task, bool kept) {
	if (model.isEmpty() || task.isEmpty())
		return;
	counts &c = m_by_model[model][task];
	++c.answered;
	if (!kept)
		++c.rejected;
}

model_tally::counts model_tally::of(const QString &model,
                                   const QString &task) const {
	return m_by_model.value(model).value(task);
}

QStringList model_tally::weak_tasks(const QString &model) const {
	QList<QPair<double, QString>> weak;
	const QHash<QString, counts> tasks = m_by_model.value(model);
	for (auto it = tasks.cbegin(); it != tasks.cend(); ++it) {
		const counts &c = it.value();
		if (c.answered < k_min_answers)
			continue;
		const double share = double(c.rejected) / c.answered;
		if (share >= k_weak_share)
			weak << qMakePair(share, it.key());
	}
	std::sort(weak.begin(), weak.end(),
	          [](const auto &a, const auto &b) { return a.first > b.first; });
	QStringList out;
	for (const auto &w : weak)
		out << w.second;
	return out;
}

QStringList model_tally::warnings(const QString &model) const {
	QStringList out;
	for (const QString &task : weak_tasks(model)) {
		const counts c = of(model, task);
		out << QString("%1 is unreliable at %2: %3 of its last %4 answers "
		               "failed their check.")
		         .arg(model, task).arg(c.rejected).arg(c.answered);
	}
	return out;
}

// Under `ai/tally/<model>/<task>` as "answered/rejected". A model's name has
// characters QSettings reads as separators, so it is percent-encoded.
void model_tally::load(QSettings &s) {
	m_by_model.clear();
	s.beginGroup("ai/tally");
	for (const QString &enc : s.childGroups()) {
		const QString model =
		  QString::fromUtf8(QByteArray::fromPercentEncoding(enc.toUtf8()));
		s.beginGroup(enc);
		for (const QString &task : s.childKeys()) {
			const QStringList parts = s.value(task).toString().split('/');
			if (parts.size() != 2)
				continue;
			counts c;
			c.answered = parts.at(0).toInt();
			c.rejected = qBound(0, parts.at(1).toInt(), c.answered);
			m_by_model[model][task] = c;
		}
		s.endGroup();
	}
	s.endGroup();
}

void model_tally::save(QSettings &s) const {
	s.beginGroup("ai/tally");
	s.remove(QString());
	for (auto m = m_by_model.cbegin(); m != m_by_model.cend(); ++m) {
		s.beginGroup(QString::fromLatin1(m.key().toUtf8().toPercentEncoding()));
		for (auto t = m.value().cbegin(); t != m.value().cend(); ++t)
			s.setValue(t.key(), QString("%1/%2").arg(t.value().answered)
			                                    .arg(t.value().rejected));
		s.endGroup();
	}
	s.endGroup();
}
