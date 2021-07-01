/*
 * /net/reactivated/Fprint/Device/foo object implementation
 * Copyright (C) 2008 Daniel Drake <dsd@gentoo.org>
 * Copyright (C) 2020 Marco Trevisan <marco.trevisan@canonical.com>
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

#include <glib/gi18n.h>
#include <gio/gio.h>
#include <polkit/polkit.h>
#include <fprint.h>

#include <sys/types.h>
#include <pwd.h>
#include <errno.h>

#include "fprintd-dbus.h"
#include "fprintd.h"
#include "storage.h"

#define VERIFY_STOP_DEVICE_WAIT 1 /* Seconds to wait for the device to complete */

static const char *FINGERS_NAMES[] = {
  [FP_FINGER_UNKNOWN]      = "unknown",
  [FP_FINGER_LEFT_THUMB]   = "left-thumb",
  [FP_FINGER_LEFT_INDEX]   = "left-index-finger",
  [FP_FINGER_LEFT_MIDDLE]  = "left-middle-finger",
  [FP_FINGER_LEFT_RING]    = "left-ring-finger",
  [FP_FINGER_LEFT_LITTLE]  = "left-little-finger",
  [FP_FINGER_RIGHT_THUMB]  = "right-thumb",
  [FP_FINGER_RIGHT_INDEX]  = "right-index-finger",
  [FP_FINGER_RIGHT_MIDDLE] = "right-middle-finger",
  [FP_FINGER_RIGHT_RING]   = "right-ring-finger",
  [FP_FINGER_RIGHT_LITTLE] = "right-little-finger"
};

static void fprint_device_dbus_skeleton_iface_init (FprintDBusDeviceIface *);
static gboolean action_authorization_handler (GDBusInterfaceSkeleton *,
                                              GDBusMethodInvocation *,
                                              gpointer user_data);

static GQuark quark_auth_user = 0;

typedef enum {
  ACTION_NONE = 0,
  ACTION_IDENTIFY,
  ACTION_VERIFY,
  ACTION_ENROLL,
  ACTION_OPEN,
  ACTION_CLOSE,
  ACTION_DELETE,
} FprintDeviceAction;

typedef enum {
  STATE_CLAIMED,
  STATE_UNCLAIMED,
  STATE_AUTO_CLAIM,
  STATE_ANYTIME,
} FprintDeviceClaimState;

typedef struct
{
  volatile gint _refcount;

  /* current method invocation */
  GDBusMethodInvocation *invocation;

  /* The current user of the device, if claimed */
  const char * const sender;

  /* The current user of the device, or if allowed,
   * what was passed as a username argument */
  const char * const username;

  gboolean           verify_status_reported;
} SessionData;

typedef struct
{
  guint32          id;
  FpDevice        *dev;
  SessionData     *_session;

  guint            verify_stop_wait_timeout_id;

  PolkitAuthority *auth;

  /* Hashtable of connected clients */
  GHashTable *clients;

  /* Required to restart the operation on a retry failure. */
  FpPrint   *verify_data;
  GPtrArray *identify_data;
  int        enroll_data;

  /* whether we're running an identify, or a verify */
  FprintDeviceAction     current_action;
  GCancellable          *current_cancellable;
  GDBusMethodInvocation *current_cancel_invocation;
} FprintDevicePrivate;

G_DEFINE_TYPE_WITH_CODE (FprintDevice, fprint_device,
                         FPRINT_DBUS_TYPE_DEVICE_SKELETON,
                         G_ADD_PRIVATE (FprintDevice)
                         G_IMPLEMENT_INTERFACE (FPRINT_DBUS_TYPE_DEVICE,
                                                fprint_device_dbus_skeleton_iface_init));

enum fprint_device_properties {
  FPRINT_DEVICE_CONSTRUCT_DEV = 1,
  FPRINT_DEVICE_IN_USE,
};

enum fprint_device_signals {
  SIGNAL_VERIFY_STATUS,
  SIGNAL_VERIFY_FINGER_SELECTED,
  SIGNAL_ENROLL_STATUS,
  NUM_SIGNALS,
};

static guint32 last_id = ~0;
static guint signals[NUM_SIGNALS] = { 0, };

#ifndef POLKIT_HAS_AUTOPOINTERS
/* FIXME: Remove this once we're fine to depend on polkit 0.114 */
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PolkitAuthorizationResult, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PolkitSubject, g_object_unref)
#endif

static void
session_data_unref (SessionData *session)
{
  if (g_atomic_int_dec_and_test (&session->_refcount))
    {
      g_clear_pointer ((char **) &session->sender, g_free);
      g_clear_pointer ((char **) &session->username, g_free);
      g_clear_object (&session->invocation);
      g_free (session);
    }
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SessionData, session_data_unref);

static SessionData *
session_data_get (FprintDevicePrivate *priv)
{
  SessionData *invalid = (SessionData *) &priv->_session;
  SessionData *cur;

  /* Get the current pointer and mark the pointer as "busy". */
  do
    {
      cur = priv->_session;
      /* Swap if cur is valid, otherwise busy loop. */
    }
  while (cur == invalid || !g_atomic_pointer_compare_and_exchange (&priv->_session, cur, invalid));

  /* We can safely increase the reference count now. */
  if (cur)
    g_atomic_int_inc (&cur->_refcount);

  /* Swap back, this must succeed. */
  if (!g_atomic_pointer_compare_and_exchange (&priv->_session, invalid, cur))
    g_assert_not_reached ();

  return cur;
}

/* Pass NULL sender and username to unset session data. */
static SessionData *
session_data_set_new (FprintDevicePrivate *priv, gchar *sender, gchar *username)
{
  SessionData *invalid = (SessionData *) &priv->_session;
  SessionData *new = NULL;
  SessionData *old;

  g_assert ((!sender && !username) || (sender && username));
  if (sender)
    {
      new = g_new0 (SessionData, 1);
      /* Internal reference of the pointer and returned reference. */
      new->_refcount = 2;
      *(char **) &new->sender = sender;
      *(char **) &new->username = username;
    }

  /* Get the current (but not if it is busy) and put the new one in place. */
  do
    {
      old = priv->_session;
      /* Swap if old is valid, otherwise busy loop as someone is ref'ing it currently. */
    }
  while (old == invalid || !g_atomic_pointer_compare_and_exchange (&priv->_session, old, new));

  /* We can safely drop the our internal reference now. */
  if (old)
    session_data_unref (old);

  return new;
}

typedef FprintDevice FprintDeviceActionUnset;
static void
auto_device_action_unset (FprintDeviceActionUnset *self)
{
  FprintDevicePrivate *priv = fprint_device_get_instance_private (self);

  priv->current_action = ACTION_NONE;
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FprintDeviceActionUnset, auto_device_action_unset);

static void
fprint_device_dispose (GObject *object)
{
  FprintDevice *self = (FprintDevice *) object;
  FprintDevicePrivate *priv = fprint_device_get_instance_private (self);

  g_hash_table_remove_all (priv->clients);

  G_OBJECT_CLASS (fprint_device_parent_class)->dispose (object);
}

static void
fprint_device_finalize (GObject *object)
{
  FprintDevice *self = (FprintDevice *) object;
  FprintDevicePrivate *priv = fprint_device_get_instance_private (self);

  g_clear_handle_id (&priv->verify_stop_wait_timeout_id, g_source_remove);
  g_hash_table_destroy (priv->clients);
  session_data_set_new (priv, NULL, NULL);
  g_clear_object (&priv->auth);
  g_clear_object (&priv->dev);

  if (priv->current_action != ACTION_NONE ||
      priv->_session ||
      priv->verify_data ||
      priv->identify_data ||
      priv->current_cancellable ||
      priv->current_cancel_invocation)
    g_critical ("Device was not cleaned up properly before being finalized.");

  G_OBJECT_CLASS (fprint_device_parent_class)->finalize (object);
}

