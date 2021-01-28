/*
 * fprintd example to verify a fingerprint
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <gio/gio.h>
#include "fprintd-dbus.h"

static FprintDBusManager *manager = NULL;
static GDBusConnection *connection = NULL;
static char *finger_name = NULL;
static gboolean g_fatal_warnings = FALSE;
static char **usernames = NULL;

static void
create_manager (void)
{
  g_autoptr(GError) error = NULL;

  connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
  if (connection == NULL)
    {
      g_print ("Failed to connect to session bus: %s\n", error->message);
      exit (1);
    }

  manager = fprint_dbus_manager_proxy_new_sync (connection,
                                                G_DBUS_PROXY_FLAGS_NONE,
                                                "net.reactivated.Fprint",
                                                "/net/reactivated/Fprint/Manager",
                                                NULL, &error);
  if (manager == NULL)
    {
      g_print ("Failed to get Fprintd manager: %s\n", error->message);
      exit (1);
    }
}

static FprintDBusDevice *
open_device (const char *username)
{
  g_autoptr(FprintDBusDevice) dev = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  if (!fprint_dbus_manager_call_get_default_device_sync (manager, &path,
                                                         NULL, &error))
    {
      g_print ("Impossible to verify: %s\n", error->message);
      exit (1);
    }

  g_print ("Using device %s\n", path);

  dev = fprint_dbus_device_proxy_new_sync (connection,
                                           G_DBUS_PROXY_FLAGS_NONE,
                                           "net.reactivated.Fprint",
                                           path, NULL, &error);

  if (error)
    {
      g_print ("failed to connect to device: %s\n", error->message);
      exit (1);
    }

  if (!fprint_dbus_device_call_claim_sync (dev, username, NULL, &error))
    {
      g_print ("failed to claim device: %s\n", error->message);
      exit (1);
    }

  return g_steal_pointer (&dev);
}

static void
find_finger (FprintDBusDevice *dev, const char *username)
{
  g_autoptr(GError) error = NULL;
  g_auto(GStrv) fingers = NULL;
  guint i;

  if (!fprint_dbus_device_call_list_enrolled_fingers_sync (dev, username,
                                                           &fingers,
                                                           NULL, &error))
    {
      g_print ("ListEnrolledFingers failed: %s\n", error->message);
      exit (1);
    }

  if (fingers == NULL || g_strv_length (fingers) == 0)
    {
      g_print ("No fingers enrolled for this device.\n");
      exit (1);
    }

  g_print ("Listing enrolled fingers:\n");
  for (i = 0; fingers[i] != NULL; i++)
    g_print (" - #%d: %s\n", i, fingers[i]);

  if (finger_name && !g_str_equal (finger_name, "any") &&
      !g_strv_contains ((const char **) fingers, finger_name))
    {
      g_print ("Finger '%s' not enrolled for user %s.\n", finger_name,
               username);
      g_free (finger_name);
      exit (1);
    }

  if (finger_name == NULL)
    finger_name = g_strdup (fingers[0]);
}

struct VerifyState
{
  GError  *error;
  gboolean started;
  gboolean completed;
  gboolean match;
};

static void
verify_result (GObject *object, const char *result, gboolean done, void *user_data)
{
  struct VerifyState *verify_state = user_data;

  g_print ("Verify result: %s (%s)\n", result, done ? "done" : "not done");
  verify_state->match = g_str_equal (result, "verify-match");

  if (done != FALSE)
    verify_state->completed = TRUE;
}

static void
verify_finger_selected (GObject *object, const char *name, void *user_data)
{
  g_print ("Verifying: %s\n", name);
}

static void
verify_started_cb (GObject      *obj,
                   GAsyncResult *res,
                   gpointer      user_data)
{
  struct VerifyState *verify_state = user_data;

  if (fprint_dbus_device_call_verify_start_finish (FPRINT_DBUS_DEVICE (obj), res, &verify_state->error))
    {
      g_print ("Verify started!\n");
      verify_state->started = TRUE;
    }
}

static void
proxy_signal_cb (GDBusProxy  *proxy,
                 const gchar *sender_name,
                 const gchar *signal_name,
                 GVariant    *parameters,
                 gpointer     user_data)
{
  struct VerifyState *verify_state = user_data;

  if (!verify_state->started)
    return;

  if (g_str_equal (signal_name, "VerifyStatus"))
    {
      const gchar *result;
      gboolean done;

      g_variant_get (parameters, "(&sb)", &result, &done);
      verify_result (G_OBJECT (proxy), result, done, user_data);
    }
  else if (g_str_equal (signal_name, "VerifyFingerSelected"))
    {
      const gchar *name;

      g_variant_get (parameters, "(&s)", &name);
      verify_finger_selected (G_OBJECT (proxy), name, user_data);
    }
}

static gboolean
do_verify (FprintDBusDevice *dev)
{
  g_autoptr(GError) error = NULL;
  struct VerifyState verify_state = { 0 };

  /* This one is funny. We connect to the signal immediately to avoid
   * race conditions. However, we must ignore any authentication results
   * that happen before our start call returns.
   * This is because the verify call itself may internally try to verify
   * against fprintd (possibly using a separate account).
   *
   * To do so, we *must* use the async version of the verify call, as the
   * sync version would cause the signals to be queued and only processed
   * after it returns.
   */

  g_signal_connect (dev, "g-signal", G_CALLBACK (proxy_signal_cb),
                    &verify_state);

  fprint_dbus_device_call_verify_start (dev, finger_name, NULL,
                                        verify_started_cb,
                                        &verify_state);

  /* Wait for verify start while discarding any VerifyStatus signals */
  while (!verify_state.started && !verify_state.error)
    g_main_context_iteration (NULL, TRUE);

  if (verify_state.error)
    {
      g_print ("VerifyStart failed: %s\n", verify_state.error->message);
      g_clear_error (&verify_state.error);
      exit (1);
    }

  /* VerifyStatus signals are processing, wait for completion. */
  while (!verify_state.completed)
    g_main_context_iteration (NULL, TRUE);


  g_signal_handlers_disconnect_by_func (dev, proxy_signal_cb,
                                        &verify_state);

  if (!fprint_dbus_device_call_verify_stop_sync (dev, NULL, &error))
    {
      g_print ("VerifyStop failed: %s\n", error->message);
      exit (1);
    }

  return verify_state.match;
}

