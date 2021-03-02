/*
 * pam_fprint: PAM module for fingerprint authentication through fprintd
 * Copyright (C) 2007 Daniel Drake <dsd@gentoo.org>
 * Copyright (C) 2008-2014, 2017-2020 Bastien Nocera <hadess@hadess.net>
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

#include <config.h>

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <string.h>
#include <syslog.h>
#include <errno.h>

#include <libintl.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <poll.h>

#define PAM_SM_AUTH
#include <security/pam_modules.h>
#include <security/pam_ext.h>

#define _(s) ((char *) dgettext (GETTEXT_PACKAGE, s))
#define TR(s) dgettext (GETTEXT_PACKAGE, s)
#define N_(s) (s)

#include "fingerprint-strings.h"
#include "pam_fprintd_autoptrs.h"

#define DEFAULT_MAX_TRIES 3
#define DEFAULT_TIMEOUT 30
#define MIN_TIMEOUT 10

#define DEBUG_MATCH "debug="
#define MAX_TRIES_MATCH "max-tries="
#define TIMEOUT_MATCH "timeout="

static bool debug = false;
static unsigned max_tries = DEFAULT_MAX_TRIES;
static unsigned timeout = DEFAULT_TIMEOUT;

#define USEC_PER_SEC ((uint64_t) 1000000ULL)
#define NSEC_PER_USEC ((uint64_t) 1000ULL)
#define USEC_PER_MSEC ((uint64_t) 1000ULL)

static size_t user_enrolled_prints_num (pam_handle_t *pamh,
                                        sd_bus       *bus,
                                        const char   *dev,
                                        const char   *username);

static uint64_t
now (void)
{
  struct timespec ts;

  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (uint64_t) ts.tv_sec * USEC_PER_SEC + (uint64_t) ts.tv_nsec / NSEC_PER_USEC;
}

static bool
str_has_prefix (const char *s, const char *prefix)
{
  if (s == NULL || prefix == NULL)
    return false;
  return strncmp (s, prefix, strlen (prefix)) == 0;
}

static bool
send_msg (pam_handle_t *pamh, const char *msg, int style)
{
  const struct pam_message mymsg = {
    .msg_style = style,
    .msg = msg,
  };
  const struct pam_message *msgp = &mymsg;
  const struct pam_conv *pc;
  struct pam_response *resp;

  if (pam_get_item (pamh, PAM_CONV, (const void **) &pc) != PAM_SUCCESS)
    return false;

  if (!pc || !pc->conv)
    return false;

  return pc->conv (1, &msgp, &resp, pc->appdata_ptr) == PAM_SUCCESS;
}

static bool
send_info_msg (pam_handle_t *pamh, const char *msg)
{
  return send_msg (pamh, msg, PAM_TEXT_INFO);
}

static bool
send_err_msg (pam_handle_t *pamh, const char *msg)
{
  return send_msg (pamh, msg, PAM_ERROR_MSG);
}

static char *
open_device (pam_handle_t *pamh,
             sd_bus       *bus,
             const char   *username,
             bool         *has_multiple_devices)
{
  pf_auto (sd_bus_error) error = SD_BUS_ERROR_NULL;
  pf_autoptr (sd_bus_message) m = NULL;
  size_t num_devices;
  size_t max_prints;
  const char *path = NULL;
  const char *s;
  int r;

  *has_multiple_devices = false;

  if (sd_bus_call_method (bus,
                          "net.reactivated.Fprint",
                          "/net/reactivated/Fprint/Manager",
                          "net.reactivated.Fprint.Manager",
                          "GetDevices",
                          &error,
                          &m,
                          NULL) < 0)
    {
      pam_syslog (pamh, LOG_ERR, "GetDevices failed: %s", error.message);
      return NULL;
    }

  r = sd_bus_message_enter_container (m, 'a', "o");
  if (r < 0)
    {
      pam_syslog (pamh, LOG_ERR, "Failed to parse answer from GetDevices(): %d", r);
      return NULL;
    }

  num_devices = 0;
  max_prints = 0;
  while (sd_bus_message_read_basic (m, 'o', &s) > 0)
    {
      size_t enrolled_prints = user_enrolled_prints_num (pamh, bus, s, username);

      if (debug)
        pam_syslog (pamh, LOG_DEBUG, "%s prints registered: %" PRIu64, s, enrolled_prints);

      if (enrolled_prints > max_prints)
        {
          max_prints = enrolled_prints;
          path = s;
        }

      num_devices++;
    }
  *has_multiple_devices = (num_devices > 1);
  if (debug)
    pam_syslog (pamh, LOG_DEBUG, "Using device %s (out of %ld devices)", path, num_devices);

  sd_bus_message_exit_container (m);

  return path ? strdup (path) : NULL;
}

typedef struct
{
  char         *dev;
  bool          has_multiple_devices;

  unsigned      max_tries;
  char         *result;
  bool          timed_out;
  bool          is_swipe;
  bool          verify_started;
  int           verify_ret;
  pam_handle_t *pamh;

  char         *driver;
} verify_data;

static void
verify_data_free (verify_data *data)
{
  free (data->result);
  free (data->driver);
  free (data->dev);
  free (data);
}

PF_DEFINE_AUTOPTR_CLEANUP_FUNC (verify_data, verify_data_free)

static int
verify_result (sd_bus_message *m,
               void           *userdata,
               sd_bus_error   *ret_error)
{
  verify_data *data = userdata;
  const char *msg;
  const char *result = NULL;
  /* see https://github.com/systemd/systemd/issues/14643 */
  uint64_t done = false;
  int r;

  if (!sd_bus_message_is_signal (m, "net.reactivated.Fprint.Device", "VerifyStatus"))
    {
      pam_syslog (data->pamh, LOG_ERR, "Not the signal we expected (iface: %s, member: %s)",
                  sd_bus_message_get_interface (m),
                  sd_bus_message_get_member (m));
      return 0;
    }

  if ((r = sd_bus_message_read (m, "sb", &result, &done)) < 0)
    {
      pam_syslog (data->pamh, LOG_ERR, "Failed to parse VerifyResult signal: %d", r);
      data->verify_ret = PAM_AUTHINFO_UNAVAIL;
      return 0;
    }

  if (!data->verify_started)
    {
      pam_syslog (data->pamh, LOG_ERR, "Unexpected VerifyResult '%s', %" PRIu64 " signal", result, done);
      return 0;
    }

  if (debug)
    pam_syslog (data->pamh, LOG_DEBUG, "Verify result: %s (done: %d)", result, done ? 1 : 0);

  if (data->result)
    {
      free (data->result);
      data->result = NULL;
    }

  if (done && result)
    {
      data->result = strdup (result);
      return 0;
    }

  msg = verify_result_str_to_msg (result, data->is_swipe);
  if (!msg)
    {
      data->result = strdup ("Protocol error with fprintd!");
      return 0;
    }
  send_err_msg (data->pamh, msg);

  return 0;
}