static void
fprint_device_set_property (GObject *object, guint property_id,
                            const GValue *value, GParamSpec *pspec)
{
  FprintDevice *self = (FprintDevice *) object;
  FprintDevicePrivate *priv = fprint_device_get_instance_private (self);

  switch (property_id)
    {
    case FPRINT_DEVICE_CONSTRUCT_DEV:
      priv->dev = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
fprint_device_get_property (GObject *object, guint property_id,
                            GValue *value, GParamSpec *pspec)
{
  FprintDevice *self = (FprintDevice *) object;
  FprintDevicePrivate *priv = fprint_device_get_instance_private (self);

  switch (property_id)
    {
    case FPRINT_DEVICE_CONSTRUCT_DEV:
      g_value_set_object (value, priv->dev);
      break;

    case FPRINT_DEVICE_IN_USE:
      g_value_set_boolean (value, g_hash_table_size (priv->clients) != 0);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
on_nr_enroll_stages_changed (FprintDevice *rdev,
                             GParamSpec   *spec,
                             FpDevice     *device)
{
  FprintDBusDevice *dbus_dev = FPRINT_DBUS_DEVICE (rdev);
  gint nr_enroll_stages;

  nr_enroll_stages = fp_device_get_nr_enroll_stages (device);

  /* One extra step for our internal identification. */
  if (fp_device_has_feature (device, FP_DEVICE_FEATURE_IDENTIFY))
    nr_enroll_stages += 1;

  g_debug ("Device %s enroll stages changed to %d",
           fp_device_get_name (device),
           nr_enroll_stages);

  fprint_dbus_device_set_num_enroll_stages (dbus_dev, nr_enroll_stages);
}

static void
on_scan_type_changed (FprintDevice *rdev,
                      GParamSpec   *spec,
                      FpDevice     *device)
{
  FprintDBusDevice *dbus_dev = FPRINT_DBUS_DEVICE (rdev);

  if (fp_device_get_scan_type (device) == FP_SCAN_TYPE_PRESS)
    fprint_dbus_device_set_scan_type (dbus_dev, "press");
  else if (fp_device_get_scan_type (device) == FP_SCAN_TYPE_SWIPE)
    fprint_dbus_device_set_scan_type (dbus_dev, "swipe");

  g_debug ("Device %s scan type changed to '%s'",
           fp_device_get_name (device),
           fprint_dbus_device_get_scan_type (dbus_dev));
}

static void
on_finger_status_changed (FprintDevice *rdev,
                          GParamSpec   *spec,
                          FpDevice     *device)
{
  FprintDBusDevice *dbus_dev = FPRINT_DBUS_DEVICE (rdev);
  FpFingerStatusFlags finger_status = fp_device_get_finger_status (device);
  gboolean present, needed;

  present = !!(finger_status & FP_FINGER_STATUS_PRESENT);
  fprint_dbus_device_set_finger_present (dbus_dev, present);
  g_debug ("Finger present %d", present);

  needed = !!(finger_status & FP_FINGER_STATUS_NEEDED);
  fprint_dbus_device_set_finger_needed (dbus_dev, needed);
  g_debug ("Finger needed %d", needed);
}

static void
fprint_device_constructed (GObject *object)
{
  FprintDevice *rdev = FPRINT_DEVICE (object);
  FprintDBusDevice *dbus_dev = FPRINT_DBUS_DEVICE (rdev);
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);

  fprint_dbus_device_set_name (dbus_dev, fp_device_get_name (priv->dev));

  g_signal_connect_object (priv->dev, "notify::scan-type",
                           G_CALLBACK (on_scan_type_changed),
                           rdev, G_CONNECT_SWAPPED);
  on_scan_type_changed (rdev, NULL, priv->dev);

  g_signal_connect_object (priv->dev, "notify::nr-enroll-stages",
                           G_CALLBACK (on_nr_enroll_stages_changed),
                           rdev, G_CONNECT_SWAPPED);
  on_nr_enroll_stages_changed (rdev, NULL, priv->dev);

  g_signal_connect_object (priv->dev, "notify::finger-status",
                           G_CALLBACK (on_finger_status_changed),
                           rdev, G_CONNECT_SWAPPED);
  on_finger_status_changed (rdev, NULL, priv->dev);

  G_OBJECT_CLASS (fprint_device_parent_class)->constructed (object);
}

static void
fprint_device_class_init (FprintDeviceClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GParamSpec *pspec;

  gobject_class->constructed = fprint_device_constructed;
  gobject_class->dispose = fprint_device_dispose;
  gobject_class->finalize = fprint_device_finalize;
  gobject_class->set_property = fprint_device_set_property;
  gobject_class->get_property = fprint_device_get_property;

  pspec = g_param_spec_object ("dev", "Device",
                               "Set device construction property",
                               FP_TYPE_DEVICE,
                               G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_READABLE);
  g_object_class_install_property (gobject_class,
                                   FPRINT_DEVICE_CONSTRUCT_DEV, pspec);

  pspec = g_param_spec_boolean ("in-use", "In use",
                                "Whether the device is currently in use", FALSE,
                                G_PARAM_READABLE);
  g_object_class_install_property (gobject_class,
                                   FPRINT_DEVICE_IN_USE, pspec);

  signals[SIGNAL_VERIFY_STATUS] =
    g_signal_lookup ("verify-status", FPRINT_TYPE_DEVICE);
  signals[SIGNAL_ENROLL_STATUS] =
    g_signal_lookup ("enroll-status", FPRINT_TYPE_DEVICE);
  signals[SIGNAL_VERIFY_FINGER_SELECTED] =
    g_signal_lookup ("verify-finger-selected", FPRINT_TYPE_DEVICE);

  quark_auth_user = g_quark_from_static_string ("authorized-user");
}

static void
_unwatch_name (gpointer id)
{
  g_bus_unwatch_name (GPOINTER_TO_INT (id));
}

static void
fprint_device_init (FprintDevice *device)
{
  FprintDevicePrivate *priv = fprint_device_get_instance_private (device);

  priv->id = ++last_id;

  /* Setup PolicyKit */
  priv->auth = polkit_authority_get_sync (NULL, NULL);
  priv->clients = g_hash_table_new_full (g_str_hash,
                                         g_str_equal,
                                         g_free,
                                         _unwatch_name);

  g_signal_connect (device, "g-authorize-method",
                    G_CALLBACK (action_authorization_handler),
                    NULL);
}

FprintDevice *
fprint_device_new (FpDevice *dev)
{
  return g_object_new (FPRINT_TYPE_DEVICE, "dev", dev, NULL);
}

guint32
_fprint_device_get_id (FprintDevice *rdev)
{
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);

  return priv->id;
}

static const char *
fp_finger_to_name (FpFinger finger)
{
  if (finger == FP_FINGER_UNKNOWN)
    return "any";
  if (!FP_FINGER_IS_VALID (finger))
    return NULL;
  return FINGERS_NAMES[finger];
}

static FpFinger
finger_name_to_fp_finger (const char *finger_name)
{
  FpFinger i;

  if (finger_name == NULL || *finger_name == '\0' || g_str_equal (finger_name, "any"))
    return FP_FINGER_UNKNOWN;

  for (i = FP_FINGER_FIRST; i <= FP_FINGER_LAST; i++)
    if (g_str_equal (finger_name, FINGERS_NAMES[i]))
      return i;

  /* Invalid, let's try that */
  return FP_FINGER_UNKNOWN;
}

static const char *
verify_result_to_name (gboolean match, GError *error)
{
  if (!error)
    {
      if (match)
        return "verify-match";
      else
        return "verify-no-match";
    }

  if (error->domain == FP_DEVICE_RETRY)
    {
      switch (error->code)
        {
        case FP_DEVICE_RETRY_TOO_SHORT:
          return "verify-swipe-too-short";

        case FP_DEVICE_RETRY_CENTER_FINGER:
          return "verify-finger-not-centered";

        case FP_DEVICE_RETRY_REMOVE_FINGER:
          return "verify-remove-and-retry";

        default:
          return "verify-retry-scan";
        }
    }
  else
    {
      /* Which errors should be mapped to disconnection?
       * Are drivers/libfprint/fprintd really in agreement here?
       */
      if (g_error_matches (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO))
        return "verify-disconnected";
      else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return "verify-no-match";

      return "verify-unknown-error";
    }
}

static const char *
enroll_result_to_name (gboolean completed, gboolean enrolled, GError *error)
{
  if (!error)
    {
      if (!completed)
        return "enroll-stage-passed";
      else if (enrolled)
        return "enroll-completed";
      else
        return "enroll-failed";
    }

  if (error->domain == FP_DEVICE_RETRY)
    {
      switch (error->code)
        {
        case FP_DEVICE_RETRY_TOO_SHORT:
          return "enroll-swipe-too-short";

        case FP_DEVICE_RETRY_CENTER_FINGER:
          return "enroll-finger-not-centered";

        case FP_DEVICE_RETRY_REMOVE_FINGER:
          return "enroll-remove-and-retry";

        default:
          return "enroll-retry-scan";
        }
    }
  else
    {
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

static FprintDevicePermission
get_permissions_for_invocation (GDBusMethodInvocation *invocation)
{
  FprintDevicePermission required_perms;
  const char *method_name;

  required_perms = FPRINT_DEVICE_PERMISSION_NONE;
  method_name = g_dbus_method_invocation_get_method_name (invocation);

  if (g_str_equal (method_name, "Claim"))
    {
      required_perms |= FPRINT_DEVICE_PERMISSION_VERIFY;
      required_perms |= FPRINT_DEVICE_PERMISSION_ENROLL;
    }
  else if (g_str_equal (method_name, "DeleteEnrolledFinger"))
    {
      required_perms |= FPRINT_DEVICE_PERMISSION_ENROLL;
    }
  else if (g_str_equal (method_name, "DeleteEnrolledFingers"))
    {
      required_perms |= FPRINT_DEVICE_PERMISSION_ENROLL;
    }
  else if (g_str_equal (method_name, "DeleteEnrolledFingers2"))
    {
      required_perms |= FPRINT_DEVICE_PERMISSION_ENROLL;
    }
  else if (g_str_equal (method_name, "EnrollStart"))
    {
      required_perms |= FPRINT_DEVICE_PERMISSION_ENROLL;
    }
  else if (g_str_equal (method_name, "ListEnrolledFingers"))
    {
      required_perms |= FPRINT_DEVICE_PERMISSION_VERIFY;
    }
  else if (g_str_equal (method_name, "VerifyStart"))
    {
      required_perms |= FPRINT_DEVICE_PERMISSION_VERIFY;
    }
  else if (g_str_equal (method_name, "Release"))
    {
    }
  else if (g_str_equal (method_name, "EnrollStop"))
    {
    }
  else if (g_str_equal (method_name, "VerifyStop"))
    {
      /* Don't require permissiong for for release/stop operations.
       * We are authenticated already if we could start, and we don't
       * want to end up authorizing interactively again.
       */
    }
  else
    {
      g_assert_not_reached ();
    }

  return required_perms;
}

static FprintDeviceClaimState
get_claim_state_for_invocation (GDBusMethodInvocation *invocation)
{
  const char *method_name;

  method_name = g_dbus_method_invocation_get_method_name (invocation);

  if (g_str_equal (method_name, "Claim"))
    return STATE_UNCLAIMED;
  else if (g_str_equal (method_name, "DeleteEnrolledFingers"))
    return STATE_AUTO_CLAIM;
  else if (g_str_equal (method_name, "ListEnrolledFingers"))
    return STATE_ANYTIME;

  return STATE_CLAIMED;
}

static gboolean
_fprint_device_check_claimed (FprintDevice          *rdev,
                              GDBusMethodInvocation *invocation,
                              GError               **error)
{
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);

  g_autoptr(SessionData) session = NULL;
  FprintDeviceClaimState requested_state;
  const char *sender;

  requested_state = get_claim_state_for_invocation (invocation);

  if (requested_state == STATE_ANYTIME)
    return TRUE;

  session = session_data_get (priv);
  if (requested_state == STATE_AUTO_CLAIM)
    requested_state = session ? STATE_CLAIMED : STATE_UNCLAIMED;

  if (requested_state == STATE_UNCLAIMED)
    {
      /* Is it already claimed? */
      if (!session)
        return TRUE;

      g_set_error (error, FPRINT_ERROR, FPRINT_ERROR_ALREADY_IN_USE,
                   "Device was already claimed");
      return FALSE;
    }

  g_assert (requested_state == STATE_CLAIMED);

  /* The device wasn't claimed, exit */
  if (session == NULL)
    {
      g_set_error (error, FPRINT_ERROR, FPRINT_ERROR_CLAIM_DEVICE,
                   _("Device was not claimed before use"));
      return FALSE;
    }

  sender = g_dbus_method_invocation_get_sender (invocation);

  if (!g_str_equal (sender, session->sender) || session->invocation != NULL)
    {
      g_set_error (error, FPRINT_ERROR, FPRINT_ERROR_ALREADY_IN_USE,
                   _("Device already in use by another user"));
      return FALSE;
    }

  return TRUE;
}

static gboolean
_fprint_device_check_polkit_for_action (FprintDevice          *rdev,
                                        GDBusMethodInvocation *invocation,
                                        const char            *action,
                                        GError               **error)
{
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);
  const char *sender;

  g_autoptr(GError) local_error = NULL;
  g_autoptr(PolkitAuthorizationResult) result = NULL;
  g_autoptr(PolkitSubject) subject = NULL;

  /* Check that caller is privileged */
  sender = g_dbus_method_invocation_get_sender (invocation);
  subject = polkit_system_bus_name_new (sender);

  result = polkit_authority_check_authorization_sync (priv->auth,
                                                      subject,
                                                      action,
                                                      NULL,
                                                      POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION,
                                                      NULL, &local_error);
  if (result == NULL)
    {
      g_set_error (error, FPRINT_ERROR,
                   FPRINT_ERROR_PERMISSION_DENIED,
                   "Not Authorized: %s", local_error->message);
      return FALSE;
    }

  if (!polkit_authorization_result_get_is_authorized (result))
    {
      g_set_error (error, FPRINT_ERROR,
                   FPRINT_ERROR_PERMISSION_DENIED,
                   "Not Authorized: %s", action);
      return FALSE;
    }

  return TRUE;
}

static gboolean
fprint_device_check_polkit_for_permissions (FprintDevice          *rdev,
                                            GDBusMethodInvocation *invocation,
                                            FprintDevicePermission permissions,
                                            GError               **error)
{
  g_autoptr(GFlagsClass) permission_flags = NULL;
  unsigned i;

  if (permissions == FPRINT_DEVICE_PERMISSION_NONE)
    return TRUE;

  permission_flags = g_type_class_ref (FPRINT_TYPE_DEVICE_PERMISSION);

  for (i = 0; i < permission_flags->n_values; ++i)
    {
      GFlagsValue *value = &permission_flags->values[i];
      const char *action;

      if (!(value->value & permissions))
        continue;

      action = value->value_nick;
      g_debug ("Getting authorization to perform Polkit action %s",
               action);

      g_clear_error (error);
      if (_fprint_device_check_polkit_for_action (rdev, invocation,
                                                  action, error))
        return TRUE;
    }

  g_assert (!error || *error);
  return FALSE;
}

static char *
_fprint_device_check_for_username (FprintDevice          *rdev,
                                   GDBusMethodInvocation *invocation,
                                   const char            *username,
                                   GError               **error)
{
  g_autoptr(GVariant) ret = NULL;
  g_autoptr(GError) local_error = NULL;
  GDBusConnection *connection;
  const char *sender;
  struct passwd *user;
  guint32 uid;

  /* Get details about the current sender, and username/uid */
  connection = g_dbus_method_invocation_get_connection (invocation);
  sender = g_dbus_method_invocation_get_sender (invocation);

  ret = g_dbus_connection_call_sync (connection,
                                     "org.freedesktop.DBus",
                                     "/org/freedesktop/DBus",
                                     "org.freedesktop.DBus",
                                     "GetConnectionUnixUser",
                                     g_variant_new ("(s)", sender),
                                     NULL, G_DBUS_CALL_FLAGS_NONE, -1,
                                     NULL, &local_error);

  if (!ret)
    {
      g_set_error (error, FPRINT_ERROR, FPRINT_ERROR_INTERNAL,
                   "Could not get connection unix user ID: %s",
                   local_error->message);
      return NULL;
    }

  g_variant_get (ret, "(u)", &uid);
  user = getpwuid (uid);
  if (user == NULL)
    {
      g_set_error (error, FPRINT_ERROR, FPRINT_ERROR_INTERNAL,
                   "Failed to get information about user UID %u", uid);
      return NULL;
    }

  /* The current user is usually allowed to access their
   * own data, this should be followed by PolicyKit checks
   * anyway */
  if (username == NULL || *username == '\0' || g_str_equal (username, user->pw_name))
    return g_strdup (user->pw_name);

  /* If we're not allowed to set a different username,
   * then fail */
  if (!fprint_device_check_polkit_for_permissions (rdev, invocation,
                                                   FPRINT_DEVICE_PERMISSION_SETUSERNAME,
                                                   error))
    return NULL;

  return g_strdup (username);
}

static void
_fprint_device_client_vanished (GDBusConnection *connection,
                                const char      *name,
                                FprintDevice    *rdev)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(SessionData) session = NULL;
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);

  session = session_data_get (priv);

  /* Was that the client that claimed the device? */
  if (session != NULL &&
      g_strcmp0 (session->sender, name) == 0)
    {
      g_cancellable_cancel (priv->current_cancellable);

      if (!priv->current_cancellable)
        {
          /* This isn't optimal, but for verify/identify/enroll we expect the stop
           * command. And we use current_cancellable as a flag to know that the
           * underlying operation has finished already.
           * If it has finished, unset the current_action. */
          switch (priv->current_action)
            {
            case ACTION_VERIFY:
            case ACTION_IDENTIFY:
            case ACTION_ENROLL:
              priv->current_action = ACTION_NONE;
              break;

            default:
              break;
            }
        }

      while (priv->current_action != ACTION_NONE)
        g_main_context_iteration (NULL, TRUE);

      /* The session may have disappeared at this point if the device
       * was already closing. */
      g_clear_pointer (&session, session_data_unref);
      session = session_data_get (priv);
      if (session && !fp_device_close_sync (priv->dev, NULL, &error))
        g_critical ("Error closing device after disconnect: %s", error->message);

      session_data_set_new (priv, NULL, NULL);
    }
  g_hash_table_remove (priv->clients, name);

  if (g_hash_table_size (priv->clients) == 0)
    g_object_notify (G_OBJECT (rdev), "in-use");
}

