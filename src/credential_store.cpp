// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef HYDRA_HAVE_SECRET
// **Before any Qt header, deliberately.** Qt defines `signals` as a macro for
// `public`, libsecret pulls in gio, and `GDBusInterfaceInfo` has a member
// called `signals` -- so in the other order this file fails to compile inside
// a system header, with an error that points at glib and blames nothing that
// is actually wrong. The alternative is QT_NO_KEYWORDS for the whole target,
// which would rewrite every `signals:` in the tree to pay for one file.
#include <libsecret/secret.h>
#endif

#include "credential_store.h"

#include <QByteArray>

namespace credential_store {

namespace {

// Both halves are base64 before they meet, so the separator can be a single
// space and the split can never be ambiguous. The id is base64'd purely for
// that property -- it is a name the user typed, and a name may contain a space
// or a newline where base64 may not.
const char kSeparator = ' ';

#ifdef HYDRA_HAVE_SECRET

// One item, found by fixed attributes. The attributes are searchable metadata
// and are *not* encrypted by the service, so nothing identifying goes in them
// beyond which program and which kind of item this is.
const SecretSchema *schema() {
	static const SecretSchema s = {
		"org.hydra.Browser.Secret", SECRET_SCHEMA_NONE,
		{
			{ "application", SECRET_SCHEMA_ATTRIBUTE_STRING },
			{ "kind",        SECRET_SCHEMA_ATTRIBUTE_STRING },
			{ nullptr,       SECRET_SCHEMA_ATTRIBUTE_STRING },
		},
		0, 0, 0, 0, 0, 0, 0, 0,
	};
	return &s;
}

const char *kApplication = "hydra";

// Which item this is, and it is overridable **so that a test cannot destroy a
// real pairing**. Every function here addresses one item by fixed attributes,
// so a suite exercising save/clear against the default name would delete
// whatever the user had actually paired -- silently, and on a machine where the
// feature works rather than one where it does not. `test_credstore` refuses to
// run its round trip unless this is set to something else, so the protection
// cannot be forgotten by the next test that wants to write here.
QByteArray kind() {
	static const QByteArray k = [] {
		const QByteArray from_env = qgetenv("HYDRA_SECRET_KIND");
		return from_env.isEmpty() ? QByteArray("keepassxc-association")
		                          : from_env;
	}();
	return k;
}

#endif   // HYDRA_HAVE_SECRET

}  // namespace

QString encode_pair(const QString &id, const QString &key_b64) {
	if (id.isEmpty() || key_b64.isEmpty())
		return QString();
	return QString::fromLatin1(id.toUtf8().toBase64()) + kSeparator + key_b64;
}

bool decode_pair(const QString &blob, QString *id, QString *key_b64) {
	const int split = blob.indexOf(kSeparator);
	if (split <= 0 || split + 1 >= blob.size())
		return false;
	const QByteArray raw_id =
	    QByteArray::fromBase64(blob.left(split).toLatin1());
	const QString key = blob.mid(split + 1);
	// A blob that decodes to an empty half is not half a pairing, it is a
	// corrupt one, and saying so beats handing back an id nobody can use.
	if (raw_id.isEmpty() || key.isEmpty())
		return false;
	if (id)
		*id = QString::fromUtf8(raw_id);
	if (key_b64)
		*key_b64 = key;
	return true;
}

#ifdef HYDRA_HAVE_SECRET

QString unavailable_reason() {
	// Asked once. It is a blocking call to the session bus, and the answer does
	// not change while the process runs -- a keyring that appears later is not
	// worth a bus round trip on every save.
	static const QString reason = [] {
		GError *err = nullptr;
		SecretService *service =
		    secret_service_get_sync(SECRET_SERVICE_NONE, nullptr, &err);
		if (!service) {
			const QString why = err && err->message
			                        ? QString::fromUtf8(err->message)
			                        : QStringLiteral("no Secret Service answered");
			if (err)
				g_error_free(err);
			return QStringLiteral(
			           "No Secret Service is running, so the KeePassXC pairing "
			           "cannot be stored encrypted and will not survive a "
			           "restart (") +
			       why + ")";
		}
		g_object_unref(service);
		return QString();
	}();
	return reason;
}

bool available() { return unavailable_reason().isEmpty(); }

bool save(const QString &id, const QString &key_b64) {
	const QString blob = encode_pair(id, key_b64);
	if (blob.isEmpty() || !available())
		return false;
	GError *err = nullptr;
	const gboolean ok = secret_password_store_sync(
	    schema(), SECRET_COLLECTION_DEFAULT,
	    "Hydra — KeePassXC association", blob.toUtf8().constData(), nullptr,
	    &err, "application", kApplication, "kind", kind().constData(), nullptr);
	if (err)
		g_error_free(err);
	return ok == TRUE;
}

bool load(QString *id, QString *key_b64) {
	if (!available())
		return false;
	GError *err = nullptr;
	gchar *value = secret_password_lookup_sync(
	    schema(), nullptr, &err, "application", kApplication, "kind", kind().constData(),
	    nullptr);
	if (err)
		g_error_free(err);
	if (!value)
		return false;
	const QString blob = QString::fromUtf8(value);
	secret_password_free(value);
	return decode_pair(blob, id, key_b64);
}

bool clear() {
	if (!available())
		return false;
	GError *err = nullptr;
	const gboolean ok = secret_password_clear_sync(
	    schema(), nullptr, &err, "application", kApplication, "kind", kind().constData(),
	    nullptr);
	if (err)
		g_error_free(err);
	return ok == TRUE;
}

bool has_pairing() {
	QString id, key;
	return load(&id, &key);
}

#else   // built without libsecret

QString unavailable_reason() {
	return QStringLiteral(
	    "Built without libsecret — the KeePassXC pairing is kept in memory "
	    "only and has to be confirmed again after a restart.");
}

bool available() { return false; }
bool save(const QString &, const QString &) { return false; }
bool load(QString *, QString *) { return false; }
bool clear() { return false; }
bool has_pairing() { return false; }

#endif  // HYDRA_HAVE_SECRET

}  // namespace credential_store
