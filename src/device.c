/*
 * /net/reactivated/Fprint/Device/foo object implementation
 * Copyright (C) 2008 Daniel Drake <dsd@gentoo.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <dbus/dbus-glib-bindings.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <polkit/polkit.h>
#include <fprint.h>

#include <sys/types.h>
#include <pwd.h>
#include <errno.h>

#include "fprintd-marshal.h"
#include "fprintd.h"
#include "storage.h"

static const char *FINGERS_NAMES[] = {
	[FP_FINGER_UNKNOWN] = "unknown",
	"left-thumb",
	"left-index-finger",
	"left-middle-finger",
	"left-ring-finger",
	"left-little-finger",
	"right-thumb",
	"right-index-finger",
	"right-middle-finger",
	"right-ring-finger",
	"right-little-finger"
};

extern DBusGConnection *fprintd_dbus_conn;

static void fprint_device_claim(FprintDevice *rdev,
				const char *username,
				DBusGMethodInvocation *context);
static void fprint_device_release(FprintDevice *rdev,
	DBusGMethodInvocation *context);
static void fprint_device_verify_start(FprintDevice *rdev,
	const char *finger_name, DBusGMethodInvocation *context);
static void fprint_device_verify_stop(FprintDevice *rdev,
	DBusGMethodInvocation *context);
static void fprint_device_enroll_start(FprintDevice *rdev,
	const char *finger_name, DBusGMethodInvocation *context);
static void fprint_device_enroll_stop(FprintDevice *rdev,
	DBusGMethodInvocation *context);
static void fprint_device_list_enrolled_fingers(FprintDevice *rdev, 
						const char *username,
						DBusGMethodInvocation *context);
static void fprint_device_delete_enrolled_fingers(FprintDevice *rdev,
						  const char *username,
						  DBusGMethodInvocation *context);
static void fprint_device_delete_enrolled_fingers2(FprintDevice *rdev,
						    DBusGMethodInvocation *context);

#include "device-dbus-glue.h"

typedef enum {
	ACTION_NONE = 0,
	ACTION_IDENTIFY,
	ACTION_VERIFY,
	ACTION_ENROLL
} FprintDeviceAction;

typedef struct {
	/* current method invocation */
	DBusGMethodInvocation *context;

	/* The current user of the device, if claimed */
	char *sender;

	/* The current user of the device, or if allowed,
	 * what was passed as a username argument */
	char *username;
} SessionData;

typedef struct {
	guint32 id;
	FpDevice *dev;
	SessionData *session;

	PolkitAuthority *auth;

	/* Hashtable of connected clients */
	GHashTable *clients;

	/* Required to restart the operation on a retry failure. */
	FpPrint   *verify_data;
	GPtrArray *identify_data;
	int enroll_data;

	/* whether we're running an identify, or a verify */
	FprintDeviceAction current_action;
	GCancellable *current_cancellable;
	DBusGMethodInvocation *current_cancel_context;
	/* Whether the device was disconnected */
	gboolean disconnected;
} FprintDevicePrivate;

G_DEFINE_TYPE_WITH_CODE(FprintDevice, fprint_device, G_TYPE_OBJECT, G_ADD_PRIVATE (FprintDevice));

enum fprint_device_properties {
	FPRINT_DEVICE_CONSTRUCT_DEV = 1,
	FPRINT_DEVICE_IN_USE,
	FPRINT_DEVICE_NAME,
	FPRINT_DEVICE_NUM_ENROLL,
	FPRINT_DEVICE_SCAN_TYPE
};

enum fprint_device_signals {
	SIGNAL_VERIFY_STATUS,
	SIGNAL_VERIFY_FINGER_SELECTED,
	SIGNAL_ENROLL_STATUS,
	NUM_SIGNALS,
};

static guint32 last_id = ~0;
static guint signals[NUM_SIGNALS] = { 0, };

static void session_data_free(SessionData *session)
{
	g_clear_pointer(&session->sender, g_free);
	g_clear_pointer(&session->username, g_free);
	g_nullify_pointer((gpointer *) &session->context);
	g_free(session);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SessionData, session_data_free);

static void fprint_device_finalize(GObject *object)
{
	FprintDevice *self = (FprintDevice *) object;
	FprintDevicePrivate *priv = fprint_device_get_instance_private(self);

	g_hash_table_destroy (priv->clients);
	g_clear_pointer(&priv->session, session_data_free);
	/* FIXME close and stuff */

	G_OBJECT_CLASS(fprint_device_parent_class)->finalize(object);
}