static void
_fprint_device_add_client (FprintDevice *rdev, const char *sender)
{
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);
  guint id;

  id = GPOINTER_TO_UINT (g_hash_table_lookup (priv->clients, sender));
  if (id == 0)
    {
      id = g_bus_watch_name (G_BUS_TYPE_SYSTEM,
                             sender,
                             G_BUS_NAME_WATCHER_FLAGS_NONE,
                             NULL,
                             (GBusNameVanishedCallback) _fprint_device_client_vanished,
                             rdev,
                             NULL);
      g_hash_table_insert (priv->clients, g_strdup (sender), GUINT_TO_POINTER (id));
      g_object_notify (G_OBJECT (rdev), "in-use");
    }
}

static void
dev_open_cb (FpDevice *dev, GAsyncResult *res, void *user_data)
{
  g_autoptr(GError) error = NULL;
  FprintDevice *rdev = user_data;
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);

  g_autoptr(SessionData) session = NULL;
  g_autoptr(GDBusMethodInvocation) invocation = NULL;
  g_autoptr(FprintDeviceActionUnset) action_unset = NULL;

  action_unset = rdev;
  session = session_data_get (priv);
  invocation = g_steal_pointer (&session->invocation);

  if (!fp_device_open_finish (dev, res, &error))
    {
      g_autoptr(GError) dbus_error = NULL;

      dbus_error = g_error_new (FPRINT_ERROR,
                                FPRINT_ERROR_INTERNAL,
                                "Open failed with error: %s", error->message);
      g_dbus_method_invocation_return_gerror (invocation, dbus_error);
      session_data_set_new (priv, NULL, NULL);
      return;
    }

  g_debug ("claimed device %d", priv->id);

  fprint_dbus_device_complete_claim (FPRINT_DBUS_DEVICE (rdev),
                                     invocation);
}

