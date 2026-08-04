// The other half of the annoyed button: did the rules just applied break the
// page?
//
// **What is worth testing is the undo, not the dialog.** Over-blocking is
// silent -- a rule that kills a player or a login form produces no error, no
// console message and nothing in the request log -- so the only thing that
// makes this question worth asking is that answering "it broke" removes the
// rules. A prompt whose answer changes nothing is the defect this project
// removed a permission for.
#include "main_window.h"
#include "node.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QToolBar>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void spin(int ms) { QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec(); }

// Captured by value: this returns before the box exists.
static void answer(bool *saw, QString button_text) {
	QTimer::singleShot(500, [saw, button_text] {
		for (QWidget *w : QApplication::topLevelWidgets()) {
			auto *box = qobject_cast<QMessageBox *>(w);
			if (!box || !box->isVisible())
				continue;
			if (saw) *saw = true;
			if (button_text.isEmpty()) { box->reject(); return; }
			for (QAbstractButton *b : box->buttons())
				if (b->text().remove('&').startsWith(button_text)) {
					box->setResult(0);
					b->click();
					return;
				}
			box->reject();
			return;
		}
	});
}

static QAction *toolbar_action(QWidget *w, const QString &text) {
	for (QToolBar *bar : w->findChildren<QToolBar *>())
		for (QAction *a : bar->actions())
			if (a->text() == text)
				return a;
	return nullptr;
}

static QString filters_text(const QString &path) {
	QFile f(path);
	return f.open(QIODevice::ReadOnly | QIODevice::Text)
	           ? QString::fromUtf8(f.readAll()) : QString();
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	const QString out = qEnvironmentVariableIsSet("HYDRA_TEST_OUT")
	                        ? qgetenv("HYDRA_TEST_OUT") : QString("/tmp/hydra-confirm");
	QDir(out).removeRecursively();
	QDir().mkpath(out);

	// Two rules already on disk, as though a proposal had just been accepted.
	// Going through the real file rather than reaching into the window is what
	// makes the undo assertion mean something: the rules have to actually leave
	// the list the browser filters with.
	const QString filters = out + "/filters-ai.txt";
	{
		QFile f(filters);
		if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return 1;
		f.write("! seeded by try_confirm\n"
		         "||ads.example.com^\n"
		         "example.com##.ad-banner\n"
		         "||keepme.example.com^\n");
	}
	const QString tree = out + "/tree.txt";
	QFile tf(tree);
	if (!tf.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
	tf.write("- [f0] folder | Mine\n"
	          "  - [a1] unopened | A page | https://example.com/ | "
	          "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n");
	tf.close();

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(tree);
	w.resize(1000, 700);
	w.show();
	spin(1200);

	section("it is not there until it is earned");
	QAction *confirm = toolbar_action(&w, "Still working?");
	check(confirm != nullptr, "the action exists");
	if (!confirm) { std::printf("\n%d passed, %d failed\n", g_pass, g_fail); return 1; }
	check(!confirm->isVisible(),
	      "and is hidden with nothing to confirm — a button that is always "
	      "there measures who likes pressing buttons");

	section("nothing added, nothing asked");
	{
		QMetaObject::invokeMethod(&w, "offer_confirmation",
		                           Q_ARG(QStringList, QStringList()),
		                           Q_ARG(QString, QString("example.com")));
		spin(200);
		check(!confirm->isVisible(),
		      "an empty proposal raises no question, since nothing can have broken");
	}

	section("rules applied, so the question appears");
	{
		const QStringList added = { "||ads.example.com^", "example.com##.ad-banner" };
		QMetaObject::invokeMethod(&w, "offer_confirmation",
		                           Q_ARG(QStringList, added),
		                           Q_ARG(QString, QString("example.com")));
		spin(200);
		check(confirm->isVisible(), "the toolbar offers it");
	}

	section("saying it broke actually removes them");
	{
		bool saw = false;
		answer(&saw, "It Broke");
		confirm->trigger();
		spin(1500);
		check(saw, "the question was put");

		const QString after = filters_text(filters);
		check(!after.contains("||ads.example.com^"),
		      "the network rule is gone from the file the browser filters with");
		check(!after.contains("example.com##.ad-banner"),
		      "and so is the cosmetic one");
		check(after.contains("||keepme.example.com^"),
		      "while a rule nobody complained about is untouched");
		check(!confirm->isVisible(), "and the question stops being asked");
	}

	section("saying it works keeps them, and also stops asking");
	{
		const QStringList added = { "||keepme.example.com^" };
		QMetaObject::invokeMethod(&w, "offer_confirmation",
		                           Q_ARG(QStringList, added),
		                           Q_ARG(QString, QString("example.com")));
		spin(200);
		check(confirm->isVisible(), "the question comes back for a new proposal");

		bool saw = false;
		answer(&saw, "Still Works");
		confirm->trigger();
		spin(1200);
		check(saw, "the question was put again");
		check(filters_text(filters).contains("||keepme.example.com^"),
		      "the rule stays");
		check(!confirm->isVisible(), "and it is not asked twice");
	}

	section("dismissing is an answer too");
	{
		QMetaObject::invokeMethod(&w, "offer_confirmation",
		                           Q_ARG(QStringList, QStringList{ "||x.example^" }),
		                           Q_ARG(QString, QString("example.com")));
		spin(200);
		bool saw = false;
		answer(&saw, "");          // closed without choosing
		confirm->trigger();
		spin(1200);
		check(saw, "the question was put");
		check(!confirm->isVisible(),
		      "closing it means 'stop asking', not 'ask me again'");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
