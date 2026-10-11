#include "readings_dialog.h"

#include "reading_log.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

readings_dialog::readings_dialog(reading_log *log, QWidget *parent)
  : QDialog(parent), m_log(log) {
	setWindowTitle("Recent Readings");
	setObjectName("readings_dialog");
	auto *box = new QVBoxLayout(this);

	m_list = new QTreeWidget(this);
	m_list->setObjectName("readings");
	m_list->setRootIsDecorated(false);
	m_list->setHeaderLabels({ "When", "Kind", "Artist", "Title", "Read by",
	                          "Checks" });
	m_list->header()->setSectionResizeMode(3, QHeaderView::Stretch);
	box->addWidget(m_list, 1);

	auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, this);
	QPushButton *clear = bb->addButton("C&lear All", QDialogButtonBox::ResetRole);
	clear->setObjectName("clear_readings");
	connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(clear, &QPushButton::clicked, this, [this] {
		if (!m_log || m_log->size() == 0)
			return;
		if (QMessageBox::question(this, "Clear Readings",
		        QString("Forget all %1 readings?").arg(m_log->size()))
		    != QMessageBox::Yes)
			return;
		m_log->clear_all();
		fill();
		emit cleared();
	});
	box->addWidget(bb);
	fill();
	resize(860, 420);
}

void readings_dialog::fill() {
	m_list->clear();
	if (!m_log)
		return;
	const QList<reading_record> all = m_log->all();
	if (all.isEmpty()) {
		auto *row = new QTreeWidgetItem(m_list);
		row->setText(3, "Nothing has been read yet. Tools > Media > Find Media "
		                "on This Page reads a video.");
		row->setDisabled(true);
		return;
	}
	for (int i = int(all.size()) - 1; i >= 0; --i) {
		const reading_record &r = all.at(i);
		auto *row = new QTreeWidgetItem(m_list);
		row->setText(0, r.when.toLocalTime().toString("yyyy-MM-dd HH:mm"));
		row->setText(1, r.is_set ? r.kind + " (set)" : r.kind);
		row->setText(2, r.artist);
		row->setText(3, r.title);
		row->setText(4, r.model);
		QStringList checks;
		if (!r.rejected.isEmpty())
			checks << QString("%1 not kept").arg(r.rejected.size());
		if (!r.flagged.isEmpty())
			checks << QString("%1 flagged").arg(r.flagged.size());
		row->setText(5, checks.isEmpty() ? QStringLiteral("ok") : checks.join(", "));

		QStringList tip = { r.page, "saved as " + r.file_name + ".<ext>" };
		if (!r.tracklist.isEmpty())
			tip << "tracklist from " + r.tracklist;
		if (!r.from_model.isEmpty())
			tip << "from the model: " + r.from_model.join(", ");
		for (const QString &x : r.rejected)
			tip << "not kept -- " + x;
		for (const QString &x : r.flagged)
			tip << "flagged -- " + x;
		const QString text = tip.join('\n');
		for (int c = 0; c < m_list->columnCount(); ++c)
			row->setToolTip(c, text);
	}
	for (int c = 0; c < 3; ++c)
		m_list->resizeColumnToContents(c);
}
