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

#ifndef __FPRINTD_H__
#define __FPRINTD_H__

#include <glib.h>
#include <dbus/dbus-glib.h>

/* General */
#define FPRINT_SERVICE_NAME "net.reactivated.Fprint"
extern DBusGConnection *fprintd_dbus_conn;
GQuark fprint_error_quark(void);

/* Errors */
#define FPRINT_ERROR fprint_error_quark()
typedef enum {
	FPRINT_ERROR_INTERNAL,
	FPRINT_ERROR_DISCOVER_PRINTS,
	FPRINT_ERROR_PRINT_NOT_FOUND,
	FPRINT_ERROR_PRINT_LOAD,
	FPRINT_ERROR_NO_SUCH_LOADED_PRINT,
	FPRINT_ERROR_CLAIM_DEVICE,
	FPRINT_ERROR_VERIFY_START,
	FPRINT_ERROR_VERIFY_STOP,
	FPRINT_ERROR_ENROLL_START,
	FPRINT_ERROR_ENROLL_STOP,
	FPRINT_ERROR_FAILED,
} FprintError;

/* Manager */
#define FPRINT_TYPE_MANAGER            (fprint_manager_get_type())
#define FPRINT_MANAGER(object)         (G_TYPE_CHECK_INSTANCE_CAST((object), FPRINT_TYPE_MANAGER, FprintManager))
#define FPRINT_MANAGER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), FPRINT_TYPE_MANAGER, FprintManagerClass))
#define FPRINT_IS_MANAGER(object)      (G_TYPE_CHECK_INSTANCE_TYPE((object), FPRINT_TYPE_MANAGER))
#define FPRINT_IS_MANAGER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), FPRINT_TYPE_MANAGER))
#define FPRINT_MANAGER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj), FPRINT_TYPE_MANAGER, FprintManagerClass))

struct FprintManager {
	GObject parent;
};

struct FprintManagerClass {
	GObjectClass parent;
};

typedef struct FprintManager FprintManager;
typedef struct FprintManagerClass FprintManagerClass;

FprintManager *fprint_manager_new(void);
GError *fprint_manager_get_error(FprintManager *manager);
GType fprint_manager_get_type(void);

/* Device */
#define FPRINT_TYPE_DEVICE            (fprint_device_get_type())
#define FPRINT_DEVICE(object)         (G_TYPE_CHECK_INSTANCE_CAST((object), FPRINT_DEVICE_TYPE, FprintDevice))
#define FPRINT_DEVICE_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), FPRINT_DEVICE_TYPE, FprintDeviceClass))
#define FPRINT_IS_DEVICE(object)      (G_TYPE_CHECK_INSTANCE_TYPE((object), FPRINT_TYPE_DEVICE))
#define FPRINT_IS_DEVICE_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), FPRINT_TYPE_DEVICE))
#define FPRINT_DEVICE_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj), FPRINT_TYPE_DEVICE, FprintDeviceClass))

struct FprintDevice {
	GObject parent;
};

struct FprintDeviceClass {
	GObjectClass parent;
};

typedef struct FprintDevice FprintDevice;
typedef struct FprintDeviceClass FprintDeviceClass;

FprintDevice *fprint_device_new(struct fp_dscv_dev *ddev);
GType fprint_device_get_type(void);
guint32 _fprint_device_get_id(FprintDevice *rdev);
/* Print */
/* TODO */

/* Binding data included in main.c thorugh server-bindings.h which individual
 * class implementations need to access.
 */
extern const DBusGObjectInfo dbus_glib_fprint_manager_object_info;
extern const DBusGObjectInfo dbus_glib_fprint_device_object_info;

#endif