static gboolean
fprintd_device_authorize_user (FprintDevice          *rdev,
                               GDBusMethodInvocation *invocation,
                               GError               **error)
{
  GVariant *params = NULL;
  const char *username = NULL;
  g_autofree char *user = NULL;

  params = g_dbus_method_invocation_get_parameters (invocation);
  g_assert (g_variant_n_children (params) == 1);
  g_variant_get (params, "(&s)", &username);
  g_assert (username);

  user = _fprint_device_check_for_username (rdev,
                                            invocation,
                                            username,
                                            error);
  if (user == NULL)
    return FALSE;

  /* We keep the user attached to the invocation as it may not be the same
   * of the requested one, in case an empty one was passed.
   * Given that now we may have multiple cuncurrent requests, it wouldn't
   * be safe to add another member to the priv, as it would need even more
   * multi-thread checks around, and over-complicate things.
   */
  g_object_set_qdata_full (G_OBJECT (invocation), quark_auth_user,
                           g_steal_pointer (&user), g_free);

  return TRUE;
}

static gboolean
fprint_device_claim (FprintDBusDevice      *dbus_dev,
                     GDBusMethodInvocation *invocation,
                     const char            *username)
{
  FprintDevice *rdev = FPRINT_DEVICE (dbus_dev);
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);

  g_autoptr(SessionData) session = NULL;
  g_autoptr(GError) error = NULL;
  char *sender, *user;

  if (!_fprint_device_check_claimed (rdev, invocation, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  user = g_object_steal_qdata (G_OBJECT (invocation), quark_auth_user);
  g_assert (user);
  g_assert (g_str_equal (username, "") || g_str_equal (user, username));

  sender = g_strdup (g_dbus_method_invocation_get_sender (invocation));
  _fprint_device_add_client (rdev, sender);

  session = session_data_set_new (priv, g_steal_pointer (&sender), g_steal_pointer (&user));
  session->invocation = g_object_ref (invocation);

  g_debug ("user '%s' claiming the device: %d", session->username, priv->id);

  priv->current_action = ACTION_OPEN;
  fp_device_open (priv->dev, NULL, (GAsyncReadyCallback) dev_open_cb, rdev);

  return TRUE;
}

static void
dev_close_cb (FpDevice *dev, GAsyncResult *res, void *user_data)
{
  g_autoptr(GError) error = NULL;
  FprintDevice *rdev = user_data;
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);

  g_autoptr(SessionData) session = NULL;
  g_autoptr(GDBusMethodInvocation) invocation = NULL;
  g_autoptr(FprintDeviceActionUnset) action_unset = NULL;

  session = session_data_get (priv);
  session_data_set_new (priv, NULL, NULL);
  invocation = g_steal_pointer (&session->invocation);
  action_unset = rdev;

  if (!fp_device_close_finish (dev, res, &error))
    {
      g_autoptr(GError) dbus_error = NULL;

      dbus_error = g_error_new (FPRINT_ERROR,
                                FPRINT_ERROR_INTERNAL,
                                "Release failed with error: %s", error->message);
      g_dbus_method_invocation_return_gerror (invocation, dbus_error);
      return;
    }

  g_debug ("released device %d", priv->id);

  fprint_dbus_device_complete_release (FPRINT_DBUS_DEVICE (rdev),
                                       invocation);
}

static gboolean
fprint_device_release (FprintDBusDevice      *dbus_dev,
                       GDBusMethodInvocation *invocation)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(SessionData) session = NULL;
  FprintDevice *rdev = FPRINT_DEVICE (dbus_dev);
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);

  if (!_fprint_device_check_claimed (rdev, invocation, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (priv->current_cancellable)
    {
      if (priv->current_action == ACTION_ENROLL)
        g_warning ("Enrollment was in progress, stopping it");
      else if (priv->current_action == ACTION_IDENTIFY ||
               priv->current_action == ACTION_VERIFY)
        g_warning ("Verification was in progress, stopping it");
      else if (priv->current_action == ACTION_DELETE)
        g_warning ("Deletion was in progress, stopping it");

      g_cancellable_cancel (priv->current_cancellable);
      while (priv->current_action != ACTION_NONE)
        g_main_context_iteration (NULL, TRUE);
    }

  session = session_data_get (priv);
  session->invocation = g_object_ref (invocation);

  priv->current_action = ACTION_CLOSE;
  fp_device_close (priv->dev, NULL, (GAsyncReadyCallback) dev_close_cb, rdev);

  return TRUE;
}

/* NOTE: This should probably be moved to the storage layer. */
static GPtrArray *
load_user_prints (FprintDevice *rdev,
                  const char   *username)
{
  g_autoptr(GPtrArray) res = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr(GSList) fingers = NULL;
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);
  GSList *l;

  fingers = store.discover_prints (priv->dev, username);

  for (l = fingers; l; l = l->next)
    {
      g_autoptr(FpPrint) print = NULL;

      store.print_data_load (priv->dev,
                             GPOINTER_TO_UINT (l->data),
                             username,
                             &print);

      if (!print)
        continue;

      g_ptr_array_add (res, g_steal_pointer (&print));
    }

  return g_steal_pointer (&res);
}

static GPtrArray *
load_all_prints (FprintDevice *rdev)
{
  g_autoptr(GPtrArray) res = g_ptr_array_new_with_free_func (g_object_unref);
  GSList *user, *users = NULL;

  users = store.discover_users ();

  for (user = users; user; user = user->next)
    {
      const char *username = user->data;
      g_autoptr(GPtrArray) prints = load_user_prints (rdev, username);

      g_ptr_array_extend_and_steal (res, g_steal_pointer (&prints));
    }

  g_slist_free_full (users, g_free);

  return g_steal_pointer (&res);
}

static void
report_verify_status (FprintDevice *rdev,
                      gboolean      match,
                      GError       *error)
{
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);
  const char *result = verify_result_to_name (match, error);

  g_autoptr(SessionData) session = NULL;
  gboolean done;

  done = (error == NULL || error->domain != FP_DEVICE_RETRY);

  session = session_data_get (priv);

  if (done && session->verify_status_reported)
    {
      /* It is completely fine for cancellation to occur after a
       * result has been reported. */
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Verify status already reported. Ignoring %s", result);
      return;
    }

  g_debug ("report_verify_status: result %s", result);
  g_signal_emit (rdev, signals[SIGNAL_VERIFY_STATUS], 0, result, done);

  if (done)
    session->verify_status_reported = TRUE;
}

static gboolean
can_start_action (FprintDevice *rdev, GError **error)
{
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);

  switch (priv->current_action)
    {
    case ACTION_NONE:
      return TRUE;

    case ACTION_ENROLL:
      g_set_error (error,
                   FPRINT_ERROR, FPRINT_ERROR_ALREADY_IN_USE,
                   "Enrollment already in progress");
      break;

    case ACTION_IDENTIFY:
    case ACTION_VERIFY:
      g_set_error (error,
                   FPRINT_ERROR, FPRINT_ERROR_ALREADY_IN_USE,
                   "Verification already in progress");
      break;

    case ACTION_OPEN:
      g_set_error (error,
                   FPRINT_ERROR, FPRINT_ERROR_ALREADY_IN_USE,
                   "Claim already in progress");
      break;

    case ACTION_CLOSE:
      g_set_error (error,
                   FPRINT_ERROR, FPRINT_ERROR_ALREADY_IN_USE,
                   "Release already in progress");
      break;

    case ACTION_DELETE:
      g_set_error (error,
                   FPRINT_ERROR, FPRINT_ERROR_ALREADY_IN_USE,
                   "Delete already in progress");
      break;

    default:             /* Fallback only. */
      g_assert_not_reached ();
      g_set_error (error,
                   FPRINT_ERROR, FPRINT_ERROR_ALREADY_IN_USE,
                   "Another operation is already in progress");
    }

  return FALSE;
}

static gboolean
can_stop_action (FprintDevice      *rdev,
                 FprintDeviceAction action,
                 GError           **error)
{
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);
  gboolean action_matches;

  switch (priv->current_action)
    {
    case ACTION_IDENTIFY:
    case ACTION_VERIFY:
      action_matches = (action == ACTION_VERIFY || action == ACTION_IDENTIFY);
      break;

    default:
      action_matches = priv->current_action == action;
    }

  if (action_matches && !priv->current_cancel_invocation)
    return TRUE;

  if (priv->current_action != ACTION_NONE || action_matches)
    {
      g_set_error (error, FPRINT_ERROR, FPRINT_ERROR_ALREADY_IN_USE,
                   "Another operation is already in progress");
      return FALSE;
    }

  switch (action)
    {
    case ACTION_ENROLL:
      g_set_error (error, FPRINT_ERROR, FPRINT_ERROR_NO_ACTION_IN_PROGRESS,
                   "No enrollment in progress");
      return FALSE;

    case ACTION_VERIFY:
      g_set_error (error, FPRINT_ERROR, FPRINT_ERROR_NO_ACTION_IN_PROGRESS,
                   "No verification in progress");
      return FALSE;

    default:
      g_set_error (error, FPRINT_ERROR, FPRINT_ERROR_NO_ACTION_IN_PROGRESS,
                   "No action in progress");
      return FALSE;
    }
}

