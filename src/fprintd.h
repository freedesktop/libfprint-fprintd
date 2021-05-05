/*
 * fprintd header file
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

#pragma once

#include <glib.h>
#include <gio/gio.h>
#include <fprint.h>
#include "fprintd-enums.h"
#include "fprintd-dbus.h"

/* General */
#define TIMEOUT 30
#define FPRINT_SERVICE_NAME "net.reactivated.Fprint"
#define FPRINT_SERVICE_PATH "/net/reactivated/Fprint"

/* Errors */
GQuark fprint_error_quark (void);

#define FPRINT_ERROR fprint_error_quark ()
typedef enum {
  /* developer didn't claim the device */
  FPRINT_ERROR_CLAIM_DEVICE,       /*< nick=net.reactivated.Fprint.Error.ClaimDevice >*/
  /* device is already claimed by somebody else */
  FPRINT_ERROR_ALREADY_IN_USE,       /*< nick=net.reactivated.Fprint.Error.AlreadyInUse >*/
  /* internal error occurred */
  FPRINT_ERROR_INTERNAL,       /*< nick=net.reactivated.Fprint.Error.Internal >*/
  /* PolicyKit refused the action */
  FPRINT_ERROR_PERMISSION_DENIED,       /*< nick=net.reactivated.Fprint.Error.PermissionDenied >*/
  /* No prints are enrolled */
  FPRINT_ERROR_NO_ENROLLED_PRINTS,       /*< nick=net.reactivated.Fprint.Error.NoEnrolledPrints >*/
  /* Prints has already been enrolled */
  FPRINT_ERROR_FINGER_ALREADY_ENROLLED,  /*< nick=net.reactivated.Fprint.Error.FingerAlreadyEnrolled >*/
  /* No actions currently in progress */
  FPRINT_ERROR_NO_ACTION_IN_PROGRESS,       /*< nick=net.reactivated.Fprint.Error.NoActionInProgress >*/
  /* the finger name passed was invalid */
  FPRINT_ERROR_INVALID_FINGERNAME,       /*< nick=net.reactivated.Fprint.Error.InvalidFingername >*/
  /* device does not exist */
  FPRINT_ERROR_NO_SUCH_DEVICE,       /*< nick=net.reactivated.Fprint.Error.NoSuchDevice >*/
  /* Prints cannot be deleted from the fprintd storage */
  FPRINT_ERROR_PRINTS_NOT_DELETED, /*< nick=net.reactivated.Fprint.Error.PrintsNotDeleted >*/
  /* Prints cannot be deleted from the device storage */
  FPRINT_ERROR_PRINTS_NOT_DELETED_FROM_DEVICE, /*< nick=net.reactivated.Fprint.Error.PrintsNotDeletedFromDevice >*/
} FprintError;

/* Enum of possible permissions, orders and nick matter here:
   - The order controls the priority of a required permission when various are
     accepted: the lowest the value, the more priority it has.
   - Nick must match the relative polkit rule.
 */
typedef enum {
  FPRINT_DEVICE_PERMISSION_NONE = 0,
  FPRINT_DEVICE_PERMISSION_VERIFY = (1 << 0),       /*< nick=net.reactivated.fprint.device.verify >*/
  FPRINT_DEVICE_PERMISSION_ENROLL = (1 << 1),       /*< nick=net.reactivated.fprint.device.enroll >*/
  FPRINT_DEVICE_PERMISSION_SETUSERNAME = (1 << 2),       /*< nick=net.reactivated.fprint.device.setusername >*/
} FprintDevicePermission;

/* Manager */
#define FPRINT_TYPE_MANAGER (fprint_manager_get_type ())
G_DECLARE_FINAL_TYPE (FprintManager, fprint_manager, FPRINT, MANAGER, GObject)

struct _FprintManager
{
  GObject parent;
};

FprintManager *fprint_manager_new (GDBusConnection *connection,
                                   gboolean         no_timeout);

/* Device */
#define FPRINT_TYPE_DEVICE (fprint_device_get_type ())
G_DECLARE_FINAL_TYPE (FprintDevice, fprint_device, FPRINT, DEVICE,
                      FprintDBusDeviceSkeleton)

struct _FprintDevice
{
  FprintDBusDeviceSkeleton parent;
};

FprintDevice *fprint_device_new (FpDevice *dev);
guint32 _fprint_device_get_id (FprintDevice *rdev);

void fprint_device_suspend (FprintDevice       *rdev,
                            GAsyncReadyCallback callback,
                            void               *user_data);
void fprint_device_resume (FprintDevice       *rdev,
                           GAsyncReadyCallback callback,
                           void               *user_data);

void fprint_device_suspend_finish (FprintDevice *rdev,
                                   GAsyncResult *result,
                                   GError      **error);
void fprint_device_resume_finish (FprintDevice *rdev,
                                  GAsyncResult *res,
                                  GError      **error);


/* Print */
/* TODO */


/* Some compatibility definitions for older GLib. Copied from from libfprint. */
#if !GLIB_CHECK_VERSION (2, 57, 0)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GTypeClass, g_type_class_unref);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GEnumClass, g_type_class_unref);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GFlagsClass, g_type_class_unref);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GParamSpec, g_param_spec_unref);
#else
/* Re-define G_SOURCE_FUNC as we are technically not allowed to use it with
 * the version we depend on currently. */
#undef G_SOURCE_FUNC
#endif

#define G_SOURCE_FUNC(f) ((GSourceFunc) (void (*)(void))(f))
