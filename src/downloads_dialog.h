// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>
#include <QHash>

class QLabel;
class QPushButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class download_manager;

// The downloads window (architecture doc §11.2, §11.4).
//
// One list for every source, which is the whole point: a torrent appears beside
// an HTTP file with the same columns, the same progress bar and the same
// controls, because §11.4 decided torrents are a first-class download rather
// than a side feature.
//
// Two rules this window is built around:
//
//  1. **It never asks what transport a row is.** Everything it varies — whether
//     Pause is offered, whether children are shown, whether the row carries a
//     warning — comes from `source_capabilities` and the job's own fields. The
//     word "torrent" does not appear in the logic, only in a source's
//     display_name.
//
//  2. **Publicly-observable rows are visibly different.** This is the other
//     half of the §11.4 privacy decision. Making a torrent behave exactly like
//     every other download is the goal, and it is also exactly what could
//     mislead someone into thinking it *is* like every other download. The
//     consent dialog says it once before the first one; this says it
//     permanently, on the row, for as long as the transfer exists.
class downloads_dialog : public QDialog {
	Q_OBJECT
public:
	explicit downloads_dialog(download_manager *downloads,
	                           QWidget *parent = nullptr);

private:
	void refresh();              // reconcile rows against the job list
	void schedule_refresh();     // coalesce bursts of changed()
	void update_buttons();
	void act_pause();
	void act_resume();
	void act_cancel();
	void act_open_folder();
	int  selected_job() const;

	download_manager *m_downloads = nullptr;

	QTreeWidget *m_list   = nullptr;
	QLabel      *m_note   = nullptr;
	QPushButton *m_pause  = nullptr;
	QPushButton *m_resume = nullptr;
	QPushButton *m_cancel = nullptr;
	QPushButton *m_folder = nullptr;
	QTimer      *m_coalesce = nullptr;

	// Rows are reconciled in place rather than rebuilt. changed() fires on
	// every chunk of every transfer, and clearing the tree that often would
	// throw away the selection and scroll position several times a second.
	QHash<int, QTreeWidgetItem *> m_rows;
};
