// SPDX-License-Identifier: GPL-3.0-or-later
#include "media_dialog.h"
#include "download_manager.h"
#include "player_launcher.h"
#include "local_proxy.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {

QString kind_label(media_kind k) {
	switch (k) {
		case media_kind::hls:     return "HLS stream";
		case media_kind::dash:    return "DASH stream";
		case media_kind::direct:  return "File";
		case media_kind::segment: return "Segment";
	}
	return "Unknown";
}

}  // namespace

media_dialog::media_dialog(media_detector *detector, player_launcher *players,
                            download_manager *downloads, local_proxy *proxy,
                            QWidget *parent)
	: QDialog(parent), m_detector(detector), m_players(players),
	  m_downloads(downloads), m_proxy(proxy) {
	setWindowTitle("Media on this page");
	resize(720, 340);

	auto *outer = new QVBoxLayout(this);

	m_list = new QTreeWidget(this);
	m_list->setColumnCount(4);
	m_list->setHeaderLabels({"Type", "Name", "Host", ""});
	m_list->setRootIsDecorated(false);
	m_list->header()->setStretchLastSection(false);
	m_list->header()->setSectionResizeMode(1, QHeaderView::Stretch);
	outer->addWidget(m_list, 1);

	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	outer->addWidget(m_status);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	outer->addWidget(buttons);
}

void media_dialog::set_site(const QString &site_host, const QString &node_id,
                            const stream_context &ctx) {
	m_site    = site_host;
	m_node_id = node_id;
	m_ctx     = ctx;
	repopulate();
}

void media_dialog::repopulate() {
	m_list->clear();
	const QList<media_item> items = m_detector->items_for(m_site);

	for (const media_item &item : items) {
		auto *row = new QTreeWidgetItem(m_list);
		row->setText(0, kind_label(item.kind));
		row->setText(1, item.label);
		row->setText(2, item.url.host());

		// Both actions on every row; the list is already primary-first (§11.3).
		auto *cell    = new QWidget(m_list);
		auto *layout  = new QHBoxLayout(cell);
		layout->setContentsMargins(0, 0, 0, 0);
		auto *watch   = new QPushButton("▶ Watch", cell);
		auto *save    = new QPushButton("⬇ Download", cell);
		layout->addWidget(watch);
		layout->addWidget(save);
		connect(watch, &QPushButton::clicked, this, [this, item] { this->watch(item); });
		connect(save,  &QPushButton::clicked, this, [this, item] { this->save(item); });
		m_list->setItemWidget(row, 3, cell);
	}
	m_list->resizeColumnToContents(0);
	m_list->resizeColumnToContents(3);

	if (items.isEmpty()) {
		m_status->setText("Nothing detected yet. Many sites only request the "
		                  "manifest when their player starts, so try pressing "
		                  "play first.");
	} else {
		m_status->setText(QString("%1 item(s). The first row is the stream this "
		                          "page looks to be playing.").arg(items.size()));
	}
}

void media_dialog::watch(const media_item &item) {
	const QString warn = m_players->warning_for(item);
	// Hand the player a localhost URL when the proxy is up, so the CDN sees
	// the page's Referer and cookies rather than a naked request (§11.3).
	const QUrl via = m_proxy ? m_proxy->publish(item.url, m_ctx) : QUrl();
	QString error;
	if (!m_players->play(item, &error, via)) {
		m_status->setText("<b>" + error.toHtmlEscaped() + "</b>");
		return;
	}
	m_status->setText(warn.isEmpty()
	                      ? QString("Opened in %1%2.").arg(m_players->selected(),
	                            via.isValid() ? " via the local proxy" : "")
	                      : "<b>Warning:</b> " + warn.toHtmlEscaped());
}

void media_dialog::save(const media_item &item) {
	QString error;
	const int id = m_downloads->enqueue(item.url, m_node_id, &error);
	m_status->setText(id ? QString("Queued download to %1.")
	                           .arg(m_downloads->directory())
	                     : "<b>" + error.toHtmlEscaped() + "</b>");
}
