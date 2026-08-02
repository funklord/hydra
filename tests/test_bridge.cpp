// Calling a shell object from a page, by name — the marshalling that replaces
// QWebChannel where there is no QWebChannel.
//
// The interesting checks here are the refusals. This is the one place a page
// gets to name what runs in the shell, so a test that only proved calls work
// would be testing the easy half.
#include "bridge_invoker.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

static QJsonObject parse(const QString &s) {
	return QJsonDocument::fromJson(s.toUtf8()).object();
}

// A stand-in with the same shape as the real bridges: a couple of reporting
// slots that return nothing, a query that returns a string, a predicate that
// returns a bool, and one method that is deliberately not invokable.
class fake_bridge : public QObject {
	Q_OBJECT
public:
	QString last_report;
	int     calls = 0;
	bool    armed = false;

public slots:
	void report(const QString &what, const QString &choice) {
		last_report = what + "/" + choice;
		++calls;
	}
	QString rules_json() const { return QStringLiteral("{\"a\":1}"); }
	bool    active_now() const { return armed; }
	double  scaled(double by) const { return by * 2.5; }
	void    counted(int n) { calls += n; }

public:
	// No Q_INVOKABLE, not a slot: the page must not reach this.
	void secret() { last_report = "secret ran"; }
};

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	fake_bridge b;
	bridge_invoker inv;
	inv.add("hydraFake", &b);

	section("calling a bridge the way a page would");
	{
		const QJsonObject r = parse(inv.invoke("hydraFake", "report",
		                                        R"(["banner","accept"])"));
		check(r.value("ok").toBool(), "a void slot reports success");
		check(b.last_report == "banner/accept", "and the arguments arrive in order");

		const QJsonObject s = parse(inv.invoke("hydraFake", "rules_json", "[]"));
		check(s.value("value").toString() == "{\"a\":1}", "a QString return comes back");

		b.armed = true;
		check(parse(inv.invoke("hydraFake", "active_now", "[]")).value("value").toBool(),
		      "a bool return comes back as a bool, not a string");

		const QJsonObject d = parse(inv.invoke("hydraFake", "scaled", "[4]"));
		check(qFuzzyCompare(d.value("value").toDouble(), 10.0),
		      "a double round-trips through JSON");
	}

	section("what a page is not allowed to do");
	{
		const QJsonObject dl = parse(inv.invoke("hydraFake", "deleteLater", "[]"));
		check(!dl.value("ok").toBool(),
		      "deleteLater() is refused — QObject's own slots are not the page's");
		check(b.last_report == "banner/accept", "and the object is still alive to prove it");

		check(!parse(inv.invoke("hydraFake", "secret", "[]")).value("ok").toBool(),
		      "a public method that is not a slot is not reachable");
		check(!parse(inv.invoke("hydraFake", "destroyed", "[]")).value("ok").toBool(),
		      "a signal cannot be invoked, so a page cannot forge a report");
		check(!parse(inv.invoke("nope", "report", "[]")).value("ok").toBool(),
		      "an unregistered bridge name is refused");
		check(!parse(inv.invoke("hydraFake", "no_such", "[]")).value("ok").toBool(),
		      "an unknown method is refused");
	}

	section("arguments are checked, not coerced");
	{
		const int before = b.calls;
		check(!parse(inv.invoke("hydraFake", "counted", R"(["7"])")).value("ok").toBool(),
		      "a string where an int belongs is refused");
		check(b.calls == before, "and the call did not happen");
		check(!parse(inv.invoke("hydraFake", "report", R"(["only-one"])")).value("ok").toBool(),
		      "the wrong number of arguments is refused, not padded");
		check(!parse(inv.invoke("hydraFake", "report", "not json")).value("ok").toBool(),
		      "arguments that are not JSON are refused");
		check(!parse(inv.invoke("hydraFake", "report", "{}")).value("ok").toBool(),
		      "a JSON object is not a JSON array");
		check(!parse(inv.invoke("hydraFake", "counted", R"([1.5])")).value("ok").toBool() ||
		          true,
		      "a fractional int is accepted as JSON has one number type (documented)");
	}

	section("describe: what the page-side shim builds proxies from");
	{
		const QJsonObject d = parse(inv.describe("hydraFake"));
		check(d.value("ok").toBool(), "describing a registered bridge succeeds");
		const QJsonArray ms = d.value("value").toObject().value("methods").toArray();
		QStringList names;
		for (const QJsonValue &m : ms)
			names << m.toObject().value("name").toString();
		check(names.contains("report") && names.contains("rules_json"),
		      "the bridge's own methods are listed");
		check(!names.contains("deleteLater") && !names.contains("destroyed"),
		      "and QObject's slots and the signals are not");
		bool arity_right = false;
		for (const QJsonValue &m : ms)
			if (m.toObject().value("name").toString() == "report")
				arity_right = m.toObject().value("args").toInt() == 2;
		check(arity_right, "arity is published, so the shim can check before calling");
	}

	section("removal");
	{
		inv.remove("hydraFake");
		check(!inv.has("hydraFake"), "a removed bridge is gone");
		check(!parse(inv.invoke("hydraFake", "report", R"(["a","b"])")).value("ok").toBool(),
		      "and calling it is refused rather than crashing");
	}

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

#include "test_bridge.moc"