static void fprint_device_set_property(GObject *object, guint property_id,
	const GValue *value, GParamSpec *pspec)
{
	FprintDevice *self = (FprintDevice *) object;
	FprintDevicePrivate *priv = fprint_device_get_instance_private(self);

	switch (property_id) {
	case FPRINT_DEVICE_CONSTRUCT_DEV:
		priv->dev = g_value_dup_object(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
		break;
	}
}

static void fprint_device_get_property(GObject *object, guint property_id,
				       GValue *value, GParamSpec *pspec)
{
	FprintDevice *self = (FprintDevice *) object;
	FprintDevicePrivate *priv = fprint_device_get_instance_private(self);

	switch (property_id) {
	case FPRINT_DEVICE_CONSTRUCT_DEV:
		g_value_set_object(value, priv->dev);
		break;
	case FPRINT_DEVICE_IN_USE:
		g_value_set_boolean(value, g_hash_table_size (priv->clients) != 0);
		break;
	case FPRINT_DEVICE_NAME:
		g_value_set_static_string (value, fp_device_get_name (priv->dev));
		break;
	case FPRINT_DEVICE_NUM_ENROLL:
		if (priv->dev)
			g_value_set_int (value, fp_device_get_nr_enroll_stages (priv->dev));
		else
			g_value_set_int (value, -1);
		break;
	case FPRINT_DEVICE_SCAN_TYPE: {
		const char *type;

		if (fp_device_get_scan_type (priv->dev) == FP_SCAN_TYPE_PRESS)
			type = "press";
		else
			type = "swipe";

		g_value_set_static_string (value, type);
		break;
	}
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
		break;
	}
}

static void fprint_device_class_init(FprintDeviceClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
	GParamSpec *pspec;

	dbus_g_object_type_install_info(FPRINT_TYPE_DEVICE,
		&dbus_glib_fprint_device_object_info);

	gobject_class->finalize = fprint_device_finalize;
	gobject_class->set_property = fprint_device_set_property;
	gobject_class->get_property = fprint_device_get_property;

	pspec = g_param_spec_object("dev", "Device",
				     "Set device construction property",
				     FP_TYPE_DEVICE,
				     G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_READABLE);
	g_object_class_install_property(gobject_class,
					FPRINT_DEVICE_CONSTRUCT_DEV, pspec);

	pspec = g_param_spec_boolean("in-use", "In use",
				     "Whether the device is currently in use", FALSE,
				     G_PARAM_READABLE);
	g_object_class_install_property(gobject_class,
					FPRINT_DEVICE_IN_USE, pspec);

	pspec = g_param_spec_string("name", "Name",
				    "The product name of the device", NULL,
				    G_PARAM_READABLE);
	g_object_class_install_property(gobject_class,
					FPRINT_DEVICE_NAME, pspec);

	pspec = g_param_spec_string("scan-type", "Scan Type",
				    "The scan type of the device", "press",
				    G_PARAM_READABLE);
	g_object_class_install_property(gobject_class,
					FPRINT_DEVICE_SCAN_TYPE, pspec);

	pspec = g_param_spec_int("num-enroll-stages", "Number of enrollments stages",
				  "Number of enrollment stages for the device.",
				  -1, G_MAXINT, -1, G_PARAM_READABLE);
	g_object_class_install_property(gobject_class,
					FPRINT_DEVICE_NUM_ENROLL, pspec);

	signals[SIGNAL_VERIFY_STATUS] = g_signal_new("verify-status",
		G_TYPE_FROM_CLASS(gobject_class), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
		fprintd_marshal_VOID__STRING_BOOLEAN, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);
	signals[SIGNAL_ENROLL_STATUS] = g_signal_new("enroll-status",
		G_TYPE_FROM_CLASS(gobject_class), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
		fprintd_marshal_VOID__STRING_BOOLEAN, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);
	signals[SIGNAL_VERIFY_FINGER_SELECTED] = g_signal_new("verify-finger-selected",
		G_TYPE_FROM_CLASS(gobject_class), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
		g_cclosure_marshal_VOID__STRING, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void fprint_device_init(FprintDevice *device)
{
	FprintDevicePrivate *priv = fprint_device_get_instance_private(device);
	priv->id = ++last_id;

	/* Setup PolicyKit */
	priv->auth = polkit_authority_get_sync (NULL, NULL);
	priv->clients = g_hash_table_new_full (g_str_hash,
					       g_str_equal,
					       g_free,
					       NULL);
}

FprintDevice *fprint_device_new(FpDevice *dev)
{
	return g_object_new(FPRINT_TYPE_DEVICE, "dev", dev, NULL);
}

guint32 _fprint_device_get_id(FprintDevice *rdev)
{
	FprintDevicePrivate *priv = fprint_device_get_instance_private(rdev);

	return priv->id;
}

static const char *
finger_num_to_name (int finger_num)
{
	if (finger_num == -1)
		return "any";
	if (!FP_FINGER_IS_VALID (finger_num))
		return NULL;
	return FINGERS_NAMES[finger_num];
}

static int
finger_name_to_num (const char *finger_name)
{
	guint i;

	if (finger_name == NULL || *finger_name == '\0' || g_str_equal (finger_name, "any"))
		return -1;

	for (i = FP_FINGER_FIRST; i <= FP_FINGER_LAST; i++) {
		if (g_str_equal (finger_name, FINGERS_NAMES[i]))
			return i;
	}

	/* Invalid, let's try that */
	return -1;
}

static const char *
verify_result_to_name (gboolean match, GError *error)
{
	if (!error) {
		if (match)
			return "verify-match";
		else
			return "verify-no-match";
	}

	if (error->domain == FP_DEVICE_RETRY) {
		switch (error->code) {
			case FP_DEVICE_RETRY_TOO_SHORT:
				return "verify-swipe-too-short";
			case FP_DEVICE_RETRY_CENTER_FINGER:
				return "verify-finger-not-centered";
			case FP_DEVICE_RETRY_REMOVE_FINGER:
				return "verify-remove-and-retry";
			default:
				return "verify-retry-scan";
		}
	} else {
		/* Which errors should be mapped to disconnection?
		 * Are drivers/libfprint/fprintd really in agreement here?
		 */
		if (g_error_matches (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO))
			return "verify-disconnect";

		return "verify-unknown-error";
	}
}

static const char *
enroll_result_to_name (gboolean completed, gboolean enrolled, GError *error)
{
	if (!error) {
		if (!completed)
			return "enroll-stage-passed";
		else if (enrolled)
			return "enroll-completed";
		else
			return "enroll-failed";
	}

	if (error->domain == FP_DEVICE_RETRY) {
		switch (error->code) {
			case FP_DEVICE_RETRY_TOO_SHORT:
				return "enroll-swipe-too-short";
			case FP_DEVICE_RETRY_CENTER_FINGER:
				return "enroll-finger-not-centered";
			case FP_DEVICE_RETRY_REMOVE_FINGER:
				return "verify-remove-and-retry";
			default:
				return "enroll-remove-and-retry";
		}
	} else {
		/* Which errors should be mapped to disconnection?
		 * Are drivers/libfprint/fprintd really in agreement here?
		 */
		if (g_error_matches (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO))
			return "enroll-disconnected";
		else if (g_error_matches (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_FULL))
			return "enroll-data-full";
		else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			return "enroll-failed";

		return "enroll-unknown-error";
	}
}

static void
set_disconnected (FprintDevicePrivate *priv, const char *res)
{
	if (g_str_equal (res, "enroll-disconnected") ||
	    g_str_equal (res, "verify-disconnected"))
		priv->disconnected = TRUE;
}

static gboolean
_fprint_device_check_claimed (FprintDevice *rdev,
			      DBusGMethodInvocation *context,
			      GError **error)
{
	FprintDevicePrivate *priv = fprint_device_get_instance_private(rdev);
	char *sender;
	gboolean retval;

	/* The device wasn't claimed, exit */
	if (priv->session == NULL) {
		g_set_error (error, FPRINT_ERROR, FPRINT_ERROR_CLAIM_DEVICE,
			     _("Device was not claimed before use"));
		return FALSE;
	}

	sender = dbus_g_method_get_sender (context);
	retval = g_str_equal (sender, priv->session->sender);
	g_free (sender);

	if (retval == FALSE ||
	    priv->session->context != NULL) {
		g_set_error (error, FPRINT_ERROR, FPRINT_ERROR_ALREADY_IN_USE,
			     _("Device already in use by another user"));
	}

	return retval;
}

static gboolean
_fprint_device_check_polkit_for_action (FprintDevice *rdev, DBusGMethodInvocation *context, const char *action, GError **error)
{
	FprintDevicePrivate *priv = fprint_device_get_instance_private(rdev);
	char *sender;
	PolkitSubject *subject;
	PolkitAuthorizationResult *result;
	GError *_error = NULL;

	/* Check that caller is privileged */
	sender = dbus_g_method_get_sender (context);
	subject = polkit_system_bus_name_new (sender);
	g_free (sender);

	result = polkit_authority_check_authorization_sync (priv->auth,
                                                            subject,
                                                            action,
							    NULL,
                                                            POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION,
					                    NULL, &_error);
	g_object_unref (subject);

	if (result == NULL) {
		g_set_error (error, FPRINT_ERROR,
			     FPRINT_ERROR_PERMISSION_DENIED,
			     "Not Authorized: %s", _error->message);
		g_error_free (_error);
		return FALSE;
	}

	if (!polkit_authorization_result_get_is_authorized (result)) {
		g_set_error (error, FPRINT_ERROR,
			     FPRINT_ERROR_PERMISSION_DENIED,
			     "Not Authorized: %s", action);
		g_object_unref (result);
		return FALSE;
	}

	g_object_unref (result);

	return TRUE;
}

static gboolean
_fprint_device_check_polkit_for_actions (FprintDevice *rdev,
					 DBusGMethodInvocation *context,
					 const char *action1,
					 const char *action2,
					 GError **error)
{
	if (_fprint_device_check_polkit_for_action (rdev, context, action1, error) != FALSE)
		return TRUE;

	g_error_free (*error);
	*error = NULL;

	return _fprint_device_check_polkit_for_action (rdev, context, action2, error);
}

static char *
_fprint_device_check_for_username (FprintDevice *rdev,
				   DBusGMethodInvocation *context,
				   const char *username,
				   char **ret_sender,
				   GError **error)
{
	DBusConnection *conn;
	DBusError dbus_error;
	char *sender;
	unsigned long uid;
	struct passwd *user;

	/* Get details about the current sender, and username/uid */
	conn = dbus_g_connection_get_connection (fprintd_dbus_conn);
	sender = dbus_g_method_get_sender (context);
	dbus_error_init (&dbus_error);
	uid = dbus_bus_get_unix_user (conn, sender, &dbus_error);

	if (dbus_error_is_set(&dbus_error)) {
		g_free (sender);
		dbus_set_g_error (error, &dbus_error);
		return NULL;
	}

	user = getpwuid (uid);
	if (user == NULL) {
		g_free (sender);
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_INTERNAL,
			    "Failed to get information about user UID %lu", uid);
		return NULL;
	}

	/* The current user is usually allowed to access their
	 * own data, this should be followed by PolicyKit checks
	 * anyway */
	if (username == NULL || *username == '\0' || g_str_equal (username, user->pw_name)) {
		if (ret_sender != NULL)
			*ret_sender = sender;
		else
			g_free (sender);
		return g_strdup (user->pw_name);
	}

	/* If we're not allowed to set a different username,
	 * then fail */
	if (_fprint_device_check_polkit_for_action (rdev, context, "net.reactivated.fprint.device.setusername", error) == FALSE) {
		g_free (sender);
		return NULL;
	}

	if (ret_sender != NULL)
		*ret_sender = sender;
	else
		g_free (sender);

	return g_strdup (username);
}

static void
_fprint_device_client_vanished (GDBusConnection *connection,
				const char *name,
				FprintDevice *rdev)
{
	g_autoptr(GError) error = NULL;
	FprintDevicePrivate *priv = fprint_device_get_instance_private(rdev);

	/* Was that the client that claimed the device? */
	if (priv->session != NULL &&
	    g_strcmp0 (priv->session->sender, name) == 0) {
		while (priv->current_action != ACTION_NONE) {
			g_cancellable_cancel (priv->current_cancellable);

			g_main_context_iteration (NULL, TRUE);
		}

		if (!fp_device_close_sync (priv->dev, NULL, &error))
			g_warning ("Error closing device after disconnect: %s", error->message);

		g_clear_pointer(&priv->session, session_data_free);
	}
	g_hash_table_remove (priv->clients, name);

	if (g_hash_table_size (priv->clients) == 0) {
		g_object_notify (G_OBJECT (rdev), "in-use");
	}
}

static void
_fprint_device_add_client (FprintDevice *rdev, const char *sender)
{
	FprintDevicePrivate *priv = fprint_device_get_instance_private(rdev);
	guint id;

	id = GPOINTER_TO_UINT (g_hash_table_lookup (priv->clients, sender));
	if (id == 0) {
		id = g_bus_watch_name (G_BUS_TYPE_SYSTEM,
				       sender,
				       G_BUS_NAME_WATCHER_FLAGS_NONE,
				       NULL,
				       (GBusNameVanishedCallback) _fprint_device_client_vanished,
				       rdev,
				       NULL);
		g_hash_table_insert (priv->clients, g_strdup (sender), GUINT_TO_POINTER(id));
		g_object_notify (G_OBJECT (rdev), "in-use");
	}
}

static void dev_open_cb(FpDevice *dev, GAsyncResult *res, void *user_data)
{
	g_autoptr(GError) error = NULL;
	FprintDevice *rdev = user_data;
	FprintDevicePrivate *priv = fprint_device_get_instance_private(rdev);
	DBusGMethodInvocation *context = g_steal_pointer(&priv->session->context);

	if (!fp_device_open_finish (dev, res, &error)) {
		g_autoptr(GError) dbus_error = NULL;

		dbus_error = g_error_new (FPRINT_ERROR,
		                          FPRINT_ERROR_INTERNAL,
		                          "Open failed with error: %s", error->message);
		dbus_g_method_return_error(context, dbus_error);
		g_clear_pointer(&priv->session, session_data_free);
		return;
	}

	g_debug("claimed device %d", priv->id);

	dbus_g_method_return(context);
}

static void fprint_device_claim(FprintDevice *rdev,
				const char *username,
				DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = fprint_device_get_instance_private(rdev);
	GError *error = NULL;
	char *sender, *user;

	/* Is it already claimed? */
	if (priv->session) {
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_ALREADY_IN_USE,
			    "Device was already claimed");
		dbus_g_method_return_error(context, error);
		g_error_free(error);
		return;
	}

	g_assert_null(priv->session);

	sender = NULL;
	user = _fprint_device_check_for_username (rdev,
						  context,
						  username,
						  &sender,
						  &error);
	if (user == NULL) {
		g_free (sender);
		dbus_g_method_return_error (context, error);
		g_error_free (error);
		return;
	}

	if (_fprint_device_check_polkit_for_actions (rdev, context,
						     "net.reactivated.fprint.device.verify",
						     "net.reactivated.fprint.device.enroll",
						     &error) == FALSE) {
		g_free (sender);
		g_free (user);
		dbus_g_method_return_error (context, error);
		g_error_free (error);
		return;
	}

	_fprint_device_add_client (rdev, sender);

	priv->session = g_new0(SessionData, 1);
	priv->session->context = context;
	priv->session->username = user;
	priv->session->sender = sender;

	g_debug ("user '%s' claiming the device: %d", priv->session->username, priv->id);

	fp_device_open (priv->dev, NULL, (GAsyncReadyCallback) dev_open_cb, rdev);
}

static void dev_close_cb(FpDevice *dev, GAsyncResult *res, void *user_data)
{
	g_autoptr(GError) error = NULL;
	FprintDevice *rdev = user_data;
	FprintDevicePrivate *priv = fprint_device_get_instance_private(rdev);
	g_autoptr(SessionData) session = g_steal_pointer(&priv->session);
	DBusGMethodInvocation *context = g_steal_pointer(&session->context);

	if (!fp_device_close_finish (dev, res, &error)) {
		g_autoptr(GError) dbus_error = NULL;

		dbus_error = g_error_new (FPRINT_ERROR,
		                          FPRINT_ERROR_INTERNAL,
		                          "Release failed with error: %s", error->message);
		dbus_g_method_return_error(context, dbus_error);
		return;
	}

	g_debug("released device %d", priv->id);

	dbus_g_method_return(context);
}

static void fprint_device_release(FprintDevice *rdev,
	DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = fprint_device_get_instance_private(rdev);
	GError *error = NULL;

	if (_fprint_device_check_claimed(rdev, context, &error) == FALSE) {
		dbus_g_method_return_error (context, error);
		g_error_free(error);
		return;
	}

	/* People that can claim can also release */
	if (_fprint_device_check_polkit_for_actions (rdev, context,
						     "net.reactivated.fprint.device.verify",
						     "net.reactivated.fprint.device.enroll",
						     &error) == FALSE) {
		dbus_g_method_return_error (context, error);
		g_error_free(error);
		return;
	}

	priv->session->context = context;
	fp_device_close (priv->dev, NULL, (GAsyncReadyCallback) dev_close_cb, rdev);
}

static void verify_cb(FpDevice *dev, GAsyncResult *res, void *user_data)
{
	g_autoptr(GError) error = NULL;
	FprintDevice *rdev = user_data;
	FprintDevicePrivate *priv = fprint_device_get_instance_private(rdev);
	gboolean success;
	const char *name;
	gboolean match;

	success = fp_device_verify_finish (dev, res, &match, NULL, &error);
	g_assert (!!success == !error);
	name = verify_result_to_name (match, error);

	g_debug("verify_cb: result %s", name);

	set_disconnected (priv, name);

	/* Automatically restart the operation for retry failures */
	if (error && error->domain == FP_DEVICE_RETRY) {
		g_signal_emit(rdev, signals[SIGNAL_VERIFY_STATUS], 0, name, FALSE);

		/* TODO: Support early match result callback from libfprint */
		fp_device_verify (priv->dev,
				  priv->verify_data,
				  priv->current_cancellable,
				  NULL, NULL, NULL,
				  (GAsyncReadyCallback) verify_cb,
				  rdev);
	} else {
		g_clear_object (&priv->verify_data);
		g_signal_emit(rdev, signals[SIGNAL_VERIFY_STATUS], 0, name, TRUE);

		if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning ("Device reported an error during verify: %s", error->message);

		/* Return the cancellation or reset action right away if vanished. */
		if (priv->current_cancel_context) {
			dbus_g_method_return(priv->current_cancel_context);
			priv->current_cancel_context = NULL;
			priv->current_action = ACTION_NONE;
		} else if (g_cancellable_is_cancelled (priv->current_cancellable)) {
			priv->current_action = ACTION_NONE;
		}

		g_clear_object (&priv->current_cancellable);
	}
}

static void identify_cb(FpDevice *dev, GAsyncResult *res, void *user_data)
{
	g_autoptr(GError) error = NULL;
	FprintDevice *rdev = user_data;
	FprintDevicePrivate *priv = fprint_device_get_instance_private(rdev);
	const char *name;
	gboolean success;
	FpPrint *match;

	success = fp_device_identify_finish (dev, res, &match, NULL, &error);
	g_assert (!!success == !error);
	name = verify_result_to_name (match != NULL, error);

	g_debug("verify_cb: result %s", name);

	set_disconnected (priv, name);

	/* Automatically restart the operation for retry failures */
	if (error && error->domain == FP_DEVICE_RETRY) {
		g_signal_emit (rdev, signals[SIGNAL_VERIFY_STATUS], 0, name, FALSE);

		/* TODO: Support early match result callback from libfprint */
		fp_device_identify (priv->dev,
				    priv->identify_data,
				    priv->current_cancellable,
				    NULL, NULL, NULL,
				    (GAsyncReadyCallback) identify_cb,
				    rdev);
	} else {
		g_clear_pointer (&priv->identify_data, g_ptr_array_unref);
		g_signal_emit (rdev, signals[SIGNAL_VERIFY_STATUS], 0, name, TRUE);

		if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning ("Device reported an error during identify: %s", error->message);

		/* Return the cancellation or reset action right away if vanished. */
		if (priv->current_cancel_context) {
			dbus_g_method_return(priv->current_cancel_context);
			priv->current_cancel_context = NULL;
			priv->current_action = ACTION_NONE;
		} else if (g_cancellable_is_cancelled (priv->current_cancellable)) {
			priv->current_action = ACTION_NONE;
		}

		g_clear_object (&priv->current_cancellable);
	}
}

static void fprint_device_verify_start(FprintDevice *rdev,
	const char *finger_name, DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = fprint_device_get_instance_private(rdev);
	g_autoptr(GPtrArray) gallery = NULL;
	g_autoptr(FpPrint) print = NULL;
	g_autoptr(GError) error = NULL;
	guint finger_num = finger_name_to_num (finger_name);

	if (_fprint_device_check_claimed(rdev, context, &error) == FALSE) {
		dbus_g_method_return_error (context, error);
		return;
	}

	if (_fprint_device_check_polkit_for_action (rdev, context, "net.reactivated.fprint.device.verify", &error) == FALSE) {
		dbus_g_method_return_error (context, error);
		return;
	}

	if (priv->current_action != ACTION_NONE) {
		if (priv->current_action == ACTION_ENROLL) {
			g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_ALREADY_IN_USE,
				    "Enrollment in progress");
		} else {
			g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_ALREADY_IN_USE,
				    "Verification already in progress");
		}
		dbus_g_method_return_error(context, error);
		return;
	}

	if (finger_num == -1) {
		GSList *prints;

		prints = store.discover_prints(priv->dev, priv->session->username);
		if (prints == NULL) {
			g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_NO_ENROLLED_PRINTS,
				    "No fingerprints enrolled");
			dbus_g_method_return_error(context, error);
			return;
		}
		if (fp_device_supports_identify (priv->dev)) {
			GSList *l;

			gallery = g_ptr_array_new_with_free_func (g_object_unref);

			for (l = prints; l != NULL; l = l->next) {
				g_debug ("adding finger %d to the gallery", GPOINTER_TO_INT (l->data));
				store.print_data_load(priv->dev, GPOINTER_TO_INT (l->data),
						      priv->session->username, &print);

				if (print)
					g_ptr_array_add (gallery, g_steal_pointer (&print));
			}
		} else {
			finger_num = GPOINTER_TO_INT (prints->data);
		}
		g_slist_free(prints);
	}

	if (fp_device_supports_identify (priv->dev) && finger_num == -1) {
		if (gallery->len == 0) {
			g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_NO_ENROLLED_PRINTS,
				    "No fingerprints on that device");
			dbus_g_method_return_error(context, error);
			return;
		}
		priv->current_action = ACTION_IDENTIFY;

		g_debug ("start identification device %d", priv->id);
		priv->current_cancellable = g_cancellable_new ();
		priv->identify_data = g_ptr_array_ref (gallery);
		/* TODO: Support early match result callback from libfprint */
		fp_device_identify (priv->dev, gallery, priv->current_cancellable,
		                    NULL, NULL, NULL,
		                    (GAsyncReadyCallback) identify_cb, rdev);
	} else {
		priv->current_action = ACTION_VERIFY;

		g_debug("start verification device %d finger %d", priv->id, finger_num);

		store.print_data_load(priv->dev, finger_num,
				      priv->session->username, &print);

		if (!print) {
			g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_INTERNAL,
				    "No such print %d", finger_num);
			dbus_g_method_return_error(context, error);
			return;
		}

		priv->current_cancellable = g_cancellable_new ();
		priv->verify_data = g_object_ref (print);
		/* TODO: Support early match result callback from libfprint */
		fp_device_verify (priv->dev, print, priv->current_cancellable,
		                  NULL, NULL, NULL,
		                  (GAsyncReadyCallback) verify_cb, rdev);
	}

	/* Emit VerifyFingerSelected telling the front-end which finger
	 * we selected for auth */
	g_signal_emit(rdev, signals[SIGNAL_VERIFY_FINGER_SELECTED],
		      0, finger_num_to_name (finger_num));

	dbus_g_method_return(context);
}