static int
verify_finger_selected (sd_bus_message *m,
                        void           *userdata,
                        sd_bus_error   *ret_error)
{
  verify_data *data = userdata;
  const char *finger_name = NULL;
  pf_autofree char *msg = NULL;

  if (sd_bus_message_read_basic (m, 's', &finger_name) < 0)
    {
      pam_syslog (data->pamh, LOG_ERR, "Failed to parse VerifyFingerSelected signal: %d", errno);
      data->verify_ret = PAM_AUTHINFO_UNAVAIL;
      return 0;
    }

  if (!data->verify_started)
    {
      pam_syslog (data->pamh, LOG_ERR, "Unexpected VerifyFingerSelected %s signal", finger_name);
      return 0;
    }

  msg = finger_str_to_msg (finger_name, data->driver, data->is_swipe);
  if (!msg)
    {
      data->result = strdup ("Protocol error with fprintd!");
      return 0;
    }
  if (debug)
    pam_syslog (data->pamh, LOG_DEBUG, "verify_finger_selected %s", msg);
  send_info_msg (data->pamh, msg);

  return 0;
}

/* See https://github.com/systemd/systemd/issues/14636 */
static int
get_property_string (sd_bus       *bus,
                     const char   *destination,
                     const char   *path,
                     const char   *interface,
                     const char   *member,
                     sd_bus_error *error,
                     char        **ret)
{

  pf_autoptr (sd_bus_message) reply = NULL;
  const char *s;
  char *n;
  int r;

  r = sd_bus_call_method (bus, destination, path, "org.freedesktop.DBus.Properties", "Get", error, &reply, "ss", interface, member);
  if (r < 0)
    return r;

  r = sd_bus_message_enter_container (reply, 'v', "s");
  if (r < 0)
    return sd_bus_error_set_errno (error, r);

  r = sd_bus_message_read_basic (reply, 's', &s);
  if (r < 0)
    return sd_bus_error_set_errno (error, r);

  n = strdup (s);
  if (!n)
    return sd_bus_error_set_errno (error, -ENOMEM);

  *ret = n;
  return 0;
}


