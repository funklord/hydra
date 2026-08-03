// SPDX-License-Identifier: GPL-3.0-or-later
#include "android_dialogs.h"

#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QGuiApplication>
#include <QScreen>

namespace {

class dialog_sizer : public QObject {
public:
	using QObject::QObject;

protected:
	bool eventFilter(QObject *o, QEvent *e) override {
		if (e->type() != QEvent::Show)
			return QObject::eventFilter(o, e);
		auto *dlg = qobject_cast<QDialog *>(o);
		if (!dlg)
			return QObject::eventFilter(o, e);

		// availableGeometry, not geometry: the status and navigation bars are
		// not ours, and a dialog placed under them has its buttons where the
		// system's back gesture lives.
		if (QScreen *screen = QGuiApplication::primaryScreen()) {
			const QRect area = screen->availableGeometry();
			// A minimum of nothing: a dialog laid out for a desktop asks for
			// more width than the screen has, and honouring that request is
			// what put the buttons off the edge. Shrinking is allowed to make
			// the contents smaller than they would like -- unreadable is worse
			// than unreachable only when it is actually unreadable, and these
			// are lists and buttons, which wrap.
			dlg->setMinimumSize(0, 0);
			dlg->setGeometry(area);
		}
		return QObject::eventFilter(o, e);
	}
};

}  // namespace

namespace android_dialogs {

void install() {
	// Parented to the application, so it lives exactly as long as the filter is
	// wanted and is not something a caller has to remember to delete.
	qApp->installEventFilter(new dialog_sizer(qApp));
}

}  // namespace android_dialogs