static void fprint_device_verify_stop(FprintDevice *rdev,
	DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = fprint_device_get_instance_private(rdev);
	GError *error = NULL;

	if (_fprint_device_check_claimed(rdev, context, &error) == FALSE) {
		dbus_g_method_return_error (context, error);
		g_error_free(error);
		return;
	}

	if (_fprint_device_check_polkit_for_action (rdev, context, "net.reactivated.fprint.device.verify", &error) == FALSE) {
		dbus_g_method_return_error (context, error);
		g_error_free(error);
		return;
	}

	if (priv->current_action == ACTION_NONE) {
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_NO_ACTION_IN_PROGRESS,
			    "No verification in progress");
		dbus_g_method_return_error(context, error);
		g_error_free (error);
		return;
	}

	if (priv->current_cancellable) {
		/* We return only when the action was cancelled */
		g_cancellable_cancel (priv->current_cancellable);
		priv->current_cancel_context = context;
	} else {
		dbus_g_method_return (context);
		priv->current_action = ACTION_NONE;
	}
}

static void enroll_progress_cb(FpDevice *dev,
                               gint      completed_stages,
                               FpPrint  *print,
                               gpointer  user_data,
                               GError   *error)
{
	FprintDevice *rdev = user_data;
	const char *name = enroll_result_to_name (FALSE, FALSE, error);

	g_debug("enroll_stage_cb: result %s", name);

	if (completed_stages < fp_device_get_nr_enroll_stages (dev))
		g_signal_emit(rdev, signals[SIGNAL_ENROLL_STATUS], 0, name, FALSE);
}

