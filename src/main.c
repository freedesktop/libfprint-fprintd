/*
 * fprint D-Bus daemon
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

#include <locale.h>
#include <poll.h>
#include <stdlib.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <fprint.h>
#include <glib-object.h>
#include <glib-unix.h>
#include <gmodule.h>

#include "fprintd.h"
#include "storage.h"
#include "file_storage.h"

fp_storage store;

static gboolean no_timeout = FALSE;
static gboolean g_fatal_warnings = FALSE;

static void
set_storage_file (void)
{
  store.init = &file_storage_init;
  store.deinit = &file_storage_deinit;
  store.print_data_save = &file_storage_print_data_save;
  store.print_data_load = &file_storage_print_data_load;
  store.print_data_delete = &file_storage_print_data_delete;
  store.discover_prints = &file_storage_discover_prints;
  store.discover_users = &file_storage_discover_users;
}

static gboolean
load_storage_module (const char *module_name)
{
  GModule *module;
  g_autofree char *filename = NULL;

  filename = g_module_build_path (PLUGINDIR, module_name);
  module = g_module_open (filename, 0);
  if (module == NULL)
    return FALSE;

  if (!g_module_symbol (module, "init", (gpointer *) &store.init) ||
      !g_module_symbol (module, "deinit", (gpointer *) &store.deinit) ||
      !g_module_symbol (module, "print_data_save", (gpointer *) &store.print_data_save) ||
      !g_module_symbol (module, "print_data_load", (gpointer *) &store.print_data_load) ||
      !g_module_symbol (module, "print_data_delete", (gpointer *) &store.print_data_delete) ||
      !g_module_symbol (module, "discover_prints", (gpointer *) &store.discover_prints))
    {
      g_module_close (module);
      return FALSE;
    }

  g_module_make_resident (module);

  return TRUE;
}

static gboolean
load_conf (void)
{
  g_autofree char *filename = NULL;
  g_autofree char *module_name = NULL;

  g_autoptr(GKeyFile) file = NULL;
  g_autoptr(GError) error = NULL;

  filename = g_build_filename (SYSCONFDIR, "fprintd.conf", NULL);
  file = g_key_file_new ();
  g_debug ("About to load configuration file '%s'", filename);
  if (!g_key_file_load_from_file (file, filename, G_KEY_FILE_NONE, &error))
    {
      g_warning ("Could not open \"%s\": %s\n", filename, error->message);
      return FALSE;
    }

  module_name = g_key_file_get_string (file, "storage", "type", &error);
  if (module_name == NULL)
    return FALSE;

  if (g_str_equal (module_name, "file"))
    {
      set_storage_file ();
      return TRUE;
    }

  return load_storage_module (module_name);
}

static const GOptionEntry entries[] = {
  {"g-fatal-warnings", 0, 0, G_OPTION_ARG_NONE, &g_fatal_warnings, "Make all warnings fatal", NULL},
  {"no-timeout", 't', 0, G_OPTION_ARG_NONE, &no_timeout, "Do not exit after unused for a while", NULL},
  { NULL }
};

static gboolean
sigterm_callback (gpointer data)
{
  GMainLoop *loop = data;

  g_main_loop_quit (loop);
  return FALSE;
}

static void
on_name_acquired (GDBusConnection *connection,
                  const char      *name,
                  gpointer         user_data)
{
  g_debug ("D-Bus service launched with name: %s", name);
}

static void
on_name_lost (GDBusConnection *connection,
              const char      *name,
              gpointer         user_data)
{
  GMainLoop *loop = user_data;

  g_warning ("Failed to get name: %s", name);

  g_main_loop_quit (loop);
}

int
main (int argc, char **argv)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GMainLoop) loop = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(FprintManager) manager = NULL;
  g_autoptr(GDBusConnection) connection = NULL;
  guint32 request_name_ret;

  setlocale (LC_ALL, "");

  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  context = g_option_context_new ("Fingerprint handler daemon");
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);

  if (g_option_context_parse (context, &argc, &argv, &error) == FALSE)
    {
      g_warning ("couldn't parse command-line options: %s\n", error->message);
      return 1;
    }

  if (g_fatal_warnings)
    {
      GLogLevelFlags fatal_mask;

      fatal_mask = g_log_set_always_fatal (G_LOG_FATAL_MASK);
      fatal_mask |= G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL;
      g_log_set_always_fatal (fatal_mask);
    }

  /* Obtain a connection to the system bus */
  connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
  if (!G_IS_DBUS_CONNECTION (connection))
    {
      g_warning ("Failed to open connection to bus: %s", error->message);
      return 1;
    }

  /* Load the configuration file,
   * and the default storage plugin */
  if (!load_conf ())
    set_storage_file ();
  store.init ();

  loop = g_main_loop_new (NULL, FALSE);
  g_unix_signal_add (SIGTERM, sigterm_callback, loop);

  g_debug ("Launching FprintObject");

  /* create the one instance of the Manager object to be shared between
   * all fprintd users. This blocks until all the devices are enumerated */
  manager = fprint_manager_new (connection, no_timeout);

  /* Obtain the well-known name after the manager has been initialized.
   * Otherwise a client immediately enumerating the devices will not see
   * any. */
  request_name_ret = g_bus_own_name_on_connection (connection,
                                                   FPRINT_SERVICE_NAME,
                                                   G_BUS_NAME_OWNER_FLAGS_NONE,
                                                   on_name_acquired,
                                                   on_name_lost,
                                                   loop, NULL);

  g_debug ("entering main loop");
  g_main_loop_run (loop);
  g_bus_unown_name (request_name_ret);
  g_debug ("main loop completed");

  store.deinit ();

  return 0;
}
