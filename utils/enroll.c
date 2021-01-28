/*
 * fprintd example to enroll right index finger
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

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include "fprintd-dbus.h"

#define N_(x) x
#define TR(x) x
#include "fingerprint-strings.h"

static FprintDBusManager *manager = NULL;
static GDBusConnection *connection = NULL;
static char *finger_name = NULL;
static char **usernames = NULL;

typedef enum {
  ENROLL_INCOMPLETE,
  ENROLL_COMPLETED,
  ENROLL_FAILED,
} FprintEnrollStatus;

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
      g_print ("Impossible to enroll: %s\n", error->message);
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
enroll_result (GObject *object, const char *result, gboolean done, void *user_data)
{
  FprintEnrollStatus *enroll_status = user_data;

  g_print ("Enroll result: %s\n", result);
  if (done != FALSE)
    {
      if (g_str_equal (result, "enroll-completed"))
        *enroll_status = ENROLL_COMPLETED;
      else
        *enroll_status = ENROLL_FAILED;
    }
}

static void
proxy_signal_cb (GDBusProxy  *proxy,
                 const gchar *sender_name,
                 const gchar *signal_name,
                 GVariant    *parameters,
                 gpointer     user_data)
{
  if (g_str_equal (signal_name, "EnrollStatus"))
    {
      const gchar *result;
      gboolean done;

      g_variant_get (parameters, "(&sb)", &result, &done);
      enroll_result (G_OBJECT (proxy), result, done, user_data);
    }
}

static FprintEnrollStatus
do_enroll (FprintDBusDevice *dev)
{
  g_autoptr(GError) error = NULL;
  FprintEnrollStatus enroll_status = ENROLL_INCOMPLETE;
  gboolean found;
  guint i;

  g_signal_connect (dev, "g-signal", G_CALLBACK (proxy_signal_cb),
                    &enroll_status);

  found = FALSE;
  for (i = 0; fingers[i].dbus_name != NULL; i++)
    {
      if (g_strcmp0 (fingers[i].dbus_name, finger_name) == 0)
        {
          found = TRUE;
          break;
        }
    }
  if (!found)
    {
      g_autoptr(GString) s = NULL;

      s = g_string_new (NULL);
      g_string_append_printf (s, "Invalid finger name '%s'. Name must be one of ", finger_name);
      for (i = 0; fingers[i].dbus_name != NULL; i++)
        {
          g_string_append_printf (s, "%s", fingers[i].dbus_name);
          if (fingers[i + 1].dbus_name != NULL)
            g_string_append (s, ", ");
        }
      g_warning ("%s", s->str);
      exit (1);
    }

  g_print ("Enrolling %s finger.\n", finger_name);
  if (!fprint_dbus_device_call_enroll_start_sync (dev, finger_name, NULL,
                                                  &error))
    {
      g_print ("EnrollStart failed: %s\n", error->message);
      exit (1);
    }

  while (enroll_status == ENROLL_INCOMPLETE)
    g_main_context_iteration (NULL, TRUE);

  g_signal_handlers_disconnect_by_func (dev, proxy_signal_cb, &enroll_status);

  if (!fprint_dbus_device_call_enroll_stop_sync (dev, NULL, &error))
    {
      g_print ("EnrollStop failed: %s\n", error->message);
      exit (1);
    }

  return enroll_status;
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
  { G_OPTION_REMAINING, '\0', 0, G_OPTION_ARG_STRING_ARRAY, &usernames, NULL, "[username]" },
  { NULL }
};

int
main (int argc, char **argv)
{
  g_autoptr(FprintDBusDevice) dev = NULL;
  GOptionContext *context;
  FprintEnrollStatus status;

  g_autoptr(GError) err = NULL;

  setlocale (LC_ALL, "");

  context = g_option_context_new ("Enroll a fingerprint");
  g_option_context_add_main_entries (context, entries, NULL);

  if (g_option_context_parse (context, &argc, &argv, &err) == FALSE)
    {
      g_print ("couldn't parse command-line options: %s\n", err->message);
      return 1;
    }

  if (finger_name == NULL)
    finger_name = g_strdup ("right-index-finger");

  create_manager ();

  dev = open_device (usernames ? usernames[0] : "");
  status = do_enroll (dev);
  release_device (dev);
  g_free (finger_name);
  g_strfreev (usernames);
  return status == ENROLL_COMPLETED ? EXIT_SUCCESS : EXIT_FAILURE;
}
