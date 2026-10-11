#pragma once

#include <QDialog>

class reading_log;
class QTreeWidget;

// **What was read, after the status line has gone.** The list behind Tools ->
// Media -> Recent Readings: one row per reading, newest first, and in each
// row's tooltip everything the reading decided and why -- the page, the file
// name it produced, the tracklist taken, and what its checks rejected or
// flagged. Read-only but for Clear All: correcting a reading is the next
// step, and belongs to the channel rather than to one row.
class readings_dialog : public QDialog {
	Q_OBJECT
public:
	readings_dialog(reading_log *log, QWidget *parent = nullptr);

signals:
	// The person cleared the history; the shell saves the empty log.
	void cleared();

private:
	void fill();

	reading_log *m_log = nullptr;
	QTreeWidget *m_list = nullptr;
};