static void
stoppable_action_completed (FprintDevice *rdev)
{
  g_autoptr(SessionData) session = NULL;
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);
  FprintDBusDevice *dbus_dev = FPRINT_DBUS_DEVICE (rdev);

  session = session_data_get (priv);

  /* Return the cancellation or reset action right away if vanished. */
  if (priv->current_cancel_invocation)
    {
      switch (priv->current_action)
        {
        case ACTION_VERIFY:
        case ACTION_IDENTIFY:
          fprint_dbus_device_complete_verify_stop (dbus_dev,
                                                   g_steal_pointer (&priv->current_cancel_invocation));
          break;

        case ACTION_ENROLL:
          fprint_dbus_device_complete_enroll_stop (dbus_dev,
                                                   g_steal_pointer (&priv->current_cancel_invocation));
          break;

        default:
          g_assert_not_reached ();
        }

      priv->current_action = ACTION_NONE;
      session->verify_status_reported = FALSE;
    }
  else if (g_cancellable_is_cancelled (priv->current_cancellable))
    {
      priv->current_action = ACTION_NONE;
      session->verify_status_reported = FALSE;
    }

  g_clear_object (&priv->current_cancellable);
}

static void
stoppable_action_stop (FprintDevice          *rdev,
                       GDBusMethodInvocation *invocation)
{
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);

  g_assert (priv->current_cancel_invocation == NULL);

  priv->current_cancel_invocation = invocation;

  /* We return only when the action was cancelled */
  if (priv->current_cancellable)
    g_cancellable_cancel (priv->current_cancellable);
  else
    stoppable_action_completed (rdev);
}

static void
match_cb (FpDevice *device,
          FpPrint  *match,
          FpPrint  *print,
          gpointer  user_data,
          GError   *error)
{
  FprintDevice *rdev = user_data;
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);
  gboolean matched;
  gboolean cancelled;

  g_assert_true (error == NULL || error->domain == FP_DEVICE_RETRY);

  cancelled = g_cancellable_is_cancelled (priv->current_cancellable);
  matched = match != NULL && cancelled == FALSE;

  /* No-match is reported only after the operation completes.
   * This avoids problems when the operation is immediately restarted. */
  report_verify_status (rdev, matched, error);
}

static void
verify_cb (FpDevice *dev, GAsyncResult *res, void *user_data)
{
  g_autoptr(GError) error = NULL;
  FprintDevice *rdev = user_data;
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);
  gboolean success;
  const char *name;
  gboolean match;

  success = fp_device_verify_finish (dev, res, &match, NULL, &error);
  g_assert (!!success == !error);
  name = verify_result_to_name (match, error);

  g_debug ("verify_cb: result %s", name);

  /* Automatically restart the operation for retry failures */
  if (error && error->domain == FP_DEVICE_RETRY)
    {
      fp_device_verify (priv->dev,
                        priv->verify_data,
                        priv->current_cancellable,
                        match_cb, rdev, NULL,
                        (GAsyncReadyCallback) verify_cb,
                        rdev);
    }
  else
    {
      g_clear_object (&priv->verify_data);

      if (error)
        {
          report_verify_status (rdev, FALSE, error);

          if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning ("Device reported an error during verify: %s",
                       error->message);
        }

      stoppable_action_completed (rdev);
    }
}

static void
identify_cb (FpDevice *dev, GAsyncResult *res, void *user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) match = NULL;
  FprintDevice *rdev = user_data;
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);
  const char *name;
  gboolean success;

  success = fp_device_identify_finish (dev, res, &match, NULL, &error);
  g_assert (!!success == !error);
  name = verify_result_to_name (match != NULL, error);

  g_debug ("identify_cb: result %s", name);

  /* Automatically restart the operation for retry failures */
  if (error && error->domain == FP_DEVICE_RETRY)
    {
      fp_device_identify (priv->dev,
                          priv->identify_data,
                          priv->current_cancellable,
                          match_cb, rdev, NULL,
                          (GAsyncReadyCallback) identify_cb,
                          rdev);
    }
  else
    {
      g_clear_pointer (&priv->identify_data, g_ptr_array_unref);

      if (error)
        {
          report_verify_status (rdev, FALSE, error);

          if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning ("Device reported an error during identify: %s",
                       error->message);
        }

      stoppable_action_completed (rdev);
    }
}

static gboolean
fprint_device_verify_start (FprintDBusDevice      *dbus_dev,
                            GDBusMethodInvocation *invocation,
                            const char            *finger_name)
{
  FprintDevice *rdev = FPRINT_DEVICE (dbus_dev);
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);

  g_autoptr(GPtrArray) gallery = NULL;
  g_autoptr(SessionData) session = NULL;
  g_autoptr(GError) error = NULL;
  FpFinger finger = finger_name_to_fp_finger (finger_name);

  if (!_fprint_device_check_claimed (rdev, invocation, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  session = session_data_get (priv);

  if (!can_start_action (rdev, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (finger == FP_FINGER_UNKNOWN)
    {
      gallery = load_user_prints (rdev, session->username);

      if (!gallery->len)
        {
          g_set_error (&error, FPRINT_ERROR, FPRINT_ERROR_NO_ENROLLED_PRINTS,
                       "No fingerprints enrolled");
          g_dbus_method_invocation_return_gerror (invocation, error);
          return TRUE;
        }
      if (fp_device_has_feature (priv->dev, FP_DEVICE_FEATURE_IDENTIFY))
        {
          guint i;

          for (i = 0; i < gallery->len; i++)
            {
              FpPrint *fprint = g_ptr_array_index (gallery, i);

              g_debug ("adding finger %s to the gallery",
                       fp_finger_to_name (fp_print_get_finger (fprint)));
            }
        }
    }

  if (fp_device_has_feature (priv->dev, FP_DEVICE_FEATURE_IDENTIFY) &&
      finger == FP_FINGER_UNKNOWN)
    {
      priv->current_action = ACTION_IDENTIFY;

      g_debug ("start identification device %d", priv->id);
      priv->current_cancellable = g_cancellable_new ();
      priv->identify_data = g_ptr_array_ref (gallery);
      fp_device_identify (priv->dev, gallery, priv->current_cancellable,
                          match_cb, rdev, NULL,
                          (GAsyncReadyCallback) identify_cb, rdev);
    }
  else
    {
      g_autoptr(FpPrint) print = NULL;

      if (gallery)
        {
          print = g_ptr_array_steal_index_fast (gallery, 0);
          finger = fp_print_get_finger (print);
        }
      else
        {
          store.print_data_load (priv->dev, finger, session->username, &print);
        }

      if (!print)
        {
          g_set_error (&error, FPRINT_ERROR, FPRINT_ERROR_NO_ENROLLED_PRINTS,
                       "No such print %d", finger);
          g_dbus_method_invocation_return_gerror (invocation,
                                                  error);
          return TRUE;
        }

      g_debug ("start verification device %d finger %s", priv->id,
               fp_finger_to_name (finger));

      priv->current_action = ACTION_VERIFY;
      priv->current_cancellable = g_cancellable_new ();
      priv->verify_data = g_object_ref (print);
      fp_device_verify (priv->dev, print, priv->current_cancellable,
                        match_cb, rdev, NULL,
                        (GAsyncReadyCallback) verify_cb, rdev);
    }

  fprint_dbus_device_complete_verify_start (dbus_dev, invocation);

  /* Emit VerifyFingerSelected telling the front-end which finger
   * we selected for auth */
  g_signal_emit (rdev, signals[SIGNAL_VERIFY_FINGER_SELECTED],
                 0, fp_finger_to_name (finger));

  return TRUE;
}

static gboolean
verify_stop_wait_timeout (gpointer data)
{
  guint *timeout_id = data;

  *timeout_id = 0;
  return FALSE;
}

static gboolean
verify_has_completed (FprintDevice *rdev)
{
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);

  if (!priv->current_cancellable ||
      g_cancellable_is_cancelled (priv->current_cancellable))
    return TRUE;

  switch (priv->current_action)
    {
    case ACTION_VERIFY:
      return !priv->verify_data;

    case ACTION_IDENTIFY:
      return !priv->identify_data;

    default:
      g_assert_not_reached ();
    }
}

static gboolean
fprint_device_verify_stop (FprintDBusDevice      *dbus_dev,
                           GDBusMethodInvocation *invocation)
{
  FprintDevice *rdev = FPRINT_DEVICE (dbus_dev);
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);

  g_autoptr(GError) error = NULL;

  if (!_fprint_device_check_claimed (rdev, invocation, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (!can_stop_action (rdev, ACTION_VERIFY, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (!verify_has_completed (rdev))
    {
      g_autoptr(SessionData) session = session_data_get (priv);

      if (session->verify_status_reported)
        {
          /* If we got a status report we need to delay the cancellation
           * of the action, leaving the device some more time to complete
           * the operation (and in case return the real error) before proceed
           * in cancelling it.
           * In case Release or client vanished while waiting the invocation
           * will be handled by stoppable_action_completed() during cancellation
           */
          g_assert (priv->verify_stop_wait_timeout_id == 0);

          priv->verify_stop_wait_timeout_id =
            g_timeout_add_seconds (VERIFY_STOP_DEVICE_WAIT, verify_stop_wait_timeout,
                                   &priv->verify_stop_wait_timeout_id);

          g_assert (priv->current_cancel_invocation == NULL);
          priv->current_cancel_invocation = invocation;

          while (priv->verify_stop_wait_timeout_id && !verify_has_completed (rdev))
            g_main_context_iteration (NULL, TRUE);

          g_clear_handle_id (&priv->verify_stop_wait_timeout_id, g_source_remove);

          if (!priv->current_cancel_invocation)
            return TRUE;

          priv->current_cancel_invocation = NULL;
        }
    }

  stoppable_action_stop (rdev, invocation);

  return TRUE;
}

static void
enroll_progress_cb (FpDevice *dev,
                    gint      completed_stages,
                    FpPrint  *print,
                    gpointer  user_data,
                    GError   *error)
{
  FprintDevice *rdev = user_data;
  const char *name = enroll_result_to_name (FALSE, FALSE, error);

  g_debug ("enroll_stage_cb: result %s", name);

  /* NOTE: We add one more step internally, but we can ignore that here. */
  if (completed_stages < fp_device_get_nr_enroll_stages (dev))
    g_signal_emit (rdev, signals[SIGNAL_ENROLL_STATUS], 0, name, FALSE);
}

static gint
garbage_collect_sort (gconstpointer a, gconstpointer b)
{
  /* Prefer */
  FpPrint *print_a = *((FpPrint **) a);
  FpPrint *print_b = *((FpPrint **) b);
  const GDate *date_a, *date_b;
  guint32 julian_a = 0;
  guint32 julian_b = 0;
  gint32 rand_a, rand_b;

  date_a = fp_print_get_enroll_date (print_a);
  date_b = fp_print_get_enroll_date (print_b);

  if (date_a && g_date_valid (date_a))
    julian_a = g_date_get_julian (date_a);
  if (date_b && g_date_valid (date_b))
    julian_b = g_date_get_julian (date_b);

  /* Sort older prints first. */
  if (julian_a != julian_b)
    return julian_b - julian_a;

  /* Randomize the order,
   * do so by sorting on a random number we assign to each print.
   * Not nice, but gets the job done.
   */
  rand_a = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (print_a), "sort-rand"));
  rand_b = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (print_b), "sort-rand"));
  if (rand_a == 0)
    {
      rand_a = g_random_int_range (1, G_MAXINT32);
      g_object_set_data (G_OBJECT (print_a), "sort-rand", GINT_TO_POINTER (rand_a));
    }

  if (rand_b == 0)
    {
      rand_b = g_random_int_range (1, G_MAXINT32);
      g_object_set_data (G_OBJECT (print_b), "sort-rand", GINT_TO_POINTER (rand_b));
    }

  return rand_b - rand_a;
}

static gboolean
try_delete_print (FprintDevice *rdev)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) device_prints = NULL;
  g_autoptr(GPtrArray) host_prints = NULL;
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);
  guint i;

  device_prints = fp_device_list_prints_sync (priv->dev, NULL, &error);
  if (!device_prints)
    {
      g_warning ("Failed to query prints: %s", error->message);
      return FALSE;
    }

  g_debug ("Device has %d prints stored", device_prints->len);

  /* Sort in order of preferred garbage collection.
   * With randomization if we can't sort them. */
  g_ptr_array_sort (device_prints, garbage_collect_sort);

  host_prints = load_all_prints (rdev);

  for (i = 0; i < host_prints->len; i++)
    {
      guint index;

      if (!g_ptr_array_find_with_equal_func (device_prints,
                                             g_ptr_array_index (host_prints, i),
                                             (GEqualFunc) fp_print_equal,
                                             &index))
        continue;

      /* Found an equal print, remove it */
      g_ptr_array_remove_index (device_prints, index);
    }

  g_debug ("Device has %d prints stored that we do not need", device_prints->len);
  if (device_prints->len == 0)
    return FALSE;

  /* Just delete the first print in the list at this point.
   * We could be smarter and fetch some more metadata. */
  fp_device_delete_print_sync (priv->dev,
                               g_ptr_array_index (device_prints, 0),
                               NULL,
                               &error);

  if (error)
    {
      g_warning ("Failed to garbage collect a print: %s", error->message);
      return FALSE;
    }

  return TRUE;
}

