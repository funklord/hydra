// What the window says about the page in front of it.
//
// Five separate pieces of state -- the title, the loading bar, the find count,
// the link target and whatever went wrong -- each assert something about the
// current page, and each was hooked into the four moments that change it one
// addition at a time. Two of them were never hooked into all four and had
// become lies; `page_changed` re-derives the lot in one place, and this is what
// holds it to that.
//
// Split out of `try_navigate`, which had grown to sixty-nine checks across
// three subjects: a failure named the driver and meant any of them.
#include "shell_fixture.h"

#include "auth_dialog.h"
#include "cert_dialog.h"
#include "node.h"
#include "tab_tree_model.h"
#include <QAbstractButton>
#include <QApplication>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QStatusBar>

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);
	using namespace shell;

	fixture f("/tmp/hydra-chrome");
	main_window &w   = f.window;
	QAction   *back  = f.back, *fwd = f.fwd, *reload = f.reload;
	QLineEdit *address = f.address;
	QTreeView *tv    = f.tv;
	policy_engine &policy = f.policy;
	const QString out = f.out, one = f.one, two = f.two;
	(void)fwd; (void)back; (void)reload; (void)tv; (void)policy;
	(void)one; (void)two;

	if (!back || !fwd || !reload || !address || !tv) {
		std::printf("the window did not come up as expected\n");
		return 1;
	}
	check(f.open_tab(0, "one.html"), "a page is open to talk about");

	section("the window says which page it is showing");
	{
		// A window called "Hydra" and nothing else is indistinguishable from
		// every other one in a task switcher.
		check(w.windowTitle().contains("one") || w.windowTitle().contains("One"),
		      QString("the title names the page (%1)").arg(w.windowTitle()));
		check(w.windowTitle().endsWith("Hydra"),
		      "and still says which browser it is");
	}

	section("where a link would take you");
	{
		// Driven through the window's own method rather than by hovering: what
		// is worth checking is the eliding and the clearing, and synthesising a
		// mouse move over an offscreen page tests Qt rather than this.
		QStatusBar *sb = w.findChild<QStatusBar *>();
		check(sb, "the status bar is reachable");
		if (sb) {
			w.show_link_target(QUrl("https://example.test/a/page"));
			check(sb->currentMessage() == "https://example.test/a/page",
			      QString("a short target is shown whole (%1)")
			          .arg(sb->currentMessage()));

			// **The host has to survive.** A url elided from the right keeps
			// the scheme and loses the only part that answers "where does this
			// go", which is the entire question being asked.
			const QString host = "https://example.test/";
			w.show_link_target(QUrl(host + QString("x").repeated(400)));
			const QString shown = sb->currentMessage();
			check(shown.size() < 140,
			      QString("a long one is cut down (%1 chars)").arg(shown.size()));
			check(shown.startsWith("https://example.test/"),
			      QString("and keeps the host (%1)").arg(shown.left(40)));

			w.show_link_target(QUrl());
			check(sb->currentMessage().isEmpty(),
			      "leaving the link clears it rather than leaving a stale claim");
		}
	}

	section("a page that does not arrive says so");
	{
		// The bar is only ever on screen while something is loading, and a
		// failure used to be entirely silent: the bar would have sat at
		// whatever it reached and nothing would have said why.
		QProgressBar *bar = w.findChild<QProgressBar *>("load_progress");
		QStatusBar   *sb  = w.findChild<QStatusBar *>();
		check(bar && sb, "the loading bar and the status bar are reachable");
		if (bar && sb) {
			check(!bar->isVisible(), "no bar once the page has arrived");

			const QString missing =
			    QUrl::fromLocalFile(out + "/there-is-no-such-page.html").toString();
			address->setText(missing);
			emit address->returnPressed();
			for (int i = 0; i < 40 &&
			                 !sb->currentMessage().contains("could not be loaded"); ++i)
				spin(200);
			check(sb->currentMessage().contains("could not be loaded"),
			      QString("a failed load is reported (%1)").arg(sb->currentMessage()));
			check(!bar->isVisible(), "and the bar goes away rather than sticking");
		}
	}

	section("a certificate that could not be trusted");
	{
		// The refusal itself is not the change -- Qt rejects an unhandled
		// certificate error already, which is correct. What was missing is any
		// account of it: the page just failed, and "could not be loaded" is
		// what a site being down says too. Those want different responses.
		QStatusBar *sb = w.findChild<QStatusBar *>();
		if (sb) {
			w.report_certificate_rejected(QUrl("https://expired.example.test/x"),
			                               "The certificate has expired");
			const QString msg = sb->currentMessage();
			check(msg.contains("expired.example.test"),
			      QString("it names the site (%1)").arg(msg.left(40)));
			check(msg.contains("certificate"),
			      "and says the certificate was the problem");
			check(msg.contains("expired"), "including what was wrong with it");

			// **One refusal must not silence later failures.** A bad
			// certificate suppresses the generic message for its own load; if
			// that suppression outlived the load, every failure afterwards
			// would be silent for the life of the window.
			//
			// Waited for the exact sentence, not a fragment: the certificate
			// message contains "could not be trusted", so a predicate of
			// "could not" matches the very message this is waiting to see
			// replaced, falls straight through, and reports the app broken
			// when the predicate was.
			address->setText(QUrl::fromLocalFile(out + "/still-not-there.html")
			                     .toString());
			emit address->returnPressed();
			for (int i = 0; i < 40 &&
			                 !sb->currentMessage().contains("could not be loaded"); ++i)
				spin(200);
			check(sb->currentMessage().contains("could not be loaded"),
			      QString("an ordinary failure after it still speaks (%1)")
			          .arg(sb->currentMessage()));
		}
	}

	section("a site asking for a username and password");
	{
		// Built directly and never exec'd: a modal would block the driver, and
		// what is worth checking is what it says before anybody types. Until
		// this existed the challenge was unanswered, so the page failed with
		// nothing on screen to say a password had been asked for.
		{
			auth_dialog secure("bank.example", "Accounts", true, &w);
			auto *site = secure.findChild<QLabel *>("auth_site");
			check(site && site->text().contains("bank.example"),
			      "it names the site being signed in to");
			check(secure.findChild<QLabel *>("auth_realm") != nullptr,
			      "and shows the realm when the site gave one");
			check(secure.findChild<QLabel *>("auth_insecure") == nullptr,
			      "no warning on an encrypted connection");
			auto *pw = secure.findChild<QLineEdit *>("auth_password");
			check(pw && pw->echoMode() == QLineEdit::Password,
			      "the password field does not show what is typed");
		}
		{
			// **The case worth interrupting for.** Basic authentication over a
			// plain connection puts the password on the wire in a form
			// anything in between can read, and only the person typing it can
			// decide whether that is acceptable.
			auth_dialog plain("intranet.example", "", false, &w);
			check(plain.findChild<QLabel *>("auth_insecure") != nullptr,
			      "an unencrypted connection is called out");
			check(plain.findChild<QLabel *>("auth_realm") == nullptr,
			      "and an empty realm is left out rather than shown blank");
		}
	}

	section("the proxy asking is not the site asking");
	{
		// **The two prompts are the same box.** A person who cannot tell which
		// one is in front of them can hand a site's password to whoever runs
		// the network, and that is a mistake with no undo -- so what is checked
		// here is the wording, which is the only thing that distinguishes them.
		auth_dialog proxy("proxy.corp.example", "Staff", false, &w,
		                   auth_dialog::asker::proxy);
		auto *site = proxy.findChild<QLabel *>("auth_site");
		check(site && site->text().contains("proxy.corp.example"),
		      "the proxy prompt names the proxy");
		check(site && site->text().contains("proxy"),
		      "and calls it a proxy rather than presenting it as the site");
		auto *note = proxy.findChild<QLabel *>("auth_proxy_note");
		check(note && note->text().contains("not the site"),
		      "with a line saying which password is being asked for");
		check(proxy.windowTitle().contains("proxy"),
		      "and a title that says so before the window is read");

		auth_dialog ordinary("bank.example", "", true, &w);
		check(ordinary.findChild<QLabel *>("auth_proxy_note") == nullptr,
		      "while the site prompt carries no such line, or it would mean "
		      "nothing on the one that does");

		// Photographed as well as asserted. A padlock that passed every check
		// while rendering as an unreadable smudge is why: an assertion reads
		// the widget tree, and whether the words on it distinguish two prompts
		// is a question only looking can answer.
		proxy.adjustSize();
		proxy.grab().save(f.out + "/auth-proxy.png");
	}

	section("a site asking who you are, by certificate");
	{
		QList<web_view_backend::certificate_offer> offered;
		web_view_backend::certificate_offer a;
		a.subject = "Ada Lovelace";  a.issuer = "Example CA";
		a.valid_until = "2027-01-01";
		web_view_backend::certificate_offer b;
		b.subject = "Ada (work)";    b.issuer = "Corp CA";
		b.valid_until = "2026-09-01";
		offered << a << b;

		cert_dialog dlg("id.example", offered, &w);
		auto *site = dlg.findChild<QLabel *>("cert_site");
		check(site && site->text().contains("id.example"),
		      "it names the site asking to identify you");

		auto *list = dlg.findChild<QListWidget *>("cert_list");
		check(list && list->count() == 2, "both certificates are offered");
		check(list && list->item(0)->text().contains("Ada Lovelace") &&
		          list->item(0)->text().contains("Example CA"),
		      "each named by holder and issuer, since an unexpected issuer is "
		      "the case worth noticing");

		// **Nothing selected, and Send unavailable.** A pre-selected row plus
		// an enabled button is one keystroke from identifying somebody who was
		// still reading the question.
		check(list && list->currentRow() < 0, "nothing is selected to begin with");
		QPushButton *send = nullptr, *none = nullptr;
		for (QPushButton *b2 : dlg.findChildren<QPushButton *>()) {
			if (b2->text().contains("Send"))  send = b2;
			if (b2->text().contains("Don't")) none = b2;
		}
		check(send && !send->isEnabled(),
		      "so Send is unavailable until there is something to send");
		check(none && none->isDefault(),
		      "and declining is the default button");
		check(dlg.chosen() == -1,
		      "an unanswered dialog sends nothing, which is what Qt did before "
		      "a chooser existed -- now as a decision rather than the only "
		      "outcome available");

		if (list && send) {
			list->setCurrentRow(1);
			check(send->isEnabled(), "selecting one makes Send available");
		}
		dlg.adjustSize();
		dlg.grab().save(f.out + "/cert-choose.png");
	}

	section("a page whose renderer dies says so");
	{
		// Driven directly, because a render process cannot be killed from here
		// on purpose and the wiring is one line. What is worth checking is what
		// gets said, that it names the site, and that it does not expire.
		QStatusBar *sb = w.findChild<QStatusBar *>();
		if (sb) {
			w.report_render_crash("example.test");
			check(sb->currentMessage().contains("example.test"),
			      QString("it names the site (%1)").arg(sb->currentMessage()));
			check(sb->currentMessage().contains("Reload"),
			      "and says what to do about it");

			// **No timeout.** This describes a state the window is still in;
			// one that expires leaves somebody in front of a blank page
			// wondering what they missed.
			spin(1200);
			check(!sb->currentMessage().isEmpty(),
			      "and it is still there a moment later");

			// Loading something is what makes it untrue.
			address->setText(QUrl::fromLocalFile(one).toString());
			emit address->returnPressed();
			check(wait_for(address, "one.html"), "loading a page again");
			spin(500);
			check(!sb->currentMessage().contains("stopped responding"),
			      QString("clears it (%1)").arg(sb->currentMessage()));
		}
	}

	section("switching tabs drops what belonged to the old page");
	{
		// Two live bugs, both of the same shape: a piece of chrome that was
		// true about the page it was set from and is a lie about the next one.
		// Neither had anything clearing it, because each was hooked into the
		// four places that matter one addition at a time.
		auto *count = w.findChild<QLabel *>("find_count");
		QAction *find_act = nullptr;
		for (QAction *a : w.findChildren<QAction *>())
			if (a->text().contains("Find on &Page"))
				find_act = a;
		QStatusBar *sb = w.findChild<QStatusBar *>();

		if (count && find_act && sb && tv->model()->rowCount(tv->model()->index(0, 0)) > 1) {
			find_act->trigger();
			auto *input = w.findChild<QLineEdit *>("find_input");
			input->setText("one");
			for (int i = 0; i < 30 && count->text().isEmpty(); ++i)
				spin(200);
			check(!count->text().isEmpty(),
			      QString("a count is showing before the switch (%1)").arg(count->text()));

			w.show_link_target(QUrl("https://example.test/somewhere"));
			check(!sb->currentMessage().isEmpty(), "and a link target with it");

			// The second tab.
			emit tv->activated(tv->model()->index(1, 0, tv->model()->index(0, 0)));
			check(wait_for(address, "two.html"), "the other tab opens");
			spin(400);

			check(count->text().isEmpty(),
			      QString("the match count does not follow (%1)").arg(count->text()));
			check(!sb->currentMessage().contains("example.test"),
			      QString("nor does the link target (%1)").arg(sb->currentMessage()));
			check(w.windowTitle().contains("two") || w.windowTitle().contains("Two"),
			      QString("and the title is the new page's (%1)").arg(w.windowTitle()));
		}
	}

	section("and when the page goes away again");
	{
		// Deleting the open tab takes its view out of the stack and leaves the
		// window on its placeholder -- the state it started in, so the buttons
		// have to go back with it rather than keep the last page's answers.
		// **Every tab, not just one.** The tree holds two now, and deleting one
		// while the other is still open leaves a page in the window -- so the
		// checks below would be asserting that the chrome forgets a page which
		// is still there, which is the opposite of what they mean.
		auto *model = w.findChild<tab_tree_model *>();
		node *folder = model->root()->children.first();
		while (!folder->children.isEmpty())
			model->remove_node(folder->children.first());
		spin(900);
		check(!back->isEnabled() && !fwd->isEnabled() && !reload->isEnabled(),
		      "all three go grey again once the page is gone");
		check(w.windowTitle() == "Hydra",
		      QString("and the window drops the page's name (%1)")
		          .arg(w.windowTitle()));
	}

	return report();
}