static gboolean try_delete_print(FprintDevice *rdev)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) device_prints = NULL;
	FprintDevicePrivate *priv = fprint_device_get_instance_private(rdev);
	GSList *users, *user;

	device_prints = fp_device_list_prints_sync (priv->dev, NULL, &error);
	if (!device_prints) {
		g_warning ("Failed to query prints: %s", error->message);
		return FALSE;
	}

	g_debug ("Device has %d prints stored", device_prints->len);

	users = store.discover_users();

	for (user = users; user; user = user->next) {
		const char *username = user->data;
		GSList *fingers, *finger;

		fingers = store.discover_prints (priv->dev, username);

		for (finger = fingers; finger; finger = finger->next) {
			g_autoptr(FpPrint) print = NULL;
			guint index;

			store.print_data_load (priv->dev,
			                       GPOINTER_TO_INT (fingers->data),
			                       username,
			                       &print);

			if (!print)
				continue;

			if (!g_ptr_array_find_with_equal_func (device_prints,
			                                       print,
			                                       (GEqualFunc) fp_print_equal,
			                                       &index))
				continue;

			/* Found an equal print, remove it */
			g_ptr_array_remove_index (device_prints, index);
		}

		g_slist_free (fingers);
	}

	g_slist_free_full (users, g_free);

	g_debug ("Device has %d prints stored that we do not need", device_prints->len);
	if (device_prints->len == 0)
		return FALSE;

	/* Just delete the first print in the list at this point.
	 * We could be smarter and fetch some more metadata. */
	fp_device_delete_print_sync (priv->dev,
				     g_ptr_array_index (device_prints, 0),
				     NULL,
				     &error);

	if (error) {
		g_warning ("Failed to garbage collect a print: %s", error->message);
		return FALSE;
	}

	return TRUE;
}