static void
release_device (FprintDBusDevice *dev)
{
  g_autoptr(GError) error = NULL;
  if (!fprint_dbus_device_call_release_sync (dev, NULL, &error))
    {
      g_print ("ReleaseDevice failed: %s\n", error->message);
      exit (1);
    }
}

static const GOptionEntry entries[] = {
  { "finger", 'f',  0, G_OPTION_ARG_STRING, &finger_name, "Finger selected to verify (default is automatic)", NULL },
  {"g-fatal-warnings", 0, 0, G_OPTION_ARG_NONE, &g_fatal_warnings, "Make all warnings fatal", NULL},
  { G_OPTION_REMAINING, '\0', 0, G_OPTION_ARG_STRING_ARRAY, &usernames, NULL, "[username]" },
  { NULL }
};

int
main (int argc, char **argv)
{
  g_autoptr(FprintDBusDevice) dev = NULL;
  g_autoptr(GError) err = NULL;
  GOptionContext *context;
  const char *username = NULL;
  gboolean match;

  setlocale (LC_ALL, "");

  context = g_option_context_new ("Verify a fingerprint");
  g_option_context_add_main_entries (context, entries, NULL);

  if (g_option_context_parse (context, &argc, &argv, &err) == FALSE)
    {
      g_print ("couldn't parse command-line options: %s\n", err->message);
      return 1;
    }

  if (usernames == NULL)
    username = "";
  else
    username = usernames[0];

  if (g_fatal_warnings)
    {
      GLogLevelFlags fatal_mask;

      fatal_mask = g_log_set_always_fatal (G_LOG_FATAL_MASK);
      fatal_mask |= G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL;
      g_log_set_always_fatal (fatal_mask);
    }

  create_manager ();

  dev = open_device (username);
  find_finger (dev, username);
  match = do_verify (dev);
  release_device (dev);
  return match ? EXIT_SUCCESS : EXIT_FAILURE;
}
