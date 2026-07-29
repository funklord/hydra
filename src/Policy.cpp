#include "Policy.h"

namespace policy {

namespace {

struct Info { const char* name; const char* label; };

const Info kInfo[] = {
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
};

}  // namespace

const char* featureName(Feature f) {
    const int i = static_cast<int>(f);
    if (i < 0 || i >= featureCount())
        return "";
    return kInfo[i].name;
}

const char* featureLabel(Feature f) {
    const int i = static_cast<int>(f);
    if (i < 0 || i >= featureCount())
        return "";
    return kInfo[i].label;
}

Feature featureFromName(const QString& name) {
    for (int i = 0; i < featureCount(); ++i)
        if (name == QLatin1String(kInfo[i].name))
            return static_cast<Feature>(i);
    return Feature::Count;
}

}  // namespace policy