static FpPrint*
fprint_device_create_enroll_template(FprintDevice *rdev, gint finger_num)
{
	FprintDevicePrivate *priv = fprint_device_get_instance_private(rdev);
	FpPrint *template = NULL;
	GDateTime *datetime = NULL;
	GDate *date = NULL;
	gint year, month, day;

	template = fp_print_new (priv->dev);
	fp_print_set_finger (template, finger_num);
	fp_print_set_username (template, priv->session->username);
	datetime = g_date_time_new_now_local ();
	g_date_time_get_ymd (datetime, &year, &month, &day);
	date = g_date_new_dmy (day, month, year);
	fp_print_set_enroll_date (template, date);
	g_date_free (date);
	g_date_time_unref (datetime);

	return template;
}

static void enroll_cb(FpDevice *dev, GAsyncResult *res, void *user_data)
{
	g_autoptr(GError) error = NULL;
	FprintDevice *rdev = user_data;
	FprintDevicePrivate *priv = fprint_device_get_instance_private(rdev);
	g_autoptr(FpPrint) print = NULL;
	const char *name;

	print = fp_device_enroll_finish (dev, res, &error);

	/* We need to special case the issue where the on device storage
	 * is completely full. In that case, we check whether we can delete
	 * a print that is not coming from us; assuming it is from an old
	 * installation.
	 * We do this synchronously, which is not great but should be good
	 * enough. */
	if (g_error_matches (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_FULL)) {
		g_debug ("Device storage is full, trying to garbage collect old prints");
		if (try_delete_print (rdev)) {
			/* Success? Then restart the operation */
			fp_device_enroll (priv->dev,
			                  fprint_device_create_enroll_template (rdev, priv->enroll_data),
			                  priv->current_cancellable,
			                  enroll_progress_cb,
			                  rdev,
			                  NULL,
			                  (GAsyncReadyCallback) enroll_cb,
			                  rdev);
			return;
		}
	}

	name = enroll_result_to_name (TRUE, print != NULL, error);

	g_debug ("enroll_cb: result %s", name);

	if (print) {
		int r;
		r = store.print_data_save(print);
		if (r < 0)
			name = "enroll-failed";
	}

	set_disconnected (priv, name);

	g_signal_emit(rdev, signals[SIGNAL_ENROLL_STATUS], 0, name, TRUE);

	if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		g_warning ("Device reported an error during enroll: %s", error->message);

	/* Return the cancellation or reset action right away if vanished. */
	if (priv->current_cancel_context) {
		dbus_g_method_return(priv->current_cancel_context);
		priv->current_cancel_context = NULL;
		priv->current_action = ACTION_NONE;
	} else if (g_cancellable_is_cancelled (priv->current_cancellable)) {
		priv->current_action = ACTION_NONE;
	}
	g_clear_object (&priv->current_cancellable);
}


