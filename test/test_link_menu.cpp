// What a held finger on a link or an image offers, on Android.
//
// Pure: the entries are `link_menu::entries`, and this checks them without
// a device. The device half -- the platform menu, and each choice reaching
// the tree, the clipboard or the download list -- is
// `tool/emulator_checks.py`'s.
#include "link_menu.h"

#include <QCoreApplication>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

using namespace link_menu;

static QStringList labels(const QList<entry> &e) {
	QStringList out;
	for (const entry &x : e)
		out << x.label;
	return out;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);
	const QUrl page("https://example.org/a?b=1&c=2#d");
	const QUrl picture("https://img.example.org/p.jpg");

	section("what each kind of target earns");
	{
		const QList<entry> link = entries(page, QUrl());
		const QStringList link_labels = { "Open in New Tab", "Open in Background Tab",
		                                  "Copy Link Address", "Save Link" };
		check(labels(link) == link_labels,
		      QString("a link: open, background, copy, save (%1)").arg(labels(link).join(", ")));
		check(link[0].what == action::open && link[1].what == action::open_background &&
		        link[2].what == action::copy && link[3].what == action::save,
		      "each entry does what it says");
		check(link[0].value == "https://example.org/a?b=1&c=2#d",
		      "the address whole, query and fragment kept");

		const QList<entry> image = entries(QUrl(), picture);
		const QStringList image_labels = { "Open Image in New Tab", "Copy Image Address",
		                                   "Save Image" };
		check(labels(image) == image_labels, "an image: open, copy, save");

		const QList<entry> both = entries(page, picture);
		check(both.size() == 7 && both[0].value == page.toString(QUrl::FullyEncoded) &&
		        both[4].value == picture.toString(),
		      "a linked image: the link's four, then the image's three");
		check(entries(picture, picture).size() == 4,
		      "an image that links to itself is offered once, as a link");
	}

	section("what is not offered");
	{
		check(entries(QUrl("javascript:void(0)"), QUrl()).isEmpty(),
		      "a javascript: link does nothing outside its page");
		check(entries(QUrl(), QUrl("data:image/png;base64,iVBOR")).isEmpty() &&
		        entries(QUrl(), QUrl("blob:https://example.org/1-2")).isEmpty(),
		      "a data: or blob: image is not an address anybody can use");
		check(entries(QUrl("javascript:go()"), picture).size() == 3,
		      "but the picture inside a script link still is");
		check(entries(QUrl(), QUrl()).isEmpty(), "nothing for nothing");
	}

	section("mail and phone links: the address, without the scheme");
	{
		const QList<entry> mail = entries(QUrl("mailto:someone@example.org"), QUrl());
		check(mail.size() == 1 && mail[0].label == "Copy Email Address" &&
		        mail[0].what == action::copy && mail[0].value == "someone@example.org",
		      "mailto: copies the address");
		const QList<entry> tel = entries(QUrl("tel:+46701234567"), QUrl());
		check(tel.size() == 1 && tel[0].value == "+46701234567", "tel: copies the number");
	}

	section("the string the Java side reads");
	{
		const QString s = encode(entries(page, QUrl()));
		const QStringList lines = s.split('\n');
		check(lines.size() == 4 && lines[0] == "open\tOpen in New Tab\t" + page.toString(QUrl::FullyEncoded) &&
		        lines[1].startsWith("background\t") && lines[3].startsWith("save\t"),
		      "one line per entry: action, label, value");
		check(encode({}).isEmpty(), "empty when there is nothing, which leaves the press to the page");
		const QString odd = encode({ entry{ "Copy", action::copy, "a\tb\nc" } });
		check(odd == "copy\tCopy\ta b c", "a tab or newline in a value cannot break the format");
		action a = action::copy;
		check(parse_action("background", &a) && a == action::open_background &&
		        parse_action("save", &a) && a == action::save && !parse_action("delete", &a),
		      "the action word round-trips, and an unknown one is refused");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