static int
verify_started_cb (sd_bus_message *m,
                   void           *userdata,
                   sd_bus_error   *ret_error)
{
  const sd_bus_error *error = sd_bus_message_get_error (m);
  verify_data *data = userdata;

  if (error)
    {
      if (sd_bus_error_has_name (error, "net.reactivated.Fprint.Error.NoEnrolledPrints"))
        {
          pam_syslog (data->pamh, LOG_DEBUG, "No prints enrolled");
          data->verify_ret = PAM_AUTHINFO_UNAVAIL;
        }
      else
        {
          data->verify_ret = PAM_AUTH_ERR;
        }

      if (debug)
        pam_syslog (data->pamh, LOG_DEBUG, "VerifyStart failed: %s", error->message);

      return 1;
    }

  if (debug)
    pam_syslog (data->pamh, LOG_DEBUG, "VerifyStart completed successfully");

  data->verify_started = true;

  return 1;
}

static void
fd_cleanup (int *fd)
{
  if (*fd >= 0)
    close (*fd);
}

typedef int fd_int;
PF_DEFINE_AUTO_CLEAN_FUNC (fd_int, fd_cleanup);

static int
do_verify (sd_bus      *bus,
           verify_data *data)
{
  pf_autoptr (sd_bus_slot) verify_status_slot = NULL;
  pf_autoptr (sd_bus_slot) verify_finger_selected_slot = NULL;
  pf_autofree char *scan_type = NULL;
  sigset_t signals;
  fd_int signal_fd = -1;
  int r;

  /* Get some properties for the device */
  r = get_property_string (bus,
                           "net.reactivated.Fprint",
                           data->dev,
                           "net.reactivated.Fprint.Device",
                           "scan-type",
                           NULL,
                           &scan_type);
  if (r < 0)
    pam_syslog (data->pamh, LOG_ERR, "Failed to get scan-type for %s: %d", data->dev, r);
  if (debug)
    pam_syslog (data->pamh, LOG_DEBUG, "scan-type for %s: %s", data->dev, scan_type);
  if (str_equal (scan_type, "swipe"))
    data->is_swipe = true;

  if (data->has_multiple_devices)
    {
      get_property_string (bus,
                           "net.reactivated.Fprint",
                           data->dev,
                           "net.reactivated.Fprint.Device",
                           "name",
                           NULL,
                           &data->driver);
      if (r < 0)
        pam_syslog (data->pamh, LOG_ERR, "Failed to get driver name for %s: %d", data->dev, r);
      if (debug && r == 0)
        pam_syslog (data->pamh, LOG_DEBUG, "driver name for %s: %s", data->dev, data->driver);
    }

  sd_bus_match_signal (bus,
                       &verify_status_slot,
                       "net.reactivated.Fprint",
                       data->dev,
                       "net.reactivated.Fprint.Device",
                       "VerifyStatus",
                       verify_result,
                       data);

  sd_bus_match_signal (bus,
                       &verify_finger_selected_slot,
                       "net.reactivated.Fprint",
                       data->dev,
                       "net.reactivated.Fprint.Device",
                       "VerifyFingerSelected",
                       verify_finger_selected,
                       data);

  sigemptyset (&signals);
  sigaddset (&signals, SIGINT);
  signal_fd = signalfd (signal_fd, &signals, SFD_NONBLOCK);

  while (data->max_tries > 0)
    {
      uint64_t verification_end = now () + (timeout * USEC_PER_SEC);

      data->timed_out = false;
      data->verify_started = false;
      data->verify_ret = PAM_INCOMPLETE;

      free (data->result);
      data->result = NULL;

      if (debug)
        pam_syslog (data->pamh, LOG_DEBUG, "About to call VerifyStart");

      r = sd_bus_call_method_async (bus,
                                    NULL,
                                    "net.reactivated.Fprint",
                                    data->dev,
                                    "net.reactivated.Fprint.Device",
                                    "VerifyStart",
                                    verify_started_cb,
                                    data,
                                    "s",
                                    "any");

      if (r < 0)
        {
          if (debug)
            pam_syslog (data->pamh, LOG_DEBUG, "VerifyStart call failed: %d", r);
          break;
        }

      for (;;)
        {
          struct signalfd_siginfo siginfo;
          int64_t wait_time;

          wait_time = verification_end - now ();
          if (wait_time <= 0)
            break;

          if (read (signal_fd, &siginfo, sizeof (siginfo)) > 0)
            {
              if (debug)
                pam_syslog (data->pamh, LOG_DEBUG, "Received signal %d during verify", siginfo.ssi_signo);

              /* The only way for this to happen is if we received SIGINT. */
              return PAM_AUTHINFO_UNAVAIL;
            }

          r = sd_bus_process (bus, NULL);
          if (r < 0)
            break;
          if (data->verify_ret != PAM_INCOMPLETE)
            break;
          if (!data->verify_started)
            continue;
          if (data->result != NULL)
            break;
          if (r == 0)
            {
              struct pollfd fds[2] = {
                { sd_bus_get_fd (bus), sd_bus_get_events (bus), 0 },
                { signal_fd, POLLIN, 0 },
              };

              if (debug)
                {
                  pam_syslog (data->pamh, LOG_DEBUG,
                              "Waiting for %" PRId64 " seconds (%" PRId64 " usecs)",
                              wait_time / USEC_PER_SEC,
                              wait_time);
                }

              r = poll (fds, 2, wait_time / USEC_PER_MSEC);
              if (r < 0 && errno != EINTR)
                {
                  pam_syslog (data->pamh, LOG_ERR, "Error waiting for events: %d", errno);
                  return PAM_AUTHINFO_UNAVAIL;
                }
            }
        }

      if (data->verify_ret != PAM_INCOMPLETE)
        return data->verify_ret;

      if (now () >= verification_end)
        {
          data->timed_out = true;
          send_info_msg (data->pamh, _("Verification timed out"));
        }

      /* Ignore errors from VerifyStop */
      data->verify_started = false;
      (void) sd_bus_call_method (bus,
                                 "net.reactivated.Fprint",
                                 data->dev,
                                 "net.reactivated.Fprint.Device",
                                 "VerifyStop",
                                 NULL,
                                 NULL,
                                 NULL,
                                 NULL);

      if (data->timed_out)
        {
          return PAM_AUTHINFO_UNAVAIL;
        }
      else
        {
          if (str_equal (data->result, "verify-no-match"))
            {
              send_err_msg (data->pamh, "Failed to match fingerprint");
            }
          else if (str_equal (data->result, "verify-match"))
            {
              return PAM_SUCCESS;
            }
          else if (str_equal (data->result, "verify-unknown-error"))
            {
              return PAM_AUTHINFO_UNAVAIL;
            }
          else if (str_equal (data->result, "verify-disconnected"))
            {
              return PAM_AUTHINFO_UNAVAIL;
            }
          else
            {
              send_err_msg (data->pamh, _("An unknown error occurred"));
              return PAM_AUTH_ERR;
            }
        }
      data->max_tries--;
    }

  if (data->max_tries == 0)
    return PAM_MAXTRIES;

  return PAM_AUTH_ERR;
}

