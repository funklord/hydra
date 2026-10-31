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
 * Keeps the process in the foreground while a download is transferring.
 *
 * A browser that leaves the screen is suspended, and a torrent or a long
 * download suspended with it simply stops -- which on a phone is most of the
 * time a download takes. Architecture sec 19.6 names this as the thing a
 * torrent on Android needs. The type is `dataSync`, which is what moving a
 * file the person asked for is.
 *
 * **Only while something is moving**, as `PlaybackService` runs only while
 * something plays: `android_downloads` starts it when a job is queued,
 * resolving or running and stops it when none is. Seeding a finished torrent
 * does not hold it -- staying resident to upload, with a notification, is a
 * cost the person did not ask for by downloading.
 *
 * It must be started while the app is in front (Android 12 refuses a start
 * from the background); a download always begins with the person in the
 * app, so the start it needs is the one that happens.
 */
public class TransferService extends Service {
	private static final String CHANNEL = "hydra.transfer";
	private static final int    ID      = 2;

	@Override
	public IBinder onBind(Intent intent) {
		return null;
	}

	@Override
	public int onStartCommand(Intent intent, int flags, int startId) {
		final NotificationManager nm =
		  (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && nm != null
		    && nm.getNotificationChannel(CHANNEL) == null) {
			final NotificationChannel c = new NotificationChannel(
			  CHANNEL, "Downloading", NotificationManager.IMPORTANCE_LOW);
			c.setDescription("Shown while a download continues in the background.");
			c.setShowBadge(false);
			nm.createNotificationChannel(c);
		}

		final Intent open = new Intent(this, org.qtproject.qt.android.bindings.QtActivity.class);
		open.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
		final PendingIntent tap = PendingIntent.getActivity(
		  this, 0, open, PendingIntent.FLAG_IMMUTABLE);

		String text = intent != null ? intent.getStringExtra("text") : null;
		if (text == null || text.isEmpty())
			text = "Tap to see the downloads.";
		final Notification.Builder b =
		  Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
		    ? new Notification.Builder(this, CHANNEL)
		    : new Notification.Builder(this);
		final Notification n = b.setContentTitle("Hydra is downloading")
		                          .setContentText(text)
		                          .setSmallIcon(android.R.drawable.stat_sys_download)
		                          .setContentIntent(tap)
		                          .setOngoing(true)
		                          .build();
		startForeground(ID, n);
		// Not sticky: a process the system took has no transfer left to keep.
		return START_NOT_STICKY;
	}

	/**
	 * Android 15 gives a `dataSync` service six hours a day, and calls this
	 * when they are spent; a service still running a few seconds later is
	 * an error that takes the app down. So it stops. The transfer carries on
	 * while the browser is in front, as it always did.
	 */
	@Override
	public void onTimeout(int startId, int fgsType) {
		stopSelf();
	}
}