static void fprint_device_enroll_start(FprintDevice *rdev,
	const char *finger_name, DBusGMethodInvocation *context)
{
	g_autoptr(GError) error = NULL;
	FprintDevicePrivate *priv = fprint_device_get_instance_private(rdev);
	int finger_num = finger_name_to_num (finger_name);

	if (finger_num == -1) {
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_INVALID_FINGERNAME,
			    "Invalid finger name");
		dbus_g_method_return_error(context, error);
		return;
	}

	if (_fprint_device_check_claimed(rdev, context, &error) == FALSE) {
		dbus_g_method_return_error (context, error);
		return;
	}

	if (_fprint_device_check_polkit_for_action (rdev, context, "net.reactivated.fprint.device.enroll", &error) == FALSE) {
		dbus_g_method_return_error (context, error);
		return;
	}

	if (priv->current_action != ACTION_NONE) {
		if (priv->current_action == ACTION_ENROLL) {
			g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_ALREADY_IN_USE,
				    "Enrollment already in progress");
		} else {
			g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_ALREADY_IN_USE,
				    "Verification in progress");
		}
		dbus_g_method_return_error(context, error);
		return;
	}

	g_debug("start enrollment device %d finger %d", priv->id, finger_num);

	priv->current_cancellable = g_cancellable_new ();
	priv->enroll_data = finger_num;
	fp_device_enroll (priv->dev,
	                  fprint_device_create_enroll_template (rdev, priv->enroll_data),
	                  priv->current_cancellable,
	                  enroll_progress_cb,
	                  rdev,
	                  NULL,
	                  (GAsyncReadyCallback) enroll_cb,
	                  rdev);

	priv->current_action = ACTION_ENROLL;

	dbus_g_method_return(context);
}

