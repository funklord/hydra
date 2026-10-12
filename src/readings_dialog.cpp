#include "readings_dialog.h"

#include "channel_rules.h"
#include "media_interpretation.h"
#include "reading_log.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QHeaderView>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

readings_dialog::readings_dialog(reading_log *log, channel_rules *rules,
                                 QWidget *parent)
  : QDialog(parent), m_log(log), m_rules(rules) {
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
	m_correct = bb->addButton("&Correct…", QDialogButtonBox::ActionRole);
	m_correct->setObjectName("correct_reading");
	m_correct->setEnabled(false);
	connect(m_list, &QTreeWidget::currentItemChanged, this,
	         [this](QTreeWidgetItem *now) {
		m_correct->setEnabled(now && now->data(0, Qt::UserRole).isValid());
	});
	connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this] { correct(); });
	connect(m_correct, &QPushButton::clicked, this, [this] { correct(); });
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
		row->setData(0, Qt::UserRole, i);
		row->setText(0, r.when.toLocalTime().toString("yyyy-MM-dd HH:mm"));
		row->setText(1, r.is_set ? r.kind + " (set)" : r.kind);
		row->setText(2, r.artist);
		row->setText(3, r.title);
		row->setText(4, r.rule.isEmpty() ? r.model : r.model + ", then " + r.rule);
		QStringList checks;
		if (!r.rejected.isEmpty())
			checks << QString("%1 not kept").arg(r.rejected.size());
		if (!r.flagged.isEmpty())
			checks << QString("%1 flagged").arg(r.flagged.size());
		row->setText(5, checks.isEmpty() ? QStringLiteral("ok") : checks.join(", "));

		QStringList tip = { r.page, "saved as " + r.file_name + ".<ext>" };
		if (!r.video_title.isEmpty())
			tip << "titled \"" + r.video_title + "\"";
		if (!r.rule.isEmpty())
			tip << "artist and title by " + r.rule;
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

void readings_dialog::correct() {
	QTreeWidgetItem *row = m_list->currentItem();
	if (!m_log || !row || !row->data(0, Qt::UserRole).isValid())
		return;
	const int index = row->data(0, Qt::UserRole).toInt();
	if (index < 0 || index >= m_log->size())
		return;
	reading_record r = m_log->all().at(index);
	const QString channel = r.channel.isEmpty() ? r.uploader : r.channel;

	QDialog dlg(this);
	dlg.setWindowTitle("Correct Reading");
	dlg.setObjectName("correct_dialog");
	auto *form = new QFormLayout(&dlg);
	auto *source = new QLabel(QString("<i>%1</i>, uploaded by %2")
	                              .arg(r.video_title.toHtmlEscaped(),
	                                   r.uploader.toHtmlEscaped()), &dlg);
	source->setWordWrap(true);
	form->addRow("Video:", source);
	auto *artist = new QLineEdit(r.artist, &dlg);
	artist->setObjectName("correct_artist");
	auto *title = new QLineEdit(r.title, &dlg);
	title->setObjectName("correct_title");
	auto *kind = new QComboBox(&dlg);
	kind->setObjectName("correct_kind");
	for (int k = 0; k <= int(media_reading::kind::misc); ++k)
		kind->addItem(media_reading::name_of(media_reading::kind(k)));
	kind->setCurrentText(r.kind);
	form->addRow("&Artist:", artist);
	form->addRow("&Title:", title);
	form->addRow("&Kind:", kind);
	auto *remember = new QCheckBox(&dlg);
	remember->setObjectName("correct_remember");
	form->addRow(remember);
	auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
	                                &dlg);
	connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	form->addRow(bb);

	// **The rule the correction implies, said before it is kept**: the person
	// sees what will happen to the channel's next video, and a fix that
	// explains nothing offers nothing.
	channel_rule rule;
	// Ticked by the first edit that implies a rule, not before: a reading
	// opened and closed unchanged teaches nothing.
	bool ticked = false;
	auto infer = [&] {
		rule = channel_rules::infer(channel, r.video_title, r.uploader,
		                            artist->text(), title->text(),
		                            kind->currentText(), r.kind);
		const bool can = m_rules && !channel.isEmpty() && !r.video_title.isEmpty() &&
		                 !rule.empty();
		remember->setEnabled(can);
		remember->setText(can ? QString("Remember for %1: %2").arg(channel, rule.describe())
		                      : QStringLiteral("Nothing here to remember for the "
		                                       "channel; only this reading changes."));
		const bool changed = artist->text().simplified() != r.artist ||
		                     title->text().simplified() != r.title ||
		                     kind->currentText() != r.kind;
		if (!can)
			remember->setChecked(false);
		else if (changed && !ticked) {
			remember->setChecked(true);
			ticked = true;
		}
	};
	connect(artist, &QLineEdit::textChanged, &dlg, infer);
	connect(title, &QLineEdit::textChanged, &dlg, infer);
	connect(kind, &QComboBox::currentTextChanged, &dlg, infer);
	infer();
	if (dlg.exec() != QDialog::Accepted)
		return;

	r.artist = artist->text().simplified();
	r.title  = title->text().simplified();
	r.kind   = kind->currentText();
	r.rule   = QStringLiteral("your correction");
	m_log->replace(index, r);
	if (remember->isChecked() && m_rules)
		m_rules->set(rule);
	fill();
	emit corrected();
}