static size_t
user_enrolled_prints_num (pam_handle_t *pamh,
                          sd_bus       *bus,
                          const char   *dev,
                          const char   *username)
{
  pf_auto (sd_bus_error) error = SD_BUS_ERROR_NULL;
  pf_autoptr (sd_bus_message) m = NULL;
  size_t num_fingers = 0;
  const char *s;
  int r;

  r = sd_bus_call_method (bus,
                          "net.reactivated.Fprint",
                          dev,
                          "net.reactivated.Fprint.Device",
                          "ListEnrolledFingers",
                          &error,
                          &m,
                          "s",
                          username);
  if (r < 0)
    {
      /* If ListEnrolledFingers fails then verification should
       * also fail (both use the same underlying call), so we
       * report false here and bail out early.  */
      if (debug)
        pam_syslog (pamh, LOG_DEBUG, "ListEnrolledFingers failed for %s: %s",
                    username, error.message);
      return 0;
    }

  r = sd_bus_message_enter_container (m, 'a', "s");
  if (r < 0)
    {
      pam_syslog (pamh, LOG_ERR, "Failed to parse answer from ListEnrolledFingers(): %d", r);
      return 0;
    }

  while (sd_bus_message_read_basic (m, 's', &s) > 0)
    num_fingers++;
  sd_bus_message_exit_container (m);

  return num_fingers;
}