static void fprint_device_enroll_stop(FprintDevice *rdev,
	DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = fprint_device_get_instance_private(rdev);
	GError *error = NULL;

	if (_fprint_device_check_claimed(rdev, context, &error) == FALSE) {
		dbus_g_method_return_error (context, error);
		g_error_free (error);
		return;
	}

	if (_fprint_device_check_polkit_for_action (rdev, context, "net.reactivated.fprint.device.enroll", &error) == FALSE) {
		dbus_g_method_return_error (context, error);
		g_error_free (error);
		return;
	}

	if (priv->current_action != ACTION_ENROLL) {
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_NO_ACTION_IN_PROGRESS,
			    "No enrollment in progress");
		dbus_g_method_return_error(context, error);
		g_error_free (error);
		return;
	}

	if (priv->current_cancellable) {
		/* We return only when the action was cancelled */
		g_cancellable_cancel (priv->current_cancellable);
		priv->current_cancel_context = context;
	} else {
		dbus_g_method_return (context);
		priv->current_action = ACTION_NONE;
	}
}

static void fprint_device_list_enrolled_fingers(FprintDevice *rdev,
						const char *username,
						DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = fprint_device_get_instance_private(rdev);
	GError *error = NULL;
	GSList *prints;
	GSList *item;
	GPtrArray *ret;
	char *user, *sender;

	user = _fprint_device_check_for_username (rdev,
						  context,
						  username,
						  NULL,
						  &error);
	if (user == NULL) {
		dbus_g_method_return_error (context, error);
		g_error_free (error);
		return;
	}

	if (_fprint_device_check_polkit_for_action (rdev, context, "net.reactivated.fprint.device.verify", &error) == FALSE) {
		g_free (user);
		dbus_g_method_return_error (context, error);
		g_error_free (error);
		return;
	}

	sender = dbus_g_method_get_sender (context);
	_fprint_device_add_client (rdev, sender);
	g_free (sender);

	prints = store.discover_prints(priv->dev, user);
	g_free (user);
	if (!prints) {
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_NO_ENROLLED_PRINTS,
			"Failed to discover prints");
		dbus_g_method_return_error(context, error);
		g_error_free (error);
		return;
	}

	ret = g_ptr_array_new ();
	for (item = prints; item; item = item->next) {
		int finger_num = GPOINTER_TO_INT (item->data);
		g_ptr_array_add (ret, g_strdup (finger_num_to_name (finger_num)));
	}
	g_ptr_array_add (ret, NULL);

	g_slist_free(prints);

	dbus_g_method_return(context, g_ptr_array_free (ret, FALSE));
}

