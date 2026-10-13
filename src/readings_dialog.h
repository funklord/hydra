#pragma once

#include <QDialog>

class channel_rules;
class reading_log;
class QPushButton;
class QTreeWidget;

// **What was read, after the status line has gone.** The list behind Tools ->
// Media -> Recent Readings: one row per reading, newest first, and in each
// row's tooltip everything the reading decided and why -- the page, the file
// name it produced, the tracklist taken, and what its checks rejected or
// flagged. **Correct...** fixes a reading's artist, title or kind, and offers
// to remember the rule the fix implies for the channel (`channel_rules`), so
// that channel's later readings follow it.
class readings_dialog : public QDialog {
	Q_OBJECT
public:
	readings_dialog(reading_log *log, channel_rules *rules,
	                QWidget *parent = nullptr);

signals:
	// The person cleared the history; the shell saves the empty log.
	void cleared();
	// A reading was corrected, and perhaps a channel's rule kept; the shell
	// saves both. `overruled`: the tasks whose kept answer from `model` the
	// correction showed wrong, newly -- the shell counts them against it.
	void corrected(const QString &model, const QStringList &overruled);

private:
	void fill();
	void correct();

	reading_log   *m_log     = nullptr;
	channel_rules *m_rules   = nullptr;
	QTreeWidget   *m_list    = nullptr;
	QPushButton   *m_correct = nullptr;
};