static void
release_device (pam_handle_t *pamh,
                sd_bus       *bus,
                const char   *dev)
{
  pf_auto (sd_bus_error) error = SD_BUS_ERROR_NULL;

  if (sd_bus_call_method (bus,
                          "net.reactivated.Fprint",
                          dev,
                          "net.reactivated.Fprint.Device",
                          "Release",
                          &error,
                          NULL,
                          NULL,
                          NULL) < 0)
    pam_syslog (pamh, LOG_ERR, "ReleaseDevice failed: %s", error.message);
}

static bool
claim_device (pam_handle_t *pamh,
              sd_bus       *bus,
              const char   *dev,
              const char   *username)
{
  pf_auto (sd_bus_error) error = SD_BUS_ERROR_NULL;

  if (sd_bus_call_method (bus,
                          "net.reactivated.Fprint",
                          dev,
                          "net.reactivated.Fprint.Device",
                          "Claim",
                          &error,
                          NULL,
                          "s",
                          username) < 0)
    {
      if (debug)
        pam_syslog (pamh, LOG_DEBUG, "failed to claim device %s", error.message);
      return false;
    }

  return true;
}

static int
name_owner_changed (sd_bus_message *m,
                    void           *userdata,
                    sd_bus_error   *ret_error)
{
  verify_data *data = userdata;
  const char *name = NULL;
  const char *old_owner = NULL;
  const char *new_owner = NULL;

  if (sd_bus_message_read (m, "sss", &name, &old_owner, &new_owner) < 0)
    {
      pam_syslog (data->pamh, LOG_ERR, "Failed to parse NameOwnerChanged signal: %d", errno);
      data->verify_ret = PAM_AUTHINFO_UNAVAIL;
      return 0;
    }

  if (strcmp (name, "net.reactivated.Fprint") != 0)
    return 0;

  /* Name owner for fprintd changed, give up as we might start listening
   * to events from a new name owner otherwise. */
  data->verify_ret = PAM_AUTHINFO_UNAVAIL;

  pam_syslog (data->pamh, LOG_WARNING, "fprintd name owner changed during operation!");

  return 0;
}

static int
do_auth (pam_handle_t *pamh, const char *username)
{
  pf_autoptr (verify_data) data = NULL;
  pf_autoptr (sd_bus) bus = NULL;
  pf_autoptr (sd_bus_slot) name_owner_changed_slot = NULL;

  data = calloc (1, sizeof (verify_data));
  data->max_tries = max_tries;
  data->pamh = pamh;

  if (sd_bus_open_system (&bus) < 0)
    {
      pam_syslog (pamh, LOG_ERR, "Error with getting the bus: %d", errno);
      return PAM_AUTHINFO_UNAVAIL;
    }

  data->dev = open_device (pamh, bus, username, &data->has_multiple_devices);
  if (data->dev == NULL)
    return PAM_AUTHINFO_UNAVAIL;

  /* Only connect to NameOwnerChanged when needed. In case of automatic startup
   * we rely on the fact that we never see those signals.
   */
  name_owner_changed_slot = NULL;
  sd_bus_match_signal (bus,
                       &name_owner_changed_slot,
                       "org.freedesktop.DBus",
                       "/org/freedesktop/DBus",
                       "org.freedesktop.DBus",
                       "NameOwnerChanged",
                       name_owner_changed,
                       data);

  if (claim_device (pamh, bus, data->dev, username))
    {
      int ret = do_verify (bus, data);
      release_device (pamh, bus, data->dev);
      return ret;
    }

  return PAM_AUTHINFO_UNAVAIL;
}

