#pragma once

#include <QString>

// The Permissions API, answered from this browser's own policy.
//
// **`navigator.permissions.query({name:"camera"})` is how a site decides whether
// to bother asking.** A conferencing app checks it first and only calls
// `getUserMedia` if the answer is `granted`; if it is `prompt` or `denied` it
// tells the person their camera is missing and stops. Teams does exactly this,
// and says so in its own words: *"To give access, select the site information
// icon in your browser's address bar and turn on your mic."*
//
// **Neither engine answers it from the shield.** Qt WebEngine keeps its own
// permission store -- this profile deliberately sets `AskEveryTime`, so it has
// nothing standing to report -- and Android's WebView does not implement the
// query for media at all. Measured on both: with the shield set to allow, the
// page is told `prompt`. So a site that asks politely before calling is
// refused, while one that just calls gets a stream.
//
// Everything else was eliminated first, by measurement rather than reasoning:
// `enumerateDevices` reports devices on both platforms, `RTCPeerConnection`,
// `createOffer`, `getDisplayMedia` and insertable streams are all present, the
// codec list is full, the shield allows and Android grants. The only broken
// link was the question asked before the request.
//
// **This reports what would actually happen, which is what makes it a shim
// rather than a lie.** The caller passes the state it would really produce --
// `granted` only when nothing further would stand in the way.
namespace permissions_shim {

// `camera` and `microphone` are the three words the API uses: "granted",
// "denied" or "prompt".
//
// Written once and shared by both backends. Two copies of a transformation is
// the failure this project keeps recording under other names, and this one
// would be worse than most: the two platforms would answer the same question
// differently and the difference would only show on somebody's phone.
inline QString source(const QString &camera, const QString &microphone) {
	return QStringLiteral(R"JS(
(function () {
  if (!navigator.permissions || !navigator.permissions.query) return;
  var real = navigator.permissions.query.bind(navigator.permissions);
  var states = { camera: "%1", microphone: "%2" };
  navigator.permissions.query = function (desc) {
    var n = desc && desc.name;
    if (n && Object.prototype.hasOwnProperty.call(states, n)) {
      // Shaped like a PermissionStatus, because that is what callers touch:
      // `state` is what everybody reads, and the event methods are here so a
      // site that subscribes to changes does not throw on a plain object.
      return Promise.resolve({
        name: n, state: states[n], status: states[n], onchange: null,
        addEventListener: function () {},
        removeEventListener: function () {},
        dispatchEvent: function () { return false; }
      });
    }
    return real(desc);
  };
})();
)JS").arg(camera, microphone);
}


// **Names for the devices the platform left blank.**
//
// A site builds its microphone and speaker pickers out of `label`, so a device
// with an empty one is a device it cannot offer. Measured on the handset: every
// camera came back named ("camera 1, facing front" and three more), while the
// default microphone and the *only* audio output came back as empty strings.
// Teams therefore reported no speaker at all and would not let the microphone
// be turned on, on a machine where audio capture was working -- a track was
// being delivered the whole time.
//
// **This fills blanks and never overwrites.** A platform that names its devices
// keeps its names; the desktop already names all eight and is untouched by
// this. What it refuses to do is invent detail: an unnamed output is called
// "Speaker", not "Built-in speaker (stereo)", because the browser does not know
// that and a picker full of confident fictions is worse than one with blanks.
//
// Numbered only when there is more than one of a kind to tell apart, so the
// common case reads as a name rather than an inventory entry.
//
// **Wrapped in a Proxy rather than copied into plain objects.** A copy loses
// `instanceof MediaDeviceInfo` and the prototype's `toJSON`, and a site that
// checks either would break on a fix meant to help it. The proxy answers
// `label` itself and forwards everything else to the real device, binding
// methods so they still run against it.
inline QString device_labels() {
	return QStringLiteral(R"JS(
(function () {
  var md = navigator.mediaDevices;
  if (!md || !md.enumerateDevices) return;
  var real = md.enumerateDevices.bind(md);
  var names = { audioinput: "Microphone", audiooutput: "Speaker",
                videoinput: "Camera" };
  md.enumerateDevices = function () {
    return real().then(function (list) {
      var blanks = {};
      list.forEach(function (d) {
        if (!d.label) blanks[d.kind] = (blanks[d.kind] || 0) + 1;
      });
      var seen = {};
      return list.map(function (d) {
        if (d.label) return d;
        var base = names[d.kind];
        if (!base) return d;
        seen[d.kind] = (seen[d.kind] || 0) + 1;
        var name = blanks[d.kind] > 1 ? base + " " + seen[d.kind] : base;
        return new Proxy(d, {
          get: function (t, p) {
            if (p === "label") return name;
            var v = Reflect.get(t, p);
            return typeof v === "function" ? v.bind(t) : v;
          }
        });
      });
    });
  };
})();
)JS");
}

}  // namespace permissions_shim