#if !GLIB_CHECK_VERSION (2, 63, 3)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GDate, g_date_free);
#endif

static FpPrint *
fprint_device_create_enroll_template (FprintDevice *rdev, FpFinger finger)
{
  g_autoptr(SessionData) session = NULL;
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);

  g_autoptr(GDateTime) datetime = NULL;
  g_autoptr(GDate) date = NULL;
  FpPrint *template = NULL;
  gint year, month, day;

  session = session_data_get (priv);

  template = fp_print_new (priv->dev);
  fp_print_set_finger (template, finger);
  fp_print_set_username (template, session->username);
  datetime = g_date_time_new_now_local ();
  g_date_time_get_ymd (datetime, &year, &month, &day);
  date = g_date_new_dmy (day, month, year);
  fp_print_set_enroll_date (template, date);

  return template;
}

static void
enroll_cb (FpDevice *dev, GAsyncResult *res, void *user_data)
{
  g_autoptr(GError) error = NULL;
  FprintDevice *rdev = user_data;
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);

  g_autoptr(FpPrint) print = NULL;
  const char *name;

  print = fp_device_enroll_finish (dev, res, &error);

  /* We need to special case the issue where the on device storage
   * is completely full. In that case, we check whether we can delete
   * a print that is not coming from us; assuming it is from an old
   * installation.
   * We do this synchronously, which is not great but should be good
   * enough. */
  if (g_error_matches (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_FULL))
    {
      g_debug ("Device storage is full");
      if (fp_device_has_feature (priv->dev, FP_DEVICE_FEATURE_STORAGE_LIST) &&
          try_delete_print (rdev))
        {
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

  if (print)
    {
      int r;
      r = store.print_data_save (print);
      if (r != 0)
        name = "enroll-failed";
    }

  g_signal_emit (rdev, signals[SIGNAL_ENROLL_STATUS], 0, name, TRUE);

  if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_warning ("Device reported an error during enroll: %s", error->message);

  stoppable_action_completed (rdev);
}

static void
enroll_start (FprintDevice *rdev)
{
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);

  fp_device_enroll (priv->dev,
                    fprint_device_create_enroll_template (rdev, priv->enroll_data),
                    priv->current_cancellable,
                    enroll_progress_cb,
                    rdev,
                    NULL,
                    (GAsyncReadyCallback) enroll_cb,
                    rdev);
}

static void
enroll_identify_cb (FpDevice *dev, GAsyncResult *res, void *user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) matched_print = NULL;
  g_autoptr(FpPrint) found_print = NULL;
  FprintDevice *rdev = user_data;
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);
  const char *name;

  fp_device_identify_finish (dev, res, &matched_print, &found_print, &error);

  if (g_error_matches (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_NOT_FOUND))
    {
      g_clear_object (&found_print);
      g_clear_error (&error);
    }

  /* We may need to retry or error out. */
  if (error)
    {
      gboolean retry = error->domain == FP_DEVICE_RETRY;

      name = enroll_result_to_name (!retry, FALSE, error);
      g_signal_emit (rdev, signals[SIGNAL_ENROLL_STATUS], 0, name, !retry);

      /* Retry or clean up. */
      if (retry)
        {
          g_autoptr(GPtrArray) all_prints = NULL;

          all_prints = load_all_prints (rdev);
          fp_device_identify (priv->dev,
                              all_prints,
                              priv->current_cancellable,
                              NULL,
                              NULL,
                              NULL,
                              (GAsyncReadyCallback) enroll_identify_cb,
                              rdev);
        }
      else
        {
          if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning ("Device reported an error during identify for enroll: %s", error->message);

          stoppable_action_completed (rdev);
        }

      return;
    }

  /* Identify has finished (successfully), there are three possible cases:
   *  1. Match found in the gallery, in this case, we error out.
   *  2. No match found, but on-device print returned, we should delete it
   *  3. None of the above, we can just continue.
   */

  if (matched_print)
    {
      g_signal_emit (rdev, signals[SIGNAL_ENROLL_STATUS], 0, "enroll-duplicate", TRUE);

      stoppable_action_completed (rdev);
      return;
    }

  if (found_print && fp_device_has_feature (priv->dev, FP_DEVICE_FEATURE_STORAGE))
    {
      if (!fp_print_get_device_stored (found_print))
        g_critical ("libfprint driver bug: Returned device print not marked as stored on device.");

      /* Try to delete the print (synchronously), and continue if it succeeds. */
      if (!fp_device_delete_print_sync (priv->dev,
                                        found_print,
                                        priv->current_cancellable,
                                        &error))
        {
          g_warning ("Failed to garbage collect duplicate print, cannot continue with enroll.");
          g_signal_emit (rdev, signals[SIGNAL_ENROLL_STATUS], 0, "enroll-duplicate", TRUE);

          stoppable_action_completed (rdev);
          return;
        }
    }

  g_signal_emit (rdev, signals[SIGNAL_ENROLL_STATUS], 0, "enroll-stage-passed", FALSE);

  /* We are good and can start to enroll. */
  enroll_start (rdev);
}

