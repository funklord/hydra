// SPDX-License-Identifier: GPL-3.0-or-later
package se.vibes.hydra;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;

/**
 * Keeps the process in the foreground while a page is playing audio.
 *
 * **Why this exists, and what it is measured to do.** Backgrounding the browser
 * with a video playing stopped the sound. Android's own audio service showed
 * the player going from `state:started` to `state:stopped` while the process
 * stayed alive -- so nothing was killed, and this is not the
 * out-of-memory problem a foreground service is usually reached for.
 *
 * What a foreground service does buy is the right to keep playing at all: an
 * app in the background with no such service is not permitted sustained media
 * playback, and the platform is entitled to stop it. That is a necessary
 * condition and, on its own, may not be a sufficient one -- the activity's
 * surface is destroyed when it stops, and a WebView with nowhere to draw may
 * pause its media for reasons of its own. Whether it is enough is a question
 * for the device rather than for this comment.
 *
 * **It runs only while something is playing.** A browser that held a permanent
 * notification would be claiming a resource it uses occasionally, and would
 * train its owner to ignore the one notification that means something is
 * happening. `HydraWebView` starts and stops it from Android's own playback
 * callback rather than from anything the page says, so it follows what the
 * device observes rather than what a script claims.
 */
public class PlaybackService extends Service {
	private static final String CHANNEL = "hydra.playback";
	private static final int    ID      = 1;

	@Override
	public IBinder onBind(Intent intent) {
		// Nothing binds to this; it exists for its lifetime, not its API.
		return null;
	}

	@Override
	public int onStartCommand(Intent intent, int flags, int startId) {
		final NotificationManager nm =
		  (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && nm != null
		    && nm.getNotificationChannel(CHANNEL) == null) {
			// LOW, so it is silent and unobtrusive: this notification reports a
			// state rather than asking for attention, and a sound to announce
			// that sound is playing would be absurd.
			final NotificationChannel c = new NotificationChannel(
			  CHANNEL, "Playing audio", NotificationManager.IMPORTANCE_LOW);
			c.setDescription("Shown while a page is playing sound in the background.");
			c.setShowBadge(false);
			nm.createNotificationChannel(c);
		}

		// Tapping it returns to the browser rather than doing nothing, which is
		// what somebody reaching for it wants: they are looking for whatever is
		// making the noise.
		final Intent open = new Intent(this, org.qtproject.qt.android.bindings.QtActivity.class);
		open.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
		final PendingIntent tap = PendingIntent.getActivity(
		  this, 0, open, PendingIntent.FLAG_IMMUTABLE);

		final Notification.Builder b =
		  Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
		    ? new Notification.Builder(this, CHANNEL)
		    : new Notification.Builder(this);
		final Notification n = b.setContentTitle("Hydra is playing audio")
		                          .setContentText("Tap to return to the page.")
		                          .setSmallIcon(android.R.drawable.ic_media_play)
		                          .setContentIntent(tap)
		                          .setOngoing(true)
		                          .build();

		startForeground(ID, n);
		// Not sticky: if the system takes this process, there is no playback
		// left to keep alive, and restarting a service to announce silence
		// would be a notification about nothing.
		return START_NOT_STICKY;
	}
}
