#include "permission_dialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

// What the site is asking to do lives in `policy::ask_phrase`, next to the
// feature's name and its settings label. It was a switch here, which is the
// obvious place for it and the wrong one: the settings dialog needs the same
// list to decide which features may be set to `ask` at all, and the two copies
// would have disagreed the first time either grew a row.

// The part that is worth a second sentence, for the ones where the plain name
// understates what is being handed over. Empty where it does not.
const char *consequence(policy::feature f) {
	switch (f) {
		case policy::feature::camera:
		case policy::feature::microphone:
			// The light is the only outward sign on a desktop, and on a phone
			// there may not be one. Worth saying that the grant lasts as long
			// as the page does rather than as long as the call does.
			return "It can start and stop on its own while the page is open.";
		case policy::feature::notifications:
			return "These can arrive after you have left the site.";
		case policy::feature::clipboard_read:
			return "Whatever you last copied may include a password.";
		default: break;
	}
	return "";
}

}  // namespace

permission_dialog::permission_dialog(const QString &host, policy::feature f,
                                      bool encrypted, QWidget *parent)
    : QDialog(parent) {
	setWindowTitle("Permission");
	setObjectName("permission_dialog");
	auto *column = new QVBoxLayout(this);

	// Reached only if a feature is set to `ask` with no words written for it,
	// which `policy::can_ask` is there to stop and which the settings dialog
	// enforces. Says something true rather than nothing, because a prompt with
	// a blank in the middle of the sentence is worse than a vague one.
	const char *phrase = policy::ask_phrase(f);
	if (!phrase)
		phrase = "use a capability of this browser";

	auto *asking = new QLabel(this);
	asking->setObjectName("permission_question");
	asking->setWordWrap(true);
	asking->setText(QStringLiteral("<b>%1</b> wants to %2.")
	                  .arg(host.isEmpty() ? QStringLiteral("This page") : host,
	                        QString::fromLatin1(phrase)));
	column->addWidget(asking);

	const QString note = QString::fromLatin1(consequence(f));
	if (!note.isEmpty()) {
		auto *what = new QLabel(note, this);
		what->setObjectName("permission_note");
		what->setWordWrap(true);
		column->addWidget(what);
	}

	// **The same warning `auth_dialog` gives, for the same reason.** A camera
	// stream negotiated over a connection nobody authenticated can be pointed
	// somewhere other than where the address bar says. It is a weaker case than
	// a password -- the media itself is usually carried separately -- but the
	// page asking is not the page it claims to be, and that is the part worth
	// knowing before answering rather than after.
	if (!encrypted) {
		auto *warning = new QLabel(this);
		warning->setObjectName("permission_insecure");
		warning->setWordWrap(true);
		warning->setText("<b>This connection is not encrypted.</b> There is no "
		                  "proof the page is who it says it is.");
		column->addWidget(warning);
	}

	// **Unchecked, which is not the convenient default and is the right one.**
	// Checked-by-default is what most browsers do and it works because their
	// prompt is a small thing at the top of a window nobody is looking at. Here
	// the answer is being given under interruption, mid-task, by someone who
	// wants the interruption gone -- and the fastest way to make a dialog go
	// away is to hit the button, not to read the checkbox above it. A grant
	// that outlives the moment has to be asked for.
	//
	// The nagging this would otherwise cause is handled elsewhere and better:
	// the shell remembers the answer for the rest of the session, so a page
	// that asks twice is answered twice without anybody seeing it.
	m_remember = new QCheckBox(this);
	m_remember->setObjectName("permission_remember");
	m_remember->setText(host.isEmpty()
	                      ? QStringLiteral("Remember this answer for this site")
	                      : QStringLiteral("Remember this answer for %1").arg(host));
	m_remember->setChecked(false);
	column->addWidget(m_remember);

	// Spare height here rather than spread between the sentences -- the Android
	// lesson from `auth_dialog`, where a full-screen dialog with nothing to
	// absorb the difference put an inch of nothing between every line.
	column->addStretch(1);

	auto *buttons = new QDialogButtonBox(this);
	// **Block is the default button.** Return, an inherited focus, a stray
	// keypress arriving as the dialog appears -- each of those lands on the
	// default, and the one that costs nothing to get wrong is the refusal. A
	// wrongly refused camera is a page that does not work and a person who
	// tries again; a wrongly granted one is a camera that is on.
	auto *block = buttons->addButton("&Block", QDialogButtonBox::RejectRole);
	auto *allow = buttons->addButton("&Allow", QDialogButtonBox::AcceptRole);
	block->setDefault(true);
	block->setFocus();
	column->addWidget(buttons);

	connect(allow, &QPushButton::clicked, this, [this] {
		m_granted = true;
		accept();
	});
	connect(block, &QPushButton::clicked, this, [this] {
		m_granted = false;
		reject();
	});
	// Escape and the window button reach `reject()` without passing through the
	// button above, and `m_granted` is already false -- so a dismissal is a
	// refusal by construction rather than by remembering to set it here.
}

bool permission_dialog::remember() const { return m_remember->isChecked(); }