static gboolean
is_first_enrollment (FprintDevice * rdev)
{
  g_autoptr(GPtrArray) host_prints = NULL;
  host_prints = load_all_prints (rdev);

  return host_prints->len == 0;
}

static gboolean
fprint_device_enroll_start (FprintDBusDevice      *dbus_dev,
                            GDBusMethodInvocation *invocation,
                            const char            *finger_name)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(FpPrint) existing_print = NULL;
  g_autoptr(SessionData) session = NULL;
  FprintDevice *rdev = FPRINT_DEVICE (dbus_dev);
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);
  FpFinger finger = finger_name_to_fp_finger (finger_name);

  if (!_fprint_device_check_claimed (rdev, invocation, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (finger == FP_FINGER_UNKNOWN)
    {
      g_set_error (&error, FPRINT_ERROR, FPRINT_ERROR_INVALID_FINGERNAME,
                   "Invalid finger name");
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  session = session_data_get (priv);
  store.print_data_load (priv->dev, finger,
                         session->username, &existing_print);

  if (existing_print)
    {
      g_set_error (&error, FPRINT_ERROR, FPRINT_ERROR_FINGER_ALREADY_ENROLLED,
                   "Finger %d has already been enrolled for user %s", finger, session->username);
      g_dbus_method_invocation_return_gerror (invocation,
                                              error);
      return TRUE;
    }


  if (!can_start_action (rdev, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  g_debug ("start enrollment device %d finger %d", priv->id, finger);

  priv->current_cancellable = g_cancellable_new ();
  priv->enroll_data = finger;
  priv->current_action = ACTION_ENROLL;

  if (!fp_device_has_feature (priv->dev, FP_DEVICE_FEATURE_STORAGE_LIST) &&
      is_first_enrollment (rdev))
    {
      g_autoptr(GError) clear_err = NULL;

      if (!fp_device_clear_storage_sync (priv->dev, NULL, &clear_err))
        g_warning ("Failed to clear storage before first enrollment: %s",
                   clear_err->message);
    }

  if (fp_device_has_feature (priv->dev, FP_DEVICE_FEATURE_IDENTIFY))
    {
      g_autoptr(GPtrArray) all_prints = load_all_prints (rdev);

      /* We (now) have the policy that there must be no duplicate prints.
       * We need to do this for MoC devices, as their "identify" function
       * will generally just identify across all device stored prints.
       * For MoH, we also do it. For consistency and because it allows us
       * to implement new features in the future (i.e. logging in/unlocking
       * the correct user without selecting it first).
       */
      fp_device_identify (priv->dev,
                          all_prints,
                          priv->current_cancellable,
                          NULL,
                          NULL,
                          NULL,
                          (GAsyncReadyCallback) enroll_identify_cb,
                          rdev);
    }
  else
    {
      /* We may still want to try to use verification to check for duplicates
       * if only one fingerprint was previously enrolled, or add more verify
       * stages up to a predefined limit */
      g_warning ("Device %s does not support duplicate identification and so "
                 "fprintd duplicate detection won't work",
                 fp_device_get_name (priv->dev));
      enroll_start (rdev);
    }

  fprint_dbus_device_complete_enroll_start (dbus_dev, invocation);

  return TRUE;
}

static gboolean
fprint_device_enroll_stop (FprintDBusDevice      *dbus_dev,
                           GDBusMethodInvocation *invocation)
{
  FprintDevice *rdev = FPRINT_DEVICE (dbus_dev);

  g_autoptr(GError) error = NULL;

  if (!_fprint_device_check_claimed (rdev, invocation, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (!can_stop_action (rdev, ACTION_ENROLL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  stoppable_action_stop (rdev, invocation);

  return TRUE;
}

static gboolean
fprint_device_list_enrolled_fingers (FprintDBusDevice      *dbus_dev,
                                     GDBusMethodInvocation *invocation,
                                     const char            *username)
{
  FprintDevice *rdev = FPRINT_DEVICE (dbus_dev);

  g_autoptr(GPtrArray) ret = NULL;
  g_autoptr(GPtrArray) prints = NULL;
  const char *sender;
  const char *user;
  guint i;

  sender = g_dbus_method_invocation_get_sender (invocation);
  _fprint_device_add_client (rdev, sender);

  user = g_object_get_qdata (G_OBJECT (invocation), quark_auth_user);
  g_assert (user);
  prints = load_user_prints (rdev, user);

  ret = g_ptr_array_new ();
  for (i = 0; i < prints->len; i++)
    {
      FpFinger finger = fp_print_get_finger (g_ptr_array_index (prints, i));

      if (finger != FP_FINGER_UNKNOWN)
        g_ptr_array_add (ret, (char *) fp_finger_to_name (finger));
    }

  if (!ret->len)
    {
      g_dbus_method_invocation_return_error_literal (invocation,
                                                     FPRINT_ERROR,
                                                     FPRINT_ERROR_NO_ENROLLED_PRINTS,
                                                     "Failed to discover prints");
      return TRUE;
    }

  /* Add null-termination */
  g_ptr_array_add (ret, NULL);

  fprint_dbus_device_complete_list_enrolled_fingers (dbus_dev,
                                                     invocation, (const gchar *const *) ret->pdata);

  return TRUE;
}

static gboolean
user_has_print_enrolled (FprintDevice *rdev,
                         const char   *user,
                         FpFinger      finger)
{
  g_autoptr(GSList) prints = NULL;
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);

  prints = store.discover_prints (priv->dev, user);

  if (finger == FP_FINGER_UNKNOWN)
    return prints != NULL;

  return g_slist_find (prints, GUINT_TO_POINTER (finger)) != NULL;
}

static gboolean
delete_enrolled_fingers (FprintDevice *rdev,
                         const char   *user,
                         FpFinger      finger,
                         GError      **error)
{
  g_autoptr(GError) device_error = NULL;
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);
  guint i;
  int r;

  if (finger != FP_FINGER_UNKNOWN)
    g_debug ("Deleting enrolled finger %s for user %s",
             fp_finger_to_name (finger), user);
  else
    g_debug ("Deleting enrolled fingers for user %s", user);

  if (!user_has_print_enrolled (rdev, user, finger))
    {
      if (finger != FP_FINGER_UNKNOWN)
        g_set_error (error, FPRINT_ERROR, FPRINT_ERROR_NO_ENROLLED_PRINTS,
                     "Fingerprint for finger %s is not enrolled",
                     fp_finger_to_name (finger));
      else
        g_set_error_literal (error, FPRINT_ERROR, FPRINT_ERROR_NO_ENROLLED_PRINTS,
                             "No fingerprint enrolled");

      return FALSE;
    }

  /* First try deleting the print from the device, we don't consider it
   * fatal if this does not work. */
  if (fp_device_has_feature (priv->dev, FP_DEVICE_FEATURE_STORAGE))
    {
      g_autoptr(GSList) prints = NULL;
      GSList *l;

      prints = store.discover_prints (priv->dev, user);

      for (l = prints; l != NULL; l = l->next)
        {
          g_autoptr(FpPrint) print = NULL;

          store.print_data_load (priv->dev,
                                 GPOINTER_TO_UINT (l->data),
                                 user,
                                 &print);

          if (print)
            {
              g_autoptr(GError) local_error = NULL;

              if (finger != FP_FINGER_UNKNOWN && fp_print_get_finger (print) != finger)
                continue;

              if (!fp_device_delete_print_sync (priv->dev, print, NULL, &local_error))
                {
                  g_warning ("Error deleting print from device: %s", local_error->message);
                  g_warning ("This might indicate an issue in the libfprint driver or in the fingerprint device.");

                  if (!device_error)
                    {
                      g_set_error (&device_error, FPRINT_ERROR,
                                   FPRINT_ERROR_PRINTS_NOT_DELETED_FROM_DEVICE,
                                   "Failed to delete print from device storage: %s",
                                   local_error->message);
                    }
                }
            }
        }
    }

  if (finger != FP_FINGER_UNKNOWN)
    {
      if ((r = store.print_data_delete (priv->dev, finger, user)) != 0)
        {
          if (user_has_print_enrolled (rdev, user, finger))
            {
              g_set_error (error, FPRINT_ERROR, FPRINT_ERROR_PRINTS_NOT_DELETED,
                           "Impossible to delete fingerprint reference "
                           "got error: %d", r);
              return FALSE;
            }
        }
    }
  else
    {
      g_autoptr(GError) local_error = NULL;

      for (i = FP_FINGER_FIRST; i <= FP_FINGER_LAST; i++)
        {
          if ((r = store.print_data_delete (priv->dev, i, user)) != 0)
            {
              if (local_error)
                continue;

              if (user_has_print_enrolled (rdev, user, i))
                {
                  g_set_error (&local_error, FPRINT_ERROR, FPRINT_ERROR_PRINTS_NOT_DELETED,
                               "Impossible to delete fingerprint reference "
                               "got error: %d", r);
                  /* Do not return yet, at least try to remove the remaining prints */
                }
            }
        }

      if (local_error)
        {
          g_propagate_error (error, g_steal_pointer (&local_error));
          return FALSE;
        }
    }

  if (device_error)
    {
      /* This is a low priority error, higher priority errors would have returned failure already */
      g_propagate_error (error, g_steal_pointer (&device_error));
      return FALSE;
    }

  return TRUE;
}

#ifdef __linux__
static void
log_offending_client_cb (GObject      *object,
                         GAsyncResult *res,
                         gpointer      user_data)
{
  GDBusConnection *connection = G_DBUS_CONNECTION (object);

  g_autoptr(GVariant) ret = NULL;
  g_autofree char *path = NULL;
  g_autofree char *content = NULL;
  guint pid = 0;

  ret = g_dbus_connection_call_finish (connection, res, NULL);

  if (!ret)
    return;

  g_variant_get (ret, "(u)", &pid);
  path = g_strdup_printf ("/proc/%u/comm", pid);
  if (g_file_get_contents (path, &content, NULL, NULL))
    {
      g_strchomp (content);
      g_warning ("Offending API user is %s", content);
    }
}

static void
log_offending_client (GDBusMethodInvocation *invocation)
{
  const char *sender;
  GDBusConnection *connection;

  connection = g_dbus_method_invocation_get_connection (invocation);
  sender = g_dbus_method_invocation_get_sender (invocation);

  g_dbus_connection_call (connection,
                          "org.freedesktop.DBus",
                          "/org/freedesktop/DBus",
                          "org.freedesktop.DBus",
                          "GetConnectionUnixProcessID",
                          g_variant_new ("(s)", sender),
                          NULL, G_DBUS_CALL_FLAGS_NONE,
                          -1, NULL, log_offending_client_cb, NULL);
}
#endif

static gboolean
fprint_device_delete_enrolled_fingers (FprintDBusDevice      *dbus_dev,
                                       GDBusMethodInvocation *invocation,
                                       const char            *username)
{
  FprintDevice *rdev = FPRINT_DEVICE (dbus_dev);
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);

  g_autoptr(FprintDeviceActionUnset) action_unset = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *user = NULL;
  const char *sender;
  gboolean opened;

  g_warning ("The API user should be updated to use DeleteEnrolledFingers2 method!");
#ifdef __linux__
  log_offending_client (invocation);
#endif

  if (!can_start_action (rdev, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  priv->current_action = ACTION_DELETE;
  action_unset = rdev;

  if (!_fprint_device_check_claimed (rdev, invocation, &error))
    {
      /* Return error for anything but FPRINT_ERROR_CLAIM_DEVICE */
      if (!g_error_matches (error, FPRINT_ERROR, FPRINT_ERROR_CLAIM_DEVICE))
        {
          g_dbus_method_invocation_return_gerror (invocation,
                                                  error);
          return TRUE;
        }

      opened = FALSE;
    }
  else
    {
      opened = fp_device_is_open (priv->dev);
    }

  sender = g_dbus_method_invocation_get_sender (invocation);
  _fprint_device_add_client (rdev, sender);

  if (!opened && fp_device_has_feature (priv->dev, FP_DEVICE_FEATURE_STORAGE))
    fp_device_open_sync (priv->dev, NULL, NULL);

  user = g_object_steal_qdata (G_OBJECT (invocation), quark_auth_user);
  g_assert (user);
  g_assert (g_str_equal (username, "") || g_str_equal (user, username));

  g_clear_error (&error);
  delete_enrolled_fingers (rdev, user, FP_FINGER_UNKNOWN, &error);

  if (!opened && fp_device_has_feature (priv->dev, FP_DEVICE_FEATURE_STORAGE))
    fp_device_close_sync (priv->dev, NULL, NULL);

  if (error)
    {
      g_dbus_method_invocation_return_gerror (invocation,
                                              error);
      return TRUE;
    }

  fprint_dbus_device_complete_delete_enrolled_fingers (dbus_dev,
                                                       invocation);
  return TRUE;
}

static gboolean
fprint_device_delete_enrolled_fingers2 (FprintDBusDevice      *dbus_dev,
                                        GDBusMethodInvocation *invocation)
{
  FprintDevice *rdev = FPRINT_DEVICE (dbus_dev);
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);

  g_autoptr(SessionData) session = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(FprintDeviceActionUnset) action_unset = NULL;

  if (!_fprint_device_check_claimed (rdev, invocation, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (!can_start_action (rdev, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  priv->current_action = ACTION_DELETE;
  action_unset = rdev;

  session = session_data_get (priv);

  if (!delete_enrolled_fingers (rdev, session->username, FP_FINGER_UNKNOWN, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  fprint_dbus_device_complete_delete_enrolled_fingers2 (dbus_dev,
                                                        invocation);
  return TRUE;
}

static gboolean
fprint_device_delete_enrolled_finger (FprintDBusDevice      *dbus_dev,
                                      GDBusMethodInvocation *invocation,
                                      const gchar           *finger_name)
{
  FprintDevice *rdev = FPRINT_DEVICE (dbus_dev);
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);
  FpFinger finger = finger_name_to_fp_finger (finger_name);

  g_autoptr(FprintDeviceActionUnset) action_unset = NULL;
  g_autoptr(SessionData) session = NULL;
  g_autoptr(GError) error = NULL;

  if (!_fprint_device_check_claimed (rdev, invocation, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (finger == FP_FINGER_UNKNOWN)
    {
      g_dbus_method_invocation_return_error_literal (invocation,
                                                     FPRINT_ERROR,
                                                     FPRINT_ERROR_INVALID_FINGERNAME,
                                                     "Invalid finger name");
      return TRUE;
    }

  if (!can_start_action (rdev, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  priv->current_action = ACTION_DELETE;

  session = session_data_get (priv);
  action_unset = rdev;

  if (!delete_enrolled_fingers (rdev, session->username, finger, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  fprint_dbus_device_complete_delete_enrolled_finger (dbus_dev, invocation);
  return TRUE;
}

static gboolean
handle_unauthorized_access (FprintDevice          *rdev,
                            GDBusMethodInvocation *invocation,
                            GError                *error)
{
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);

  g_assert (error);

  g_warning ("Authorization denied to %s to call method '%s' for device '%s': %s",
             g_dbus_method_invocation_get_sender (invocation),
             g_dbus_method_invocation_get_method_name (invocation),
             fp_device_get_name (priv->dev),
             error->message);
  g_dbus_method_invocation_return_gerror (invocation, error);

  return FALSE;
}

static gboolean
action_authorization_handler (GDBusInterfaceSkeleton *interface,
                              GDBusMethodInvocation  *invocation,
                              gpointer                user_data)
{
  FprintDBusDevice *dbus_dev = FPRINT_DBUS_DEVICE (interface);
  FprintDevice *rdev = FPRINT_DEVICE (dbus_dev);
  FprintDevicePrivate *priv = fprint_device_get_instance_private (rdev);
  FprintDevicePermission required_perms;
  gboolean needs_user_auth = FALSE;

  g_autoptr(GError) error = NULL;
  const gchar *method_name;

  method_name = g_dbus_method_invocation_get_method_name (invocation);

  g_debug ("Requesting authorization from %s to call method '%s' for device '%s'",
           g_dbus_method_invocation_get_sender (invocation), method_name,
           fp_device_get_name (priv->dev));

  if (g_str_equal (method_name, "Claim"))
    needs_user_auth = TRUE;
  else if (g_str_equal (method_name, "DeleteEnrolledFingers"))
    needs_user_auth = TRUE;
  else if (g_str_equal (method_name, "ListEnrolledFingers"))
    needs_user_auth = TRUE;

  /* This is just a quick check in order to avoid authentication if
   * the user cannot make the call at this time anyway.
   * The method handler itself is required to check again! */
  if (!_fprint_device_check_claimed (rdev, invocation, &error))
    return handle_unauthorized_access (rdev, invocation, error);

  if (needs_user_auth &&
      !fprintd_device_authorize_user (rdev, invocation, &error))
    return handle_unauthorized_access (rdev, invocation, error);

  required_perms = get_permissions_for_invocation (invocation);

  /* This may possibly block the invocation till the user has not
   * provided an authentication method, so other calls could arrive */
  if (!fprint_device_check_polkit_for_permissions (rdev, invocation,
                                                   required_perms,
                                                   &error))
    return handle_unauthorized_access (rdev, invocation, error);

  g_debug ("Authorization granted to %s to call method '%s' for device '%s'!",
           g_dbus_method_invocation_get_sender (invocation),
           g_dbus_method_invocation_get_method_name (invocation),
           fp_device_get_name (priv->dev));

  return TRUE;
}

static void
fprint_device_dbus_skeleton_iface_init (FprintDBusDeviceIface *iface)
{
  iface->handle_claim = fprint_device_claim;
  iface->handle_delete_enrolled_finger = fprint_device_delete_enrolled_finger;
  iface->handle_delete_enrolled_fingers = fprint_device_delete_enrolled_fingers;
  iface->handle_delete_enrolled_fingers2 = fprint_device_delete_enrolled_fingers2;
  iface->handle_enroll_start = fprint_device_enroll_start;
  iface->handle_enroll_stop = fprint_device_enroll_stop;
  iface->handle_list_enrolled_fingers = fprint_device_list_enrolled_fingers;
  iface->handle_release = fprint_device_release;
  iface->handle_verify_start = fprint_device_verify_start;
  iface->handle_verify_stop = fprint_device_verify_stop;
}