static void delete_enrolled_fingers(FprintDevice *rdev, const char *user)
{
	FprintDevicePrivate *priv = fprint_device_get_instance_private(rdev);
	guint i;

	/* First try deleting the print from the device, we don't consider it
	 * fatal if this does not work. */
	if (fp_device_has_storage (priv->dev)) {
		g_autoptr(GSList) prints = NULL;
		GSList *l;

		prints = store.discover_prints(priv->dev, user);

		for (l = prints; l != NULL; l = l->next) {
			g_autoptr(FpPrint) print = NULL;

			store.print_data_load(priv->dev,
			                      GPOINTER_TO_INT (l->data),
			                      user,
			                      &print);

			if (print) {
				g_autoptr(GError) error = NULL;

				if (!fp_device_delete_print_sync (priv->dev, print, NULL, &error)) {
					g_warning ("Error deleting print from device: %s", error->message);
					g_warning ("This might indicate an issue in the libfprint driver or in the fingerprint device.");
				}
			}
		}
	}

	for (i = FP_FINGER_FIRST; i <= FP_FINGER_LAST; i++) {
		store.print_data_delete(priv->dev, i, user);
	}
}

#ifdef __linux__
static void log_offending_client(DBusGMethodInvocation *context)
{
	g_autofree char *sender = NULL;
	g_autofree char *path = NULL;
	g_autofree char *content = NULL;
	DBusGProxy *proxy = NULL;
	guint pid = 0;

	sender = dbus_g_method_get_sender(context);
	proxy = dbus_g_proxy_new_for_name (fprintd_dbus_conn,
					    "org.freedesktop.DBus",
					    "/org/freedesktop/DBus",
					    "org.freedesktop.DBus");

	if (!dbus_g_proxy_call(proxy,
			       "GetConnectionUnixProcessID",
			       NULL,
			       G_TYPE_STRING,
			       sender,
			       G_TYPE_INVALID,
			       G_TYPE_UINT,
			       &pid,
			       G_TYPE_INVALID)) {
		goto out;
	}

	path = g_strdup_printf ("/proc/%u/comm", pid);
	if (g_file_get_contents (path, &content, NULL, NULL)) {
		g_strchomp (content);
		g_warning ("Offending API user is %s", content);
	}

out:
	g_clear_object (&proxy);
}
#endif

static void fprint_device_delete_enrolled_fingers(FprintDevice *rdev,
						  const char *username,
						  DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = fprint_device_get_instance_private(rdev);
	GError *error = NULL;
	char *user, *sender;
	gboolean opened;

	g_warning ("The API user should be updated to use DeleteEnrolledFingers2 method!");
#ifdef __linux__
	log_offending_client(context);
#endif

	user = _fprint_device_check_for_username (rdev,
						  context,
						  username,
						  NULL,
						  &error);
	if (user == NULL) {
		dbus_g_method_return_error (context, error);
		g_error_free (error);
		return;
	}

	if (_fprint_device_check_polkit_for_action (rdev, context, "net.reactivated.fprint.device.enroll", &error) == FALSE) {
		g_free (user);
		dbus_g_method_return_error (context, error);
		g_error_free (error);
		return;
	}

	if (_fprint_device_check_claimed(rdev, context, &error) == FALSE) {
		/* Return error for anything but FPRINT_ERROR_CLAIM_DEVICE */
		if (!g_error_matches (error, FPRINT_ERROR, FPRINT_ERROR_CLAIM_DEVICE)) {
			dbus_g_method_return_error (context, error);
			return;
		}

		g_clear_error (&error);
		opened = FALSE;
	} else {
		opened = TRUE;
	}

	sender = dbus_g_method_get_sender (context);
	_fprint_device_add_client (rdev, sender);
	g_free (sender);

	if (!opened && fp_device_has_storage (priv->dev))
		fp_device_open_sync (priv->dev, NULL, NULL);

	delete_enrolled_fingers (rdev, user);

	if (!opened && fp_device_has_storage (priv->dev))
		fp_device_close_sync (priv->dev, NULL, NULL);

	g_free (user);

	dbus_g_method_return(context);
}

static void fprint_device_delete_enrolled_fingers2(FprintDevice *rdev,
						    DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = fprint_device_get_instance_private(rdev);
	GError *error = NULL;

	if (_fprint_device_check_claimed(rdev, context, &error) == FALSE) {
		dbus_g_method_return_error (context, error);
		return;
	}

	if (_fprint_device_check_polkit_for_action (rdev, context, "net.reactivated.fprint.device.enroll", &error) == FALSE) {
		dbus_g_method_return_error (context, error);
		g_error_free (error);
		return;
	}

	delete_enrolled_fingers (rdev, priv->session->username);

	dbus_g_method_return(context);
}

