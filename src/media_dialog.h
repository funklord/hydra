// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "media_detector.h"

#include <QDialog>

class QLabel;
class QTreeWidget;
class download_manager;
class player_launcher;

// The compact list behind the media badge (architecture doc §11.2/§11.3).
// One row per detected stream, each offering both ▶ Watch and ⬇ Download, with
// the primary stream first. Watch is the default action, because the whole
// point is that the site's own player is broken.
class media_dialog : public QDialog {
	Q_OBJECT
public:
	media_dialog(media_detector *detector, player_launcher *players,
	              download_manager *downloads, QWidget *parent = nullptr);

	void set_site(const QString &site_host, const QString &node_id);

private:
	void repopulate();
	void watch(const media_item &item);
	void save(const media_item &item);

	media_detector   *m_detector  = nullptr;
	player_launcher  *m_players   = nullptr;
	download_manager *m_downloads = nullptr;
	QString m_site;
	QString m_node_id;

	QTreeWidget *m_list   = nullptr;
	QLabel      *m_status = nullptr;
};
