// SPDX-License-Identifier: GPL-3.0-or-later
#include "policy.h"

namespace policy {

namespace {

struct info { const char *name; const char *label; };

const info k_info[] = {
	{ "javascript",        "JavaScript"          },
	{ "cookies",           "Cookies"             },
	{ "thirdPartyCookies", "Third-party cookies" },
	{ "ads",               "Ads / trackers"      },
	{ "popups",            "Popups"              },
	{ "images",            "Images"              },
	{ "autoplay",          "Autoplay media"      },
	{ "geolocation",       "Location"            },
	{ "camera",            "Camera"              },
	{ "microphone",        "Microphone"          },
	{ "notifications",     "Notifications"       },
	{ "referer",           "Referer header"      },
	{ "autofill",          "Password autofill"   },
	{ "extractorFetch",    "Extractor may fetch" },
	{ "extractorDom",      "Extractor may read the page" },
};

}  // namespace

const char *feature_name(feature f) {
	const int i = static_cast<int>(f);
	if (i < 0 || i >= feature_count())
		return "";
	return k_info[i].name;
}

const char *feature_label(feature f) {
	const int i = static_cast<int>(f);
	if (i < 0 || i >= feature_count())
		return "";
	return k_info[i].label;
}

feature feature_from_name(const QString &name) {
	for (int i = 0; i < feature_count(); ++i)
		if (name == QLatin1String(k_info[i].name))
			return static_cast<feature>(i);
	return feature::count;
}

}  // namespace policy