static bool
is_remote (pam_handle_t *pamh)
{
  const char *rhost = NULL;

  pam_get_item (pamh, PAM_RHOST, (const void **) (const void *) &rhost);

  /* NULL or empty rhost if the host information is not available or set.
   * "localhost" if the host is local.
   * We want to not run for known remote hosts */
  if (rhost != NULL &&
      *rhost != '\0' &&
      strcmp (rhost, "localhost") != 0)
    return true;

  if (sd_session_is_remote (NULL) > 0)
    return true;

  return false;
}

PAM_EXTERN int
pam_sm_authenticate (pam_handle_t *pamh, int flags, int argc,
                     const char **argv)
{
  const char *username;
  int i;

  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

  if (is_remote (pamh))
    return PAM_AUTHINFO_UNAVAIL;

  if (pam_get_user (pamh, &username, NULL) != PAM_SUCCESS)
    return PAM_AUTHINFO_UNAVAIL;

  for (i = 0; i < argc; i++)
    {
      if (argv[i] != NULL)
        {
          if (str_equal (argv[i], "debug"))
            {
              pam_syslog (pamh, LOG_DEBUG, "debug on");
              debug = true;
            }
          else if (str_has_prefix (argv[i], DEBUG_MATCH))
            {
              pam_syslog (pamh, LOG_DEBUG, "debug on");
              const char *value;

              value = argv[i] + strlen (DEBUG_MATCH);
              if (str_equal (value, "on") ||
                  str_equal (value, "true") ||
                  str_equal (value, "1"))
                {
                  pam_syslog (pamh, LOG_DEBUG, "debug on");
                  debug = true;
                }
              else if (str_equal (value, "off") ||
                       str_equal (value, "false") ||
                       str_equal (value, "0"))
                {
                  debug = false;
                }
              else
                {
                  pam_syslog (pamh, LOG_DEBUG, "invalid debug value '%s', disabling", value);
                }
            }
          else if (str_has_prefix (argv[i], MAX_TRIES_MATCH) && strlen (argv[i]) > strlen (MAX_TRIES_MATCH))
            {
              max_tries = atoi (argv[i] + strlen (MAX_TRIES_MATCH));
              if (max_tries < 1)
                {
                  if (debug)
                    pam_syslog (pamh, LOG_DEBUG, "invalid max tries '%s', using %d",
                                argv[i] + strlen (MAX_TRIES_MATCH), DEFAULT_MAX_TRIES);
                  max_tries = DEFAULT_MAX_TRIES;
                }
              if (debug)
                pam_syslog (pamh, LOG_DEBUG, "max_tries specified as: %d", max_tries);
            }
          else if (str_has_prefix (argv[i], TIMEOUT_MATCH) && strlen (argv[i]) <= strlen (TIMEOUT_MATCH) + 2)
            {
              timeout = atoi (argv[i] + strlen (TIMEOUT_MATCH));
              if (timeout < MIN_TIMEOUT)
                {
                  if (debug)
                    pam_syslog (pamh, LOG_DEBUG, "timeout %d secs too low, using %d",
                                timeout, MIN_TIMEOUT);
                  timeout = MIN_TIMEOUT;
                }
              else if (debug)
                {
                  pam_syslog (pamh, LOG_DEBUG, "timeout specified as: %d secs", timeout);
                }
            }
        }
    }

  return do_auth (pamh, username);
}

PAM_EXTERN int
pam_sm_setcred (pam_handle_t *pamh, int flags, int argc,
                const char **argv)
{
  return PAM_SUCCESS;
}

PAM_EXTERN int
pam_sm_chauthtok (pam_handle_t *pamh, int flags, int argc,
                  const char **argv)
{
  return PAM_SUCCESS;
}
