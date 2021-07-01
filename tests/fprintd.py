#! /usr/bin/env python3
# Copyright © 2017, 2019 Red Hat, Inc
# Copyright © 2020 Canonical Ltd
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library. If not, see <http://www.gnu.org/licenses/>.
# Authors:
#       Christian J. Kellner <christian@kellner.me>
#       Benjamin Berg <bberg@redhat.com>
#       Marco Trevisan <marco.trevisan@canonical.com>

import unittest
import time
import subprocess
import os
import os.path
import sys
import tempfile
import glob
import pwd
import re
import shutil
import socket
import struct
import dbusmock
import gi
gi.require_version('FPrint', '2.0')
from gi.repository import GLib, Gio, FPrint
from output_checker import OutputChecker
import cairo

try:
    from subprocess import DEVNULL
except ImportError:
    DEVNULL = open(os.devnull, 'wb')

FPRINT_NAMESPACE = 'net.reactivated.Fprint'
FPRINT_PATH = '/' + FPRINT_NAMESPACE.replace('.', '/')
SERVICE_FILE = '/usr/share/dbus-1/system-services/{}.service'.format(FPRINT_NAMESPACE)

class FprintDevicePermission:
    verify = FPRINT_NAMESPACE.lower() + '.device.verify'
    enroll = FPRINT_NAMESPACE.lower() + '.device.enroll'
    set_username = FPRINT_NAMESPACE.lower() + '.device.setusername'


FINGERS_MAP = {
    "left-thumb": FPrint.Finger.LEFT_THUMB,
    "left-index-finger": FPrint.Finger.LEFT_INDEX,
    "left-middle-finger": FPrint.Finger.LEFT_MIDDLE,
    "left-ring-finger": FPrint.Finger.LEFT_RING,
    "left-little-finger": FPrint.Finger.LEFT_LITTLE,
    "right-thumb": FPrint.Finger.RIGHT_THUMB,
    "right-index-finger": FPrint.Finger.RIGHT_INDEX,
    "right-middle-finger": FPrint.Finger.RIGHT_MIDDLE,
    "right-ring-finger": FPrint.Finger.RIGHT_RING,
    "right-little-finger": FPrint.Finger.RIGHT_LITTLE,
    "any": FPrint.Finger.UNKNOWN,
}

def get_timeout(topic='default'):
    vals = {
        'valgrind': {
            'test': 300,
            'device_sleep': 600,
            'default': 20,
            'daemon_start': 60,
            'daemon_stop': 10,
        },
        'asan': {
            'test': 120,
            'default': 6,
            'device_sleep': 400,
            'daemon_start': 10,
            'daemon_stop': 8,
        },
        'default': {
            'test': 60,
            'device_sleep': 100,
            'default': 3,
            'daemon_start': 5,
            'daemon_stop': 2,
        }
    }

    if os.getenv('VALGRIND') is not None:
        lut = vals['valgrind']
    elif os.getenv('ADDRESS_SANITIZER') is not None:
        lut = vals['asan']
    else:
        lut = vals['default']

    if topic not in lut:
        raise ValueError('invalid topic')
    return lut[topic]


# Copied from libfprint tests
class Connection:

    def __init__(self, addr):
        self.addr = addr

    def __enter__(self):
        self.con = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.con.connect(self.addr)
        return self.con

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.con.close()
        del self.con

def load_image(img):
    png = cairo.ImageSurface.create_from_png(img)

    # Cairo wants 4 byte aligned rows, so just add a few pixel if necessary
    w = png.get_width()
    h = png.get_height()
    w = (w + 3) // 4 * 4
    h = (h + 3) // 4 * 4
    img = cairo.ImageSurface(cairo.Format.A8, w, h)
    cr = cairo.Context(img)

    cr.set_source_rgba(1, 1, 1, 1)
    cr.paint()

    cr.set_source_rgba(0, 0, 0, 0)
    cr.set_operator(cairo.OPERATOR_SOURCE)

    cr.set_source_surface(png)
    cr.paint()

    return img

if hasattr(os.environ, 'TOPSRCDIR'):
    root = os.environ['TOPSRCDIR']
else:
    root = os.path.join(os.path.dirname(__file__), '..')

imgdir = os.path.join(root, 'tests', 'prints')

ctx = GLib.main_context_default()

class FPrintdTest(dbusmock.DBusTestCase):

    @staticmethod
    def path_from_service_file(sf):
        with open(SERVICE_FILE) as f:
                for line in f:
                    if not line.startswith('Exec='):
                        continue
                    return line.split('=', 1)[1].strip()
        return None

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        fprintd = None
        cls._polkitd = None

        cls._has_hotplug = FPrint.Device.find_property("removed") is not None

        if 'FPRINT_BUILD_DIR' in os.environ:
            print('Testing local build')
            build_dir = os.environ['FPRINT_BUILD_DIR']
            fprintd = os.path.join(build_dir, 'fprintd')
        elif 'UNDER_JHBUILD' in os.environ:
            print('Testing JHBuild version')
            jhbuild_prefix = os.environ['JHBUILD_PREFIX']
            fprintd = os.path.join(jhbuild_prefix, 'libexec', 'fprintd')
        else:
            print('Testing installed system binaries')
            fprintd = cls.path_from_service_file(SERVICE_FILE)

        assert fprintd is not None, 'failed to find daemon'
        cls.paths = {'daemon': fprintd }


        cls.tmpdir = tempfile.mkdtemp(prefix='libfprint-')

        cls.sockaddr = os.path.join(cls.tmpdir, 'virtual-image.socket')
        os.environ[cls.socket_env] = cls.sockaddr

        cls.prints = {}
        for f in glob.glob(os.path.join(imgdir, '*.png')):
            n = os.path.basename(f)[:-4]
            cls.prints[n] = load_image(f)


        cls.test_bus = Gio.TestDBus.new(Gio.TestDBusFlags.NONE)
        cls.test_bus.up()
        cls.test_bus.unset()
        addr = cls.test_bus.get_bus_address()
        os.environ['DBUS_SYSTEM_BUS_ADDRESS'] = addr
        cls.dbus = Gio.DBusConnection.new_for_address_sync(addr,
            Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION |
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT, None, None)
        assert cls.dbus.is_closed() == False

    @classmethod
    def tearDownClass(cls):
        cls.dbus.close()
        cls.test_bus.down()
        del cls.dbus
        del cls.test_bus
        shutil.rmtree(cls.tmpdir)
        dbusmock.DBusTestCase.tearDownClass()


    def daemon_start(self, driver='Virtual image device for debugging'):
        timeout = get_timeout('daemon_start')  # seconds
        env = os.environ.copy()
        env['G_DEBUG'] = 'fatal-criticals'
        env['STATE_DIRECTORY'] = (self.state_dir + ':' + '/hopefully/a/state_dir_path/that/shouldnt/be/writable')
        env['RUNTIME_DIRECTORY'] = self.run_dir

        argv = [self.paths['daemon'], '-t']
        valgrind = os.getenv('VALGRIND')
        if valgrind is not None:
            argv.insert(0, 'valgrind')
            argv.insert(1, '--leak-check=full')
            if os.path.exists(valgrind):
                argv.insert(2, '--suppressions=%s' % valgrind)
            self.valgrind = True
        self.kill_daemon = False
        self.daemon = subprocess.Popen(argv,
                                       env=env,
                                       stdout=None,
                                       stderr=subprocess.STDOUT)
        self.addCleanup(self.daemon_stop)

        timeout_count = timeout * 10
        timeout_sleep = 0.1
        while timeout_count > 0:
            time.sleep(timeout_sleep)
            timeout_count -= 1
            try:
                self.manager = Gio.DBusProxy.new_sync(self.dbus,
                                                      Gio.DBusProxyFlags.DO_NOT_AUTO_START,
                                                      None,
                                                      FPRINT_NAMESPACE,
                                                      FPRINT_PATH + '/Manager',
                                                      FPRINT_NAMESPACE + '.Manager',
                                                      None)

                devices = self.manager.GetDevices()
                # Find the virtual device, just in case it is a local run
                # and there is another usable sensor available locally
                for path in devices:
                    dev = Gio.DBusProxy.new_sync(self.dbus,
                                                 Gio.DBusProxyFlags.DO_NOT_AUTO_START,
                                                 None,
                                                 FPRINT_NAMESPACE,
                                                 path,
                                                 FPRINT_NAMESPACE + '.Device',
                                                 None)

                    if driver in str(dev.get_cached_property('name')):
                        self.device = dev
                        self._device_cancellable = Gio.Cancellable()
                        self.addCleanup(self._device_cancellable.cancel)
                        break
                else:
                    print('Did not find virtual device! Probably libfprint was build without the corresponding driver!')

                break
            except GLib.GError:
                pass
        else:
            timeout_time = timeout * 10 * timeout_sleep
            self.fail('daemon did not start in %d seconds' % timeout_time)

    def daemon_stop(self):

        if self.daemon:
            try:
                self.daemon.terminate()
            except OSError:
                pass

            try:
                self.daemon.wait(timeout=get_timeout('daemon_stop'))
            except subprocess.TimeoutExpired as e:
                if self.kill_daemon:
                    self.daemon.kill()
                else:
                    raise(e)

            if not self.kill_daemon:
                self.assertLess(self.daemon.returncode, 128)
                self.assertGreaterEqual(self.daemon.returncode, 0)

        self.daemon = None

    def polkitd_start(self):
        if self._polkitd:
            return

        if 'POLKITD_MOCK_PATH' in os.environ:
            polkitd_template = os.path.join(os.getenv('POLKITD_MOCK_PATH'), 'polkitd.py')
        else:
            polkitd_template = os.path.join(os.path.dirname(__file__), 'dbusmock/polkitd.py')
        print ('Using template from %s' % polkitd_template)

        self._polkitd, self._polkitd_obj = self.spawn_server_template(
            polkitd_template, {}, stdout=subprocess.PIPE)

        return self._polkitd

    def polkitd_stop(self):
        if self._polkitd is None:
            return
        self._polkitd.terminate()
        self._polkitd.wait()

    def polkitd_allow_all(self):
        self._polkitd_obj.SetAllowed([FprintDevicePermission.set_username,
                                      FprintDevicePermission.enroll,
                                      FprintDevicePermission.verify])

    def get_current_user(self):
        return pwd.getpwuid(os.getuid()).pw_name

    def setUp(self):
        self.test_dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.test_dir)
        self.state_dir = os.path.join(self.test_dir, 'state')
        self.run_dir = os.path.join(self.test_dir, 'run')
        self.device_id = 0
        self._async_call_res = {}
        os.environ['FP_DRIVERS_WHITELIST'] = self.device_driver

    def assertFprintError(self, fprint_error):
        if isinstance(fprint_error, list) or isinstance(fprint_error, tuple):
            fprint_error = [ re.escape(e) for e in fprint_error ]
            fprint_error = '({})'.format('|'.join(fprint_error))
        else:
            fprint_error = re.escape(fprint_error)

        return self.assertRaisesRegex(GLib.Error,
            re.escape('GDBus.Error:{}.Error.'.format(FPRINT_NAMESPACE)) +
                '{}:'.format(fprint_error))

    def skipTestIfCanWrite(self, path):
        try:
            os.open(os.path.join(path, "testfile"), os.O_CREAT | os.O_WRONLY)
            self.skipTest('Permissions aren\'t respected (CI environment?)')
        except PermissionError:
            pass

    def get_print_file_path(self, user, finger):
        return os.path.join(self.state_dir, user, self.device_driver,
            str(self.device_id), str(int(finger)))

    def get_print_name_file_path(self, user, finger_name):
        return self.get_print_file_path(user, FINGERS_MAP[finger_name])

    def get_finger_name(self, finger):
        return {v: k for k, v in FINGERS_MAP.items()}[finger]

    def set_print_not_writable(self, user, finger):
        # Replace the print with a directory, so that deletion via unlink will fail
        # But it is still listed, not using chmod as it won't work in CI environment
        print_file = self.get_print_file_path(user, finger)
        if os.path.exists(print_file):
            os.rename(print_file, print_file + '_renamed')
            self.addCleanup(os.rename, print_file + '_renamed', print_file)
        os.makedirs(print_file)
        self.addCleanup(os.rmdir, print_file)

    def assertFingerInStorage(self, user, finger):
        self.assertTrue(os.path.exists(self.get_print_file_path(user, finger)))

    def assertFingerNotInStorage(self, user, finger):
        self.assertFalse(os.path.exists(self.get_print_file_path(user, finger)))

    @property
    def finger_needed(self):
        return self.device.get_cached_property('finger-needed').unpack()

    @property
    def finger_present(self):
        return self.device.get_cached_property('finger-present').unpack()

    @property
    def num_enroll_stages(self):
        return self.device.get_cached_property('num-enroll-stages').unpack()

    # From libfprint tests
    def send_retry(self, retry_error=FPrint.DeviceRetry.TOO_SHORT, con=None):
        if con:
            con.sendall(struct.pack('ii', -1, retry_error))
            return

        with Connection(self.sockaddr) as con:
            self.send_retry(retry_error, con)

    # From libfprint tests
    def send_error(self, error=FPrint.DeviceError.GENERAL, con=None):
        if con:
            con.sendall(struct.pack('ii', -2, error))
            return

        with Connection(self.sockaddr) as con:
            self.send_error(error, con)

    # From libfprint tests
    def send_remove(self, con=None):
        if con:
            con.sendall(struct.pack('ii', -5, 0))
            return

        with Connection(self.sockaddr) as con:
            self.send_remove(con=con)

    # From libfprint tests
    def send_image(self, image, con=None):
        if con:
            img = self.prints[image]
            mem = img.get_data()
            mem = mem.tobytes()
            self.assertEqual(len(mem), img.get_width() * img.get_height())

            encoded_img = struct.pack('ii', img.get_width(), img.get_height())
            encoded_img += mem

            con.sendall(encoded_img)
            return

        with Connection(self.sockaddr) as con:
            self.send_image(image, con)

    def send_finger_automatic(self, automatic, con=None, iterate=True):
        # Set whether finger on/off is reported around images
        if con:
            con.sendall(struct.pack('ii', -3, 1 if automatic else 0))
            return

        with Connection(self.sockaddr) as con:
            self.send_finger_automatic(automatic, con=con, iterate=iterate)

        while iterate and ctx.pending():
            ctx.iteration(False)

    def send_finger_report(self, has_finger, con=None, iterate=True):
        # Send finger on/off
        if con:
            con.sendall(struct.pack('ii', -4, 1 if has_finger else 0))
            return

        with Connection(self.sockaddr) as con:
            self.send_finger_report(has_finger, con=con)

        while iterate and self.finger_present != has_finger:
            ctx.iteration(False)

    def send_sleep(self, con=None):
        self.skipTest('Not implemented for {}'.format(self.device_driver))

    def set_keep_alive(self, value):
        self.skipTest('Not implemented for {}'.format(self.device_driver))

    def _maybe_reduce_enroll_stages(self):
        pass

    def call_proxy_method_async(self, proxy, method, *args):
        def call_handler(proxy, res):
            nonlocal method

            if proxy not in self._async_call_res.keys():
                self._async_call_res[proxy] = {}
            if method not in self._async_call_res[proxy].keys():
                self._async_call_res[proxy][method] = []

            try:
                ret = proxy.call_finish(res)
            except Exception as e:
                ret = e
            self._async_call_res[proxy][method].append(ret)

        self.device.call(method, GLib.Variant(*args),
            Gio.DBusCallFlags.NONE, -1, self._device_cancellable,
            call_handler)

    def call_device_method_async(self, method, *args):
        return self.call_proxy_method_async(self.device, method, *args)

    def wait_for_async_reply(self, proxy, method=None, expected_replies=1):
        if proxy in self._async_call_res:
            proxy_replies = self._async_call_res[proxy]
            if method and method in proxy_replies:
                proxy_replies[method] = []
            else:
                proxy_replies = {}

        def get_replies():
            nonlocal proxy, method
            return (self.get_all_async_replies(proxy=proxy) if not method
                else self.get_async_replies(proxy=proxy, method=method))

        while len(get_replies()) != expected_replies:
            ctx.iteration(True)

        for res in get_replies():
            if isinstance(res, Exception):
                raise res

    def wait_for_device_reply(self, method=None, expected_replies=1):
        return self.wait_for_async_reply(self.device, method=method,
            expected_replies=expected_replies)

    def wait_for_device_reply_relaxed(self, method=None, expected_replies=1, accepted_exceptions=[]):
        try:
            self.wait_for_device_reply(method=method, expected_replies=expected_replies)
        except GLib.Error as e:
            for ae in accepted_exceptions:
                if 'GDBus.Error:{}.Error.{}'.format(FPRINT_NAMESPACE, ae) in str(e):
                    return
            raise(e)

    def get_async_replies(self, method=None, proxy=None):
        method_calls = self._async_call_res.get(proxy if proxy else self.device, {})
        return method_calls.get(method, []) if method else method_calls

    def get_all_async_replies(self, proxy=None):
        method_calls = self.get_async_replies(proxy=proxy)
        all_replies = []
        for method, replies in method_calls.items():
            all_replies.extend(replies)
        return all_replies

    def gdbus_device_method_call_process(self, method, args=[]):
        return subprocess.Popen([
            'gdbus',
            'call',
            '--system',
            '--dest',
            self.device.get_name(),
            '--object-path',
            self.device.get_object_path(),
            '--method',
            '{}.{}'.format(self.device.get_interface_name(), method),
        ] + args, stderr=subprocess.STDOUT, stdout=subprocess.PIPE)

    def call_device_method_from_other_client(self, method, args=[]):
        try:
            proc = self.gdbus_device_method_call_process(method, args)
            proc.wait(timeout=5)
            if proc.returncode != 0:
                raise GLib.GError(proc.stdout.read())
            return proc.stdout.read()
        except subprocess.TimeoutExpired as e:
            raise GLib.GError(e.output)


class FPrintdVirtualImageDeviceBaseTests(FPrintdTest):
    socket_env = 'FP_VIRTUAL_IMAGE'
    device_driver = 'virtual_image'
    driver_name = 'Virtual image device for debugging'
    has_identification = True

class FPrintdVirtualDeviceBaseTest(FPrintdVirtualImageDeviceBaseTests):

    def setUp(self):
        super().setUp()

        self.manager = None
        self.device = None
        self.polkitd_start()
        self.daemon_start(self.driver_name)

        if self.device is None:
            self.skipTest("Need {} device to run the test".format(self.device_driver))

        self._polkitd_obj.SetAllowed([FprintDevicePermission.set_username,
                                      FprintDevicePermission.enroll,
                                      FprintDevicePermission.verify])

        def signal_cb(proxy, sender, signal, params):
            print(signal, params)
            if signal == 'EnrollStatus':
                self._abort = params[1]
                self._last_result = params[0]

                if not self._abort and self._last_result.startswith('enroll-'):
                    # Exit wait loop, onto next enroll state (if any)
                    self._abort = True
                elif self._abort:
                    pass
                else:
                    self._abort = True
                    self._last_result = 'Unexpected signal values'
                    print('Unexpected signal values')
            elif signal == 'VerifyFingerSelected':
                self._selected_finger = params[0]
            elif signal == 'VerifyStatus':
                self._abort = True
                self._last_result = params[0]
                self._verify_stopped = params[1]
            else:
                self._abort = True
                self._last_result = 'Unexpected signal'

        def property_cb(proxy, changed, invalidated):
            print('Changed properties', changed, 'invalidated', invalidated)
            self._changed_properties.append(changed.unpack())

        signal_id = self.device.connect('g-signal', signal_cb)
        self.addCleanup(self.device.disconnect, signal_id)

        signal_id = self.device.connect('g-properties-changed', property_cb)
        self.addCleanup(self.device.disconnect, signal_id)
        self._changed_properties = []

    def tearDown(self):
        self.polkitd_stop()
        self.device = None
        self.manager = None

        super().tearDown()

    def try_release(self):
        if not self.device:
            return
        try:
            self.device.Release()
        except GLib.GError as e:
            if not 'net.reactivated.Fprint.Error.ClaimDevice' in e.message:
                raise(e)

    def wait_for_result(self, expected=None, max_wait=-1):
        self._last_result = None
        self._verify_stopped = False
        self._selected_finger = None
        self._abort = False

        if max_wait > 0:
            def abort_timeout():
                self._abort = True
            GLib.timeout_add(max_wait, abort_timeout)

        while not self._abort:
            ctx.iteration(True)

        self.assertTrue(self._abort)
        self._abort = False

        if expected is not None:
            self.assertEqual(self._last_result, expected)

    def enroll_image(self, img, device=None, finger='right-index-finger',
                     expected_result='enroll-completed', claim_user=None,
                     start=True, stop=True):
        if device is None:
            device = self.device
        if claim_user:
            device.Claim('(s)', claim_user)

        if device is self.device:
            self._maybe_reduce_enroll_stages()

        if start:
            device.EnrollStart('(s)', finger)

        while not self.finger_needed:
            ctx.iteration(False)
        self.assertTrue(self.finger_needed)

        if expected_result == 'enroll-duplicate':
            stages = 1
        else:
            stages = self.num_enroll_stages

        for stage in range(stages):
            self.send_image(img)
            if stage < stages - 1:
                self.wait_for_result('enroll-stage-passed')
            else:
                self.wait_for_result(expected_result)
                self.assertFalse(self.finger_needed)

        if stop:
            device.EnrollStop()

        self.assertEqual(self._last_result, expected_result)
        self.assertFalse(self.finger_needed)

        if claim_user:
            device.Release()

    def enroll_multiple_images(self, images_override={}, return_index=-1):
        enroll_map = {
            'left-thumb': 'whorl',
            'right-index-finger': 'arch',
            'left-little-finger': 'loop-right',
        }
        enroll_map.update(images_override)

        for finger, print in enroll_map.items():
            self.enroll_image(print, finger=finger)

        enrolled = self.device.ListEnrolledFingers('(s)', 'testuser')
        self.assertCountEqual(enroll_map.keys(), enrolled)

        if return_index >= 0:
            return enroll_map[enrolled[return_index]]

        return (enrolled, enroll_map)

    def enroll_users_images(self, enroll_map={}, images_override={}, allow_duplicates=False):
        if not enroll_map:
            enroll_map = {
                'test-user1': { 'left-thumb': 'whorl' },
                'test-user2': { 'right-index-finger': 'arch' },
                'test-user3': { 'left-little-finger': 'loop-right',
                                'left-thumb': 'tented_arch' },
            }
        enroll_map.update(images_override)

        enrolled_prints = []
        enrolled_prints_info = {}
        duplicates_prints_info = {}

        self.try_release()

        for user, print_map in enroll_map.items():
            self.device.Claim('(s)', user)
            for finger, p in print_map.items():
                if allow_duplicates and p in enrolled_prints:
                    prints_infos = duplicates_prints_info.get(p, [])
                    prints_infos.append((user, finger))
                    duplicates_prints_info[p] = prints_infos
                    if self.has_identification:
                        self.enroll_image(p, finger=finger,
                            expected_result='enroll-duplicate')
                    continue
                self.enroll_image(p, finger=finger)
                enrolled_prints.append(p)
                enrolled_prints_info[p] = (user, finger)
            self.device.Release()

        if allow_duplicates and duplicates_prints_info:
            # We can't just enroll duplicates prints, as fprint will check for
            # duplicates prints, so we've to handle this manually, copying the
            # actual prints
            for print_image, print_infos in duplicates_prints_info.items():
                for print_info in print_infos:
                    orig_username, orig_finger = enrolled_prints_info[print_image]
                    dup_username, dup_finger = print_info
                    dup_fp_finger = FINGERS_MAP[dup_finger]

                    orig_path = self.get_print_name_file_path(orig_username, orig_finger)
                    self.assertTrue(os.path.exists(orig_path))

                    with open(orig_path, mode='rb') as print_file:
                        dup_print = FPrint.Print.deserialize(print_file.read())
                        dup_print.set_username(dup_username)
                        dup_print.set_finger(dup_fp_finger)

                        dup_path = self.get_print_name_file_path(dup_username, dup_finger)
                        os.makedirs(os.path.dirname(dup_path), exist_ok=True)
                        with open(dup_path, mode='wb') as new_print_file:
                            new_print_file.write(dup_print.serialize())
                            print('Created ',dup_username,'duplicated',dup_finger,
                                'print in', dup_path)

                    self.assertFingerInStorage(dup_username, dup_fp_finger)
        else:
            self.assertCountEqual(enrolled_prints, set(enrolled_prints))

        for user in enroll_map:
            enrolled_fingers = enroll_map[user].keys()
            if enrolled_fingers:
                enrolled = self.device.ListEnrolledFingers('(s)', user)
                self.assertCountEqual(enrolled_fingers, enrolled)
            else:
                with self.assertFprintError('NoEnrolledPrints'):
                    self.device.ListEnrolledFingers('(s)', user)

        return (enroll_map, enrolled_prints_info)

    def get_secondary_bus_and_device(self, claim=None):
        addr = os.environ['DBUS_SYSTEM_BUS_ADDRESS']

        # Get a separate bus connection
        bus = Gio.DBusConnection.new_for_address_sync(addr,
            Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION |
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT, None, None)
        assert bus.is_closed() == False

        dev_path = self.device.get_object_path()
        dev = Gio.DBusProxy.new_sync(bus,
                                     Gio.DBusProxyFlags.DO_NOT_AUTO_START,
                                     None,
                                     FPRINT_NAMESPACE,
                                     dev_path,
                                     FPRINT_NAMESPACE + '.Device',
                                     None)

        if claim is not None:
            dev.Claim('(s)', claim)

        return bus, dev

    def assertVerifyMatch(self, selected_finger=None):
        self.wait_for_result(expected='verify-match')
        self.assertTrue(self._verify_stopped)

        if selected_finger:
            self.assertEqual(selected_finger, self._selected_finger)

    def assertVerifyNoMatch(self, selected_finger=None):
        self.wait_for_result(expected='verify-no-match')
        self.assertTrue(self._verify_stopped)

        if selected_finger:
            self.assertEqual(selected_finger, self._selected_finger)


class FPrintdVirtualStorageDeviceBaseTest(FPrintdVirtualDeviceBaseTest):

    socket_env = 'FP_VIRTUAL_DEVICE_STORAGE'
    device_driver = 'virtual_device_storage'
    driver_name = 'Virtual device with storage and identification for debugging'
    enroll_stages = 2

    def _send_command(self, con, command, *args):
        params = ' '.join(str(p) for p in args)
        con.sendall('{} {}'.format(command, params).encode('utf-8'))
        res = []
        while True:
            r = con.recv(1024)
            if not r:
                break
            res.append(r)

        return b''.join(res)

    def send_command(self, command, *args):
        self.assertIn(command, ['INSERT', 'REMOVE', 'SCAN', 'ERROR', 'RETRY',
            'FINGER', 'UNPLUG', 'SLEEP', 'SET_ENROLL_STAGES', 'SET_SCAN_TYPE',
            'SET_CANCELLATION_ENABLED', 'LIST', 'IGNORED_COMMAND',
            'SET_KEEP_ALIVE', 'CONT'])

        with Connection(self.sockaddr) as con:
            res = self._send_command(con, command, *args)

        return res

    def send_image(self, image, con=None):
        # This is meant to simulate the image scanning for image device
        self.send_command('SCAN', image)

    def send_error(self, error=FPrint.DeviceError.GENERAL, con=None):
        self.send_command('ERROR', int(error))

    def send_retry(self, retry_error=FPrint.DeviceRetry.TOO_SHORT, con=None):
        self.send_command('RETRY', int(retry_error))

    def send_remove(self, con=None):
        self.send_command('UNPLUG')

    def send_finger_automatic(self, automatic, con=None, iterate=True):
        if not automatic:
            return
        self.skipTest('Not implemented for {}'.format(self.device_driver))

    def send_finger_report(self, has_finger, con=None, iterate=True):
        self.send_command('FINGER', 1 if has_finger else 0)

        while iterate and self.finger_present != has_finger:
            ctx.iteration(False)

    def send_sleep(self, timeout, con=None):
        self.assertGreaterEqual(timeout, 0)
        self.send_command('SLEEP', timeout)

    def set_keep_alive(self, value):
        self.send_command('SET_KEEP_ALIVE', 1 if value else 0)

    def enroll_print(self, nick, finger='right-index-finger', expected_result='enroll-completed'):
        # Using the name of the image as the print id
        super().enroll_image(img=nick, finger=finger, expected_result=expected_result)

    def _maybe_reduce_enroll_stages(self, stages=-1):
        # Reduce the number of default enroll stages, we can go a bit faster
        stages = stages if stages > 0 else self.enroll_stages
        if self.has_identification:
            stages += 1 # Adding the extra stage for duplicates-check
        if self.num_enroll_stages == stages:
            return
        device_stages = stages -1 if self.has_identification else stages
        self.send_command('SET_ENROLL_STAGES', device_stages)
        while self.num_enroll_stages != stages:
            ctx.iteration(True)
        self.assertIn({'num-enroll-stages': stages}, self._changed_properties)
        self._changed_properties.remove({'num-enroll-stages': stages})
        self.assertEqual(self.num_enroll_stages, stages)

    def get_stored_prints(self):
        return self.send_command('LIST').decode('ascii').split('\n')[:-1]


class FPrintdVirtualStorageDeviceTests(FPrintdVirtualStorageDeviceBaseTest):
    def setUp(self):
        super().setUp()
        self.device.Claim('(s)', 'testuser')

    def tearDown(self):
        self.try_release()
        super().tearDown()

    def test_garbage_collect(self):
        # We expect collection in this order
        garbage_collect = [
            'no-metadata-print',
            'FP1-20201216-7-ABCDEFGH-testuser',
            'FP1-20201217-7-12345678-testuser',
        ]
        for e in garbage_collect:
            self.send_command('INSERT', e)
        # Enroll a few prints that must not be touched, sort them in at various points
        enrolled_prints = {
          'FP1-20000101-7-ABCDEFGH-testuser' : 'left-index-finger',
          'FP1-20201231-7-ABCDEFGH-testuser' : 'right-index-finger',
          'no-metadata-new' : 'left-middle-finger',
        }
        # Device supports listing, so no initial cleanup
        for i, f in enrolled_prints.items():
            self.enroll_print(i, f)

        # The virtual device sends a trailing \n
        prints = self.get_stored_prints()
        self.assertEqual(set(prints), set(garbage_collect + list(enrolled_prints.keys())))

        def trigger_garbagecollect():
            self.send_image('some-other-print')
            self.send_command('ERROR', int(FPrint.DeviceError.DATA_FULL))
            self.device.EnrollStart('(s)', 'right-thumb')
            self.device.EnrollStop()

        trigger_garbagecollect()

        prints = self.get_stored_prints()
        garbage_collect.pop()
        self.assertEqual(set(prints), set(garbage_collect + list(enrolled_prints.keys())))

        trigger_garbagecollect()

        prints = self.get_stored_prints()
        garbage_collect.pop()
        self.assertEqual(set(prints), set(garbage_collect + list(enrolled_prints.keys())))

        trigger_garbagecollect()

        prints = self.get_stored_prints()
        garbage_collect.pop()
        self.assertEqual(set(prints), set(garbage_collect + list(enrolled_prints.keys())))

    def test_garbage_collect_on_duplicate(self):
        self._maybe_reduce_enroll_stages(stages=1)
        self.send_command('INSERT', 'stored-print')
        self.device.Release()
        self.device.Claim('(s)', 'testuser')

        self.assertEqual(self.get_stored_prints(), ['stored-print'])
        # Device supports listing, so no initial cleanup
        self.device.EnrollStart('(s)', 'right-thumb')
        self.send_image('stored-print')  # During identify
        self.wait_for_result('enroll-stage-passed')
        self.assertFalse(self.get_stored_prints())

        self.send_image('stored-print')
        self.wait_for_result('enroll-completed')  # During enroll
        self.assertEqual(self.get_stored_prints(), ['stored-print'])
        self.device.EnrollStop()

    def test_garbage_collect_failed_on_duplicate(self):
        self._maybe_reduce_enroll_stages(stages=1)
        self.send_command('INSERT', 'stored-print')
        self.device.Release()
        self.device.Claim('(s)', 'testuser')

        self.assertEqual(self.get_stored_prints(), ['stored-print'])
        # Device supports listing, so no initial cleanup
        self.device.EnrollStart('(s)', 'right-thumb')
        self.send_image('stored-print')  # During identify
        self.send_error(FPrint.DeviceError.PROTO)  # During garbage collecting
        self.wait_for_result('enroll-duplicate')
        self.assertEqual(self.get_stored_prints(), ['stored-print'])

        self.device.EnrollStop()

    def test_delete(self):
        # We expect collection in this order
        garbage_prints = [
            'no-metadata-print',
            'FP1-20201216-7-ABCDEFGH-testuser',
            'FP1-20201217-7-12345678-testuser',
            'FP1-20201216-7-ABCDEFGH-other',
            'FP1-20201217-7-12345678-other',
        ]
        for e in garbage_prints:
            self.send_command('INSERT', e)
        # Enroll a few prints that will be deleted
        enrolled_prints = {
          'FP1-20000101-7-ABCDEFGH-testuser' : 'left-index-finger',
          'FP1-20201231-7-ABCDEFGH-testuser' : 'right-index-finger',
          'no-metadata-new' : 'left-middle-finger',
        }
        for i, f in enrolled_prints.items():
            self.enroll_print(i, f)

        # The virtual device sends a trailing \n
        prints = self.get_stored_prints()
        self.assertEqual(set(prints), set(garbage_prints + list(enrolled_prints.keys())))

        # Now, delete all prints for the user
        self.device.DeleteEnrolledFingers2()

        # And verify they are all gone
        prints = self.get_stored_prints()
        self.assertEqual(set(prints), set(garbage_prints))

    def test_enroll_with_one_stage_only(self):
        self._maybe_reduce_enroll_stages(stages=1)

        self.enroll_print('FP1-20000101-7-ABCDEFGH-testuser', 'left-index-finger')
        self.assertEqual(self.device.ListEnrolledFingers('(s)', 'testuser'), ['left-index-finger'])

    def test_scan_type_changes(self):
        for scan_type in [FPrint.ScanType.PRESS, FPrint.ScanType.SWIPE]:
            scan_type = scan_type.value_nick
            self.send_command('SET_SCAN_TYPE', scan_type)
            while self.device.get_cached_property('scan-type').unpack() != scan_type:
                ctx.iteration(True)
            self.assertIn({'scan-type': scan_type}, self._changed_properties)
            self.assertEqual(self.device.get_cached_property('scan-type').unpack(), scan_type)

class FPrintdVirtualStorageNoListDeviceTests(FPrintdVirtualStorageDeviceBaseTest):

    socket_env = 'FP_VIRTUAL_DEVICE_STORAGE_NO_LIST'

    def setUp(self):
        super().setUp()
        self.device.Claim('(s)', 'testuser')

    def tearDown(self):
        self.try_release()
        super().tearDown()

    def test_clear_storage(self):
        # We expect collection in this order
        garbage_collect = [
            'no-metadata-print',
            'FP1-20201216-7-ABCDEFGH-testuser',
            'FP1-20201217-7-12345678-testuser',
        ]
        for e in garbage_collect:
            self.send_command('INSERT', e)

        # Enroll print, return OK for storage clearing
        self.send_command('CONT', 0)
        self.enroll_print('print-1', 'left-index-finger')

        prints = self.get_stored_prints()
        self.assertEqual(set(prints), {'print-1'})

        self.enroll_print('print-2', 'right-index-finger')

        prints = self.get_stored_prints()
        self.assertEqual(set(prints), {'print-1', 'print-2'})


class FPrintdVirtualNoStorageDeviceBaseTest(FPrintdVirtualStorageDeviceBaseTest):

    socket_env = 'FP_VIRTUAL_DEVICE'
    device_driver = 'virtual_device'
    driver_name = 'Virtual device for debugging'
    has_identification = False


class FPrintdVirtualNoStorageDeviceTest(FPrintdVirtualNoStorageDeviceBaseTest):

    def check_verify_finger_match(self, image, expect_match, finger):
        self.device.VerifyStart('(s)', 'any')
        self.send_image(image)
        if expect_match:
            self.assertVerifyMatch(selected_finger=finger)
        else:
            self.assertVerifyNoMatch(selected_finger=finger)
        self.device.VerifyStop()

    def test_verify_any_finger_match_first_only(self):
        self.device.Claim('(s)', 'testuser')
        self.addCleanup(self.device.Release)

        enrolled, enroll_map = self.enroll_multiple_images()
        self.check_verify_finger_match(enroll_map[enrolled[0]], expect_match=True,
            finger=enrolled[0])
        self.check_verify_finger_match(enroll_map[enrolled[1]], expect_match=False,
            finger=enrolled[0])
        self.check_verify_finger_match(enroll_map[enrolled[2]], expect_match=False,
            finger=enrolled[0])

    def test_verify_any_finger_no_match(self):
        self.device.Claim('(s)', 'testuser')
        self.addCleanup(self.device.Release)

        FPrintdVirtualDeviceClaimedTest.test_verify_any_finger_no_match(self,
            selected_finger=None)


class FPrintdManagerTests(FPrintdVirtualDeviceBaseTest):

    def setUp(self):
        super().setUp()
        self._polkitd_obj.SetAllowed([''])

    def test_manager_get_devices(self):
        self.assertListEqual(self.manager.GetDevices(),
            [ self.device.get_object_path() ])

    def test_manager_get_default_device(self):
        self.assertEqual(self.manager.GetDefaultDevice(),
            self.device.get_object_path())


class FPrintdManagerPreStartTests(FPrintdVirtualImageDeviceBaseTests):

    def test_manager_get_no_devices(self):
        os.environ['FP_DRIVERS_WHITELIST'] = 'hopefully_no_existing_driver'
        self.daemon_start()
        self.assertListEqual(self.manager.GetDevices(), [])

    def test_manager_get_no_default_device(self):
        os.environ['FP_DRIVERS_WHITELIST'] = 'hopefully_no_existing_driver'
        self.daemon_start()

        with self.assertFprintError('NoSuchDevice'):
            self.manager.GetDefaultDevice()

    def test_manager_get_devices_on_name_appeared(self):
        self._appeared_name = None

        def on_name_appeared(connection, name, name_owner):
            self._appeared_name = name

        def on_name_vanished(connection, name):
            self._appeared_name = 'NAME_VANISHED'

        id = Gio.bus_watch_name_on_connection(self.dbus,
            FPRINT_NAMESPACE, Gio.BusNameWatcherFlags.NONE,
            on_name_appeared, on_name_vanished)
        self.addCleanup(Gio.bus_unwatch_name, id)

        self.daemon_start()
        while not self._appeared_name:
            ctx.iteration(True)

        self.assertEqual(self._appeared_name, FPRINT_NAMESPACE)

        try:
            appeared_device = self.dbus.call_sync(
                FPRINT_NAMESPACE,
                FPRINT_PATH + '/Manager',
                FPRINT_NAMESPACE + '.Manager',
                'GetDefaultDevice', None, None,
                Gio.DBusCallFlags.NO_AUTO_START, 500, None)
        except GLib.GError as e:
            if FPRINT_NAMESPACE + '.Error.NoSuchDevice' in e.message:
                self.skipTest("Need virtual_image device to run the test")
            raise(e)

        self.assertIsNotNone(appeared_device)
        [dev_path] = appeared_device
        self.assertTrue(dev_path.startswith(FPRINT_PATH + '/Device/'))


class FPrintdVirtualDeviceTest(FPrintdVirtualDeviceBaseTest):

    def test_name_property(self):
        self.assertEqual(self.device.get_cached_property('name').unpack(),
            self.driver_name)

    def test_enroll_stages_property(self):
        self.assertEqual(self.device.get_cached_property('num-enroll-stages').unpack(), 6)

    def test_scan_type(self):
        self.assertEqual(self.device.get_cached_property('scan-type').unpack(),
            'swipe')

    def test_initial_finger_needed(self):
        self.assertFalse(self.finger_needed)

    def test_initial_finger_needed(self):
        self.assertFalse(self.finger_present)

    def test_allowed_claim_release_enroll(self):
        self._polkitd_obj.SetAllowed([FprintDevicePermission.set_username,
                                      FprintDevicePermission.enroll])
        self.device.Claim('(s)', 'testuser')
        self.device.Release()

    def test_allowed_claim_release_verify(self):
        self._polkitd_obj.SetAllowed([FprintDevicePermission.set_username,
                                      FprintDevicePermission.verify])
        self.device.Claim('(s)', 'testuser')
        self.device.Release()

    def test_allowed_claim_current_user(self):
        self._polkitd_obj.SetAllowed([FprintDevicePermission.enroll])
        self.device.Claim('(s)', '')
        self.device.Release()

        self.device.Claim('(s)', self.get_current_user())
        self.device.Release()

    def test_allowed_list_enrolled_fingers_empty_user(self):
        self._polkitd_obj.SetAllowed([FprintDevicePermission.enroll])
        self.device.Claim('(s)', '')
        self.enroll_image('whorl', finger='left-thumb')

        self._polkitd_obj.SetAllowed([FprintDevicePermission.verify])

        self.assertEqual(self.device.ListEnrolledFingers('(s)', ''), ['left-thumb'])
        self.assertEqual(self.device.ListEnrolledFingers('(s)', self.get_current_user()), ['left-thumb'])

    def test_allowed_list_enrolled_fingers_current_user(self):
        self._polkitd_obj.SetAllowed([FprintDevicePermission.enroll])
        self.device.Claim('(s)', self.get_current_user())
        self.enroll_image('whorl', finger='right-thumb')

        self._polkitd_obj.SetAllowed([FprintDevicePermission.verify])

        self.assertEqual(self.device.ListEnrolledFingers('(s)', ''), ['right-thumb'])
        self.assertEqual(self.device.ListEnrolledFingers('(s)', self.get_current_user()), ['right-thumb'])

    def test_unallowed_claim(self):
        self._polkitd_obj.SetAllowed([''])

        with self.assertFprintError('PermissionDenied'):
            self.device.Claim('(s)', 'testuser')

        self._polkitd_obj.SetAllowed([FprintDevicePermission.set_username])

        with self.assertFprintError('PermissionDenied'):
            self.device.Claim('(s)', 'testuser')

        self._polkitd_obj.SetAllowed([FprintDevicePermission.enroll])

        with self.assertFprintError('PermissionDenied'):
            self.device.Claim('(s)', 'testuser')

        self._polkitd_obj.SetAllowed([FprintDevicePermission.verify])

        with self.assertFprintError('PermissionDenied'):
            self.device.Claim('(s)', 'testuser')

    def test_unallowed_enroll_with_verify_claim(self):
        self._polkitd_obj.SetAllowed([FprintDevicePermission.verify])
        self.device.Claim('(s)', '')

        with self.assertFprintError('PermissionDenied'):
            self.enroll_image('whorl', finger='right-thumb')

    def test_unallowed_delete_with_verify_claim(self):
        self._polkitd_obj.SetAllowed([FprintDevicePermission.verify])
        self.device.Claim('(s)', '')

        with self.assertFprintError('PermissionDenied'):
            self.device.DeleteEnrolledFingers('(s)', 'testuser')

    def test_unallowed_delete2_with_verify_claim(self):
        self._polkitd_obj.SetAllowed([FprintDevicePermission.verify])
        self.device.Claim('(s)', '')

        with self.assertFprintError('PermissionDenied'):
            self.device.DeleteEnrolledFingers2()

    def test_unallowed_delete_single_with_verify_claim(self):
        self._polkitd_obj.SetAllowed([FprintDevicePermission.verify])
        self.device.Claim('(s)', '')

        with self.assertFprintError('PermissionDenied'):
            self.device.DeleteEnrolledFingers('(s)', 'right-thumb')

    def test_unallowed_verify_with_enroll_claim(self):
        self._polkitd_obj.SetAllowed([FprintDevicePermission.enroll])
        self.device.Claim('(s)', '')

        with self.assertFprintError('PermissionDenied'):
            self.device.VerifyStart('(s)', 'any')

    def test_unallowed_claim_current_user(self):
        self._polkitd_obj.SetAllowed([''])

        with self.assertFprintError('PermissionDenied'):
            self.device.Claim('(s)', '')

        with self.assertFprintError('PermissionDenied'):
            self.device.Claim('(s)', self.get_current_user())

    def test_multiple_claims(self):
        self.device.Claim('(s)', 'testuser')

        with self.assertFprintError('AlreadyInUse'):
            self.device.Claim('(s)', 'testuser')

        self.device.Release()

    def test_always_allowed_release(self):
        self.device.Claim('(s)', 'testuser')

        self._polkitd_obj.SetAllowed([''])

        self.device.Release()

    def test_unclaimed_release(self):
        with self.assertFprintError('ClaimDevice'):
            self.device.Release()

    def test_unclaimed_verify_start(self):
        with self.assertFprintError('ClaimDevice'):
            self.device.VerifyStart('(s)', 'any')

    def test_unclaimed_verify_stop(self):
        with self.assertFprintError('ClaimDevice'):
            self.device.VerifyStop()

    def test_unclaimed_enroll_start(self):
        with self.assertFprintError('ClaimDevice'):
            self.device.EnrollStart('(s)', 'left-index-finger')

    def test_unclaimed_enroll_stop(self):
        with self.assertFprintError('ClaimDevice'):
            self.device.EnrollStop()

    def test_unclaimed_delete_enrolled_fingers(self):
        self.enroll_image('whorl', claim_user='foo-user')
        self.device.DeleteEnrolledFingers('(s)', 'foo-user')

    def test_unclaimed_delete_enrolled_fingers_no_prints(self):
        with self.assertFprintError('NoEnrolledPrints'):
            self.device.DeleteEnrolledFingers('(s)', 'testuser')

    def test_unclaimed_delete_enrolled_finger(self):
        with self.assertFprintError('ClaimDevice'):
            self.device.DeleteEnrolledFinger('(s)', 'left-index-finger')

    def test_unclaimed_delete_enrolled_fingers2(self):
        with self.assertFprintError('ClaimDevice'):
            self.device.DeleteEnrolledFingers2()

    def test_unclaimed_list_enrolled_fingers(self):
        self.enroll_image('whorl', finger='left-thumb', claim_user='testuser')
        self.assertEqual(self.device.ListEnrolledFingers('(s)', 'testuser'),
            ['left-thumb'])

    def test_unclaimed_list_enrolled_fingers_error(self):
        with self.assertFprintError('NoEnrolledPrints'):
            self.device.ListEnrolledFingers('(s)', 'testuser')

    def test_unclaimed_list_enrolled_fingers_ignores_invalid(self):
        print_path = self.get_print_file_path('testuser', FPrint.Finger.LEFT_INDEX)
        os.makedirs(os.path.dirname(print_path), exist_ok=True)
        with open(print_path, mode='wb') as new_print_file:
            new_print_file.write(b'I am an invalid print!')

        with self.assertFprintError('NoEnrolledPrints'):
            self.device.ListEnrolledFingers('(s)', 'testuser')

    def test_claim_device_open_fail(self):
        os.rename(self.tmpdir, self.tmpdir + '-moved')
        self.addCleanup(os.rename, self.tmpdir + '-moved', self.tmpdir)

        with self.assertFprintError('Internal'):
            self.device.Claim('(s)', 'testuser')

    def test_claim_from_other_client_is_released_when_vanished(self):
        self.call_device_method_from_other_client('Claim', ['testuser'])
        time.sleep(1)
        self.device.Claim('(s)', 'testuser')
        self.device.Release()

    def test_claim_disconnect(self):
        bus, dev = self.get_secondary_bus_and_device()

        def call_done(obj, result, user_data):
            # Ignore the callback (should be an error)
            pass

        # Do an async call to claim and immediately close
        dev.Claim('(s)', 'testuser', result_handler=call_done)

        # Ensure the call is on the wire, then close immediately
        bus.flush_sync()
        bus.close_sync()

        time.sleep(1)

    def test_enroll_running_disconnect(self):
        bus, dev = self.get_secondary_bus_and_device(claim='testuser')

        # Start an enroll and disconnect, without finishing/cancelling
        dev.EnrollStart('(s)', 'left-index-finger')

        # Ensure the call is on the wire, then close immediately
        bus.flush_sync()
        bus.close_sync()

        time.sleep(1)

    def test_enroll_done_disconnect(self):
        bus, dev = self.get_secondary_bus_and_device(claim='testuser')

        # Start an enroll and disconnect, without finishing/cancelling
        dev.EnrollStart('(s)', 'left-index-finger')

        # This works because we also receive the signals on the main connection
        stages = dev.get_cached_property('num-enroll-stages').unpack()
        for stage in range(stages):
            self.send_image('whorl')
            if stage < stages - 1:
                self.wait_for_result('enroll-stage-passed')
            else:
                self.wait_for_result('enroll-completed')

        bus.close_sync()

        time.sleep(1)

    def test_verify_running_disconnect(self):
        bus, dev = self.get_secondary_bus_and_device(claim='testuser')
        self.enroll_image('whorl', device=dev)

        # Start an enroll and disconnect, without finishing/cancelling
        dev.VerifyStart('(s)', 'right-index-finger')

        bus.close_sync()

        time.sleep(1)

    def test_verify_done_disconnect(self):
        bus, dev = self.get_secondary_bus_and_device(claim='testuser')
        self.enroll_image('whorl', device=dev)

        # Start an enroll and disconnect, without finishing/cancelling
        dev.VerifyStart('(s)', 'right-index-finger')
        self.send_image('whorl')
        # Wait for match and sleep a bit to give fprintd time to wrap up
        self.wait_for_result('verify-match')
        time.sleep(1)

        bus.close_sync()

        time.sleep(1)

    def test_identify_running_disconnect(self):
        bus, dev = self.get_secondary_bus_and_device(claim='testuser')
        self.enroll_image('whorl', device=dev)

        # Start an enroll and disconnect, without finishing/cancelling
        dev.VerifyStart('(s)', 'any')

        bus.close_sync()

        time.sleep(1)

    def test_identify_done_disconnect(self):
        bus, dev = self.get_secondary_bus_and_device(claim='testuser')
        self.enroll_image('whorl', device=dev)

        # Start an enroll and disconnect, without finishing/cancelling
        dev.VerifyStart('(s)', 'any')
        self.send_image('whorl')
        # Wait for match and sleep a bit to give fprintd time to wrap up
        self.wait_for_result('verify-match')
        time.sleep(1)

        bus.close_sync()

        time.sleep(1)

    def test_removal_during_enroll(self):
        if not self._has_hotplug:
            self.skipTest("libfprint is too old for hotplug")

        self._polkitd_obj.SetAllowed([FprintDevicePermission.set_username,
                                      FprintDevicePermission.enroll])
        self.device.Claim('(s)', 'testuser')
        self.device.EnrollStart('(s)', 'left-index-finger')

        # Now remove the device while we are enrolling, which will cause an error
        self.send_remove()
        self.wait_for_result(expected='enroll-unknown-error')

        # The device will still be there now until it is released
        devices = self.manager.GetDevices()
        self.assertIn(self.device.get_object_path(), devices)
        with self.assertFprintError('Internal'):
            self.device.Release()

        # And now it will be gone
        devices = self.manager.GetDevices()
        self.assertNotIn(self.device.get_object_path(), devices)

    def test_concourrent_claim(self):
        self.call_device_method_async('Claim', '(s)', [''])
        self.call_device_method_async('Claim', '(s)', [''])

        with self.assertFprintError('AlreadyInUse'):
            self.wait_for_device_reply(expected_replies=2)

        self.assertIn(GLib.Variant('()', ()), self.get_all_async_replies())


class FPrintdVirtualDeviceStorageTest(FPrintdVirtualStorageDeviceBaseTest,
                                      FPrintdVirtualDeviceTest):
    # Repeat the tests for the Virtual storage device
    def test_claim_error(self):
        self.device.Claim('(s)', self.get_current_user())
        self.addCleanup(self.try_release)
        self.set_keep_alive(True)
        self.device.Release()

        self.send_error(FPrint.DeviceError.PROTO)
        with self.assertFprintError('Internal'):
            self.device.Claim('(s)', 'testuser')


class FPrintdVirtualDeviceClaimedTest(FPrintdVirtualDeviceBaseTest):

    def setUp(self):
        super().setUp()
        self.device.Claim('(s)', 'testuser')

    def tearDown(self):
        self._polkitd_obj.SetAllowed([FprintDevicePermission.enroll])
        self.try_release()
        super().tearDown()

    def test_any_finger_enroll_start(self):
        with self.assertFprintError('InvalidFingername'):
            self.device.EnrollStart('(s)', 'any')

    def test_wrong_finger_enroll_start(self):
        with self.assertFprintError('InvalidFingername'):
            self.device.EnrollStart('(s)', 'sixth-right-finger')

    def test_any_finger_delete_print(self):
        with self.assertFprintError('InvalidFingername'):
            self.device.DeleteEnrolledFinger('(s)', 'any')

    def test_wrong_finger_delete_print(self):
        with self.assertFprintError('InvalidFingername'):
            self.device.DeleteEnrolledFinger('(s)', 'sixth-left-finger')

    def test_delete_with_no_enrolled_prints(self):
        with self.assertFprintError('NoEnrolledPrints'):
            self.device.DeleteEnrolledFinger('(s)', 'left-index-finger')

    def test_verify_with_no_enrolled_prints(self):
        with self.assertFprintError('NoEnrolledPrints'):
            self.device.VerifyStart('(s)', 'any')

    def test_enroll_verify_list_delete(self):
        # This test can trigger a race in older libfprint, only run if we have
        # hotplug support, which coincides with the fixed release.
        if not self._has_hotplug:
            self.skipTest("libfprint is too old for hotplug")

        with self.assertFprintError('NoEnrolledPrints'):
            self.device.ListEnrolledFingers('(s)', 'testuser')

        with self.assertFprintError('NoEnrolledPrints'):
            self.device.ListEnrolledFingers('(s)', 'nottestuser')

        self.enroll_image('whorl')
        self.assertFingerInStorage('testuser', FPrint.Finger.RIGHT_INDEX)

        with self.assertFprintError('NoEnrolledPrints'):
            self.device.ListEnrolledFingers('(s)', 'nottestuser')

        self.assertEqual(self.device.ListEnrolledFingers('(s)', 'testuser'), ['right-index-finger'])

        # Finger is enrolled, try to verify it
        self.device.VerifyStart('(s)', 'any')

        while not self.finger_needed:
            ctx.iteration(True)

        self.assertTrue(self.finger_needed)
        self.assertFalse(self.finger_present)

        # Try a wrong print; will stop verification
        self.send_image('tented_arch')
        self.assertVerifyNoMatch()

        self.device.VerifyStop()
        self.device.VerifyStart('(s)', 'any')

        # Send a retry error (swipe too short); will not stop verification
        self.send_retry()
        self.wait_for_result()
        self.assertFalse(self._verify_stopped)
        self.assertEqual(self._last_result, 'verify-swipe-too-short')

        # Try the correct print; will stop verification
        self.send_image('whorl')
        self.wait_for_result()
        self.assertTrue(self._verify_stopped)
        self.assertEqual(self._last_result, 'verify-match')
        self.device.VerifyStop()

        self.assertEqual(self.device.ListEnrolledFingers('(s)', 'testuser'), ['right-index-finger'])

        # And delete the print(s) again
        self.device.DeleteEnrolledFingers('(s)', 'testuser')

        self.assertFingerNotInStorage('testuser', FPrint.Finger.RIGHT_INDEX)

        with self.assertFprintError('NoEnrolledPrints'):
            self.device.ListEnrolledFingers('(s)', 'testuser')

    def test_enroll_delete_storage_error(self):
        self.enroll_image('whorl')
        self.enroll_image('tented_arch', finger='left-index-finger')

        self.set_print_not_writable('testuser', FPrint.Finger.RIGHT_INDEX)

        self.assertFingerInStorage('testuser', FPrint.Finger.RIGHT_INDEX)
        self.assertFingerInStorage('testuser', FPrint.Finger.LEFT_INDEX)

        with self.assertFprintError('PrintsNotDeleted'):
            self.device.DeleteEnrolledFingers('(s)', 'testuser')

        self.assertFingerInStorage('testuser', FPrint.Finger.RIGHT_INDEX)
        self.assertFingerNotInStorage('testuser', FPrint.Finger.LEFT_INDEX)

    def test_enroll_delete2(self):
        self.enroll_image('whorl')

        self.assertFingerInStorage('testuser', FPrint.Finger.RIGHT_INDEX)

        # And delete the print(s) again using the new API
        self.device.DeleteEnrolledFingers2()

        self.assertFingerNotInStorage('testuser', FPrint.Finger.RIGHT_INDEX)
        self.assertFalse(os.path.exists(os.path.join(self.state_dir, 'testuser')))
        self.assertTrue(os.path.exists(self.state_dir))

    def test_enroll_delete2_multiple(self):
        self.enroll_image('whorl')
        self.enroll_image('tented_arch', finger='left-index-finger')

        self.assertFingerInStorage('testuser', FPrint.Finger.RIGHT_INDEX)
        self.assertFingerInStorage('testuser', FPrint.Finger.LEFT_INDEX)

        self.device.DeleteEnrolledFingers2()

        self.assertFingerNotInStorage('testuser', FPrint.Finger.RIGHT_INDEX)
        self.assertFingerNotInStorage('testuser', FPrint.Finger.LEFT_INDEX)

    def test_enroll_delete2_storage_error(self):
        self.enroll_image('whorl')
        self.enroll_image('tented_arch', finger='left-index-finger')

        self.set_print_not_writable('testuser', FPrint.Finger.RIGHT_INDEX)

        self.assertFingerInStorage('testuser', FPrint.Finger.RIGHT_INDEX)
        self.assertFingerInStorage('testuser', FPrint.Finger.LEFT_INDEX)

        with self.assertFprintError('PrintsNotDeleted'):
            self.device.DeleteEnrolledFingers2()

        self.assertFingerInStorage('testuser', FPrint.Finger.RIGHT_INDEX)
        self.assertFingerNotInStorage('testuser', FPrint.Finger.LEFT_INDEX)

    def test_enroll_delete_single(self):
        self.enroll_image('whorl', finger='right-index-finger')
        self.enroll_image('tented_arch', finger='left-index-finger')

        self.assertFingerInStorage('testuser', FPrint.Finger.RIGHT_INDEX)
        self.assertFingerInStorage('testuser', FPrint.Finger.LEFT_INDEX)

        self.device.DeleteEnrolledFinger('(s)', 'right-index-finger')
        self.assertFingerInStorage('testuser', FPrint.Finger.LEFT_INDEX)
        self.assertFingerNotInStorage('testuser', FPrint.Finger.RIGHT_INDEX)

        self.device.DeleteEnrolledFinger('(s)', 'left-index-finger')
        self.assertFingerNotInStorage('testuser', FPrint.Finger.LEFT_INDEX)
        self.assertFingerNotInStorage('testuser', FPrint.Finger.RIGHT_INDEX)

    def test_enroll_delete_single_storage_error(self):
        self.enroll_image('whorl', finger='right-index-finger')
        self.enroll_image('tented_arch', finger='left-index-finger')

        self.set_print_not_writable('testuser', FPrint.Finger.RIGHT_INDEX)

        self.assertFingerInStorage('testuser', FPrint.Finger.RIGHT_INDEX)
        self.assertFingerInStorage('testuser', FPrint.Finger.LEFT_INDEX)

        with self.assertFprintError('PrintsNotDeleted'):
            self.device.DeleteEnrolledFinger('(s)', 'right-index-finger')

        self.assertFingerInStorage('testuser', FPrint.Finger.LEFT_INDEX)
        self.assertFingerInStorage('testuser', FPrint.Finger.RIGHT_INDEX)

        self.set_print_not_writable('testuser', FPrint.Finger.LEFT_INDEX)

        with self.assertFprintError('PrintsNotDeleted'):
            self.device.DeleteEnrolledFinger('(s)', 'left-index-finger')

        self.assertFingerInStorage('testuser', FPrint.Finger.LEFT_INDEX)
        self.assertFingerInStorage('testuser', FPrint.Finger.RIGHT_INDEX)

    def test_enroll_invalid_storage_dir(self):
        # Directory will not exist yet
        os.makedirs(self.state_dir, mode=0o500)
        self.addCleanup(os.chmod, self.state_dir, mode=0o700)

        self.skipTestIfCanWrite(self.state_dir)

        self.enroll_image('whorl', expected_result='enroll-failed')

    def test_enroll_write_print_error(self):
        self.set_print_not_writable('testuser', FPrint.Finger.LEFT_THUMB)
        self.enroll_image('whorl', expected_result='enroll-failed', finger='left-thumb')

    def test_verify_invalid_storage_dir(self):
        self.enroll_image('whorl')
        os.chmod(self.state_dir, mode=0o000)
        self.addCleanup(os.chmod, self.state_dir, mode=0o700)

        self.skipTestIfCanWrite(self.state_dir)

        with self.assertFprintError('NoEnrolledPrints'):
            self.device.VerifyStart('(s)', 'any')

    def test_verify_read_print_error(self):
        self.enroll_image('whorl', finger='left-thumb')
        self.set_print_not_writable('testuser', FPrint.Finger.LEFT_THUMB)

        with self.assertFprintError('NoEnrolledPrints'):
            self.device.VerifyStart('(s)', 'any')

    def test_enroll_stop_cancels(self):
        self.device.EnrollStart('(s)', 'left-index-finger')
        self.device.EnrollStop()
        self.wait_for_result(expected='enroll-failed')

    def test_verify_stop_cancels(self):
        self.enroll_image('whorl')
        self.device.VerifyStart('(s)', 'any')
        self.device.VerifyStop()
        self.wait_for_result(expected='verify-no-match')

    def test_verify_finger_stop_cancels(self):
        self.enroll_image('whorl', finger='left-thumb')
        self.device.VerifyStart('(s)', 'left-thumb')
        self.device.VerifyStop()

    def test_busy_device_release_on_enroll(self):
        self.device.EnrollStart('(s)', 'left-index-finger')

        self.device.Release()
        self.wait_for_result(expected='enroll-failed')

    def test_busy_device_release_on_verify(self):
        self.enroll_image('whorl', finger='left-index-finger')
        self.device.VerifyStart('(s)', 'any')

        self.device.Release()
        self.wait_for_result(expected='verify-no-match')

    def test_busy_device_release_on_verify_finger(self):
        self.enroll_image('whorl', finger='left-middle-finger')
        self.device.VerifyStart('(s)', 'left-middle-finger')

        self.device.Release()
        self.wait_for_result(expected='verify-no-match')

    def test_enroll_stop_not_started(self):
        with self.assertFprintError('NoActionInProgress'):
            self.device.EnrollStop()

    def test_verify_stop_not_started(self):
        with self.assertFprintError('NoActionInProgress'):
            self.device.VerifyStop()

    def test_verify_finger_match(self):
        self.enroll_image('whorl', finger='left-thumb')
        self.device.VerifyStart('(s)', 'left-thumb')
        self.send_image('whorl')
        self.wait_for_result()
        self.assertTrue(self._verify_stopped)
        self.assertEqual(self._last_result, 'verify-match')
        self.assertEqual(self._selected_finger, 'left-thumb')
        self.device.VerifyStop()

    def test_verify_finger_no_match(self):
        self.enroll_image('whorl', finger='left-thumb')
        self.device.VerifyStart('(s)', 'left-thumb')
        self.send_image('tented_arch')
        self.assertVerifyNoMatch(selected_finger='left-thumb')
        self.device.VerifyStop()

    def test_verify_finger_no_match_restart(self):
        self.enroll_image('whorl', finger='left-thumb')
        self.device.VerifyStart('(s)', 'left-thumb')
        self.send_image('tented_arch')
        self.assertVerifyNoMatch(selected_finger='left-thumb')
        self.device.VerifyStop()

        # Immediately starting again after a no-match must work
        self.device.VerifyStart('(s)', 'left-thumb')
        self.send_image('whorl')
        self.wait_for_result()
        self.assertTrue(self._verify_stopped)
        self.assertEqual(self._last_result, 'verify-match')
        self.assertEqual(self._selected_finger, 'left-thumb')
        self.device.VerifyStop()

    def test_verify_wrong_finger_match(self):
        self.enroll_image('whorl', finger='left-thumb')
        self.device.VerifyStart('(s)', 'left-toe')
        self.send_image('whorl')
        self.wait_for_result()
        self.assertTrue(self._verify_stopped)
        self.assertEqual(self._last_result, 'verify-match')
        self.assertEqual(self._selected_finger, 'any')
        self.device.VerifyStop()

    def test_verify_wrong_finger_no_match(self):
        self.enroll_image('whorl', finger='right-thumb')
        self.device.VerifyStart('(s)', 'right-toe')
        self.send_image('tented_arch')
        self.assertVerifyNoMatch(selected_finger='any')
        self.device.VerifyStop()

    def test_verify_any_finger_match(self):
        second_image = self.enroll_multiple_images(return_index=1)
        self.device.VerifyStart('(s)', 'any')
        self.send_image(second_image)
        self.wait_for_result()
        self.assertTrue(self._verify_stopped)
        self.assertEqual(self._last_result, 'verify-match')
        self.assertEqual(self._selected_finger, 'any')
        self.device.VerifyStop()

    def test_verify_any_finger_no_match(self, selected_finger='any'):
        enrolled, _map = self.enroll_multiple_images()
        verify_image = 'tented_arch'
        self.assertNotIn(verify_image, enrolled)
        self.device.VerifyStart('(s)', 'any')
        self.send_image(verify_image)
        self.assertVerifyNoMatch(selected_finger)
        self.device.VerifyStop()

    def test_verify_any_finger_multiple_users(self):
        enroll_map, enrolled_prints_info = self.enroll_users_images()
        enrolled_users = list(enroll_map)

        for verifying_user in enrolled_users:
            self.device.Claim('(s)', verifying_user)

            for enrolled_user in enrolled_users:
                should_match = enrolled_user == verifying_user

                for finger, print in enroll_map[enrolled_user].items():
                    self.device.VerifyStart('(s)', 'any')
                    self.send_image(print)
                    if should_match:
                        self.assertVerifyMatch(selected_finger='any')
                    else:
                        self.assertVerifyNoMatch(selected_finger='any')
                    self.device.VerifyStop()

            self.device.Release()

    def test_enroll_users_duplicate_prints(self):
        _enroll_map, prints_info = self.enroll_users_images(enroll_map={
            'test-user1': {'left-thumb': 'whorl', 'right-thumb': 'whorl'},
            'test-user2': {'left-index-finger': 'whorl'},
            'test-user3': {'left-little-finger': 'tented_arch'},
        }, allow_duplicates=True)
        self.assertEqual(prints_info, {
            'whorl': ('test-user1', 'left-thumb'),
            'tented_arch': ('test-user3', 'left-little-finger'),
        })

    def test_verify_finger_not_enrolled(self):
        self.enroll_image('whorl', finger='left-thumb')
        with self.assertFprintError('NoEnrolledPrints'):
            self.device.VerifyStart('(s)', 'right-thumb')

    def test_verify_finger_not_enrolled_stops_verification(self):
        self.enroll_image('whorl', finger='left-thumb')
        with self.assertFprintError('NoEnrolledPrints'):
            self.device.VerifyStart('(s)', 'right-thumb')

        with self.assertFprintError('NoActionInProgress'):
            self.device.VerifyStop()

    def test_identify_finger_not_enrolled(self):
        with self.assertFprintError('NoEnrolledPrints'):
            self.device.VerifyStart('(s)', 'any')

    def test_identify_finger_not_enrolled_stops_verification(self):
        with self.assertFprintError('NoEnrolledPrints'):
            self.device.VerifyStart('(s)', 'any')

        with self.assertFprintError('NoActionInProgress'):
            self.device.VerifyStop()

    def test_unallowed_enroll_start(self):
        self._polkitd_obj.SetAllowed([''])

        with self.assertFprintError('PermissionDenied'):
            self.device.EnrollStart('(s)', 'right-index-finger')

        self._polkitd_obj.SetAllowed([FprintDevicePermission.enroll])
        self.enroll_image('whorl')

    def test_always_allowed_enroll_stop(self):
        self.device.EnrollStart('(s)', 'right-index-finger')

        self._polkitd_obj.SetAllowed([''])

        self.device.EnrollStop()

    def test_unallowed_verify_start(self):
        self._polkitd_obj.SetAllowed([''])

        with self.assertFprintError('PermissionDenied'):
            self.device.VerifyStart('(s)', 'any')

    def test_always_allowed_verify_stop(self):
        self.enroll_image('whorl')
        self.device.VerifyStart('(s)', 'any')

        self._polkitd_obj.SetAllowed([''])
        self.device.VerifyStop()

    def test_list_enrolled_fingers_current_user(self):
        self.enroll_image('whorl')
        self._polkitd_obj.SetAllowed([FprintDevicePermission.verify])

        with self.assertFprintError('NoEnrolledPrints'):
            self.device.ListEnrolledFingers('(s)', '')

        with self.assertFprintError('NoEnrolledPrints'):
            self.device.ListEnrolledFingers('(s)', self.get_current_user())

    def test_unallowed_list_enrolled_fingers(self):
        self.enroll_image('whorl')

        self._polkitd_obj.SetAllowed([''])
        with self.assertFprintError('PermissionDenied'):
            self.device.ListEnrolledFingers('(s)', 'testuser')

        self._polkitd_obj.SetAllowed([FprintDevicePermission.set_username])
        with self.assertFprintError('PermissionDenied'):
            self.device.ListEnrolledFingers('(s)', 'testuser')

    def test_unallowed_list_enrolled_fingers_current_user(self):
        self.enroll_image('whorl')

        self._polkitd_obj.SetAllowed([''])
        with self.assertFprintError('PermissionDenied'):
            self.device.ListEnrolledFingers('(s)', '')

        with self.assertFprintError('PermissionDenied'):
            self.device.ListEnrolledFingers('(s)', self.get_current_user())

        self._polkitd_obj.SetAllowed([FprintDevicePermission.set_username])
        with self.assertFprintError('PermissionDenied'):
            self.device.ListEnrolledFingers('(s)', '')

        with self.assertFprintError('PermissionDenied'):
            self.device.ListEnrolledFingers('(s)', self.get_current_user())

    def test_unallowed_delete_enrolled_fingers(self):
        self.enroll_image('whorl')

        self._polkitd_obj.SetAllowed([''])
        with self.assertFprintError('PermissionDenied'):
            self.device.DeleteEnrolledFingers('(s)', 'testuser')

        self._polkitd_obj.SetAllowed([FprintDevicePermission.set_username])
        with self.assertFprintError('PermissionDenied'):
            self.device.DeleteEnrolledFingers('(s)', 'testuser')

    def test_unallowed_delete_enrolled_fingers2(self):
        self.enroll_image('whorl')

        self._polkitd_obj.SetAllowed([''])
        with self.assertFprintError('PermissionDenied'):
            self.device.DeleteEnrolledFingers2()

    def test_unallowed_delete_enrolled_finger(self):
        self.enroll_image('whorl')

        self._polkitd_obj.SetAllowed([''])
        with self.assertFprintError('PermissionDenied'):
            self.device.DeleteEnrolledFinger('(s)', 'left-little-finger')

    def test_delete_enrolled_fingers_from_other_client(self):
        with self.assertFprintError('AlreadyInUse'):
            self.call_device_method_from_other_client('DeleteEnrolledFingers', ['testuser'])

    def test_delete_enrolled_fingers2_from_other_client(self):
        with self.assertFprintError('AlreadyInUse'):
            self.call_device_method_from_other_client('DeleteEnrolledFingers2')

    def test_delete_enrolled_finger_from_other_client(self):
        with self.assertFprintError('AlreadyInUse'):
            self.call_device_method_from_other_client('DeleteEnrolledFinger', ['left-index-finger'])

    def test_release_from_other_client(self):
        with self.assertFprintError('AlreadyInUse'):
            self.call_device_method_from_other_client('Release')

    def test_enroll_start_from_other_client(self):
        with self.assertFprintError('AlreadyInUse'):
            self.call_device_method_from_other_client('EnrollStart', ['left-index-finger'])

    def test_verify_start_from_other_client(self):
        with self.assertFprintError('AlreadyInUse'):
            self.call_device_method_from_other_client('VerifyStart', ['any'])

    def test_verify_start_finger_from_other_client(self):
        with self.assertFprintError('AlreadyInUse'):
            self.call_device_method_from_other_client('VerifyStart', ['left-thumb'])

    def test_enroll_finger_status(self):
        self.assertFalse(self.finger_present)
        self.assertFalse(self.finger_needed)
        self.device.EnrollStart('(s)', 'right-middle-finger')

        self.assertEqual(self._changed_properties, [])

        while not self.finger_needed:
            ctx.iteration(False)

        self.assertIn({'finger-needed': True}, self._changed_properties)

        self.assertTrue(self.finger_needed)
        self.assertFalse(self.finger_present)

        self._changed_properties = []
        self.send_finger_report(True)
        self.assertEqual([{'finger-present': True}], self._changed_properties)
        self.assertTrue(self.finger_needed)
        self.assertTrue(self.finger_present)

        self._changed_properties = []
        self.send_finger_report(False)
        self.assertFalse(self.finger_present)
        self.assertTrue(self.finger_needed)
        self.assertEqual([{'finger-present': False}], self._changed_properties)

        self._changed_properties = []
        self.device.EnrollStop()

        while self.finger_needed:
            ctx.iteration(False)

        self.assertFalse(self.finger_present)
        self.assertFalse(self.finger_needed)
        self.assertEqual([{'finger-needed': False}], self._changed_properties)

    def test_verify_finger_status(self):
        self.assertFalse(self.finger_present)
        self.assertFalse(self.finger_needed)
        self.assertEqual(self._changed_properties, [])

        self.enroll_image('whorl')

        self.assertIn({'finger-needed': True}, self._changed_properties)
        self.assertIn({'finger-needed': False}, self._changed_properties)

        self.assertFalse(self.finger_present)
        self.assertFalse(self.finger_needed)

        self._changed_properties = []
        self.device.VerifyStart('(s)', 'any')
        self.assertEqual(self._changed_properties, [])

        while not self.finger_needed:
            ctx.iteration(False)

        self.assertIn({'finger-needed': True}, self._changed_properties)

        self.assertTrue(self.finger_needed)
        self.assertFalse(self.finger_present)

        self._changed_properties = []
        self.send_finger_report(True)
        self.assertEqual([{'finger-present': True}], self._changed_properties)
        self.assertTrue(self.finger_needed)
        self.assertTrue(self.finger_present)

        self._changed_properties = []
        self.send_finger_report(False)
        self.assertFalse(self.finger_present)
        self.assertTrue(self.finger_needed)
        self.assertEqual([{'finger-present': False}], self._changed_properties)

        self._changed_properties = []
        self.device.VerifyStop()

        while self.finger_needed:
            ctx.iteration(False)

        self.assertFalse(self.finger_present)
        self.assertFalse(self.finger_needed)
        self.assertEqual([{'finger-needed': False}], self._changed_properties)

    def test_concourrent_enroll_start(self):
        self.call_device_method_async('EnrollStart', '(s)', ['left-little-finger'])
        self.call_device_method_async('EnrollStart', '(s)', ['left-thumb'])

        with self.assertFprintError('AlreadyInUse'):
            self.wait_for_device_reply(expected_replies=2)

        self.assertIn(GLib.Variant('()', ()), self.get_all_async_replies())

    def test_concourrent_verify_start(self):
        self.enroll_image('whorl', finger='left-thumb')
        self.call_device_method_async('VerifyStart', '(s)', ['any'])
        self.call_device_method_async('VerifyStart', '(s)', ['left-thumb'])

        with self.assertFprintError('AlreadyInUse'):
            self.wait_for_device_reply(expected_replies=2)

        self.assertIn(GLib.Variant('()', ()), self.get_all_async_replies())

    def test_concourrent_list_enrolled_fingers(self):
        self.enroll_image('whorl')
        self.call_device_method_async('ListEnrolledFingers', '(s)', ['testuser'])
        self.call_device_method_async('ListEnrolledFingers', '(s)', ['testuser'])

        # No failure is expected here since it's all sync
        self.wait_for_device_reply(expected_replies=2)
        self.assertEqual([(['right-index-finger'],), (['right-index-finger'],)],
            [ f.unpack() for f in self.get_all_async_replies() ])

    def test_concourrent_delete_enrolled_fingers(self):
        self.enroll_image('whorl')
        self.call_device_method_async('DeleteEnrolledFingers', '(s)', ['testuser'])
        self.call_device_method_async('DeleteEnrolledFingers', '(s)', ['testuser'])

        accepted_exceptions = ['NoEnrolledPrints']
        if self.device_driver == 'virtual_device_storage':
            accepted_exceptions.append('AlreadyInUse')

        self.wait_for_device_reply_relaxed(expected_replies=2,
            accepted_exceptions=accepted_exceptions)

        self.assertIn(GLib.Variant('()', ()), self.get_all_async_replies())

    def test_concourrent_delete_enrolled_fingers_unclaimed(self):
        self.enroll_image('whorl')
        self.device.Release()
        self.call_device_method_async('DeleteEnrolledFingers', '(s)', ['testuser'])
        self.call_device_method_async('DeleteEnrolledFingers', '(s)', ['testuser'])

        accepted_exceptions = ['NoEnrolledPrints']
        if self.device_driver == 'virtual_device_storage':
            accepted_exceptions.append('AlreadyInUse')

        self.wait_for_device_reply_relaxed(expected_replies=2,
            accepted_exceptions=accepted_exceptions)

        self.assertIn(GLib.Variant('()', ()), self.get_all_async_replies())

    def test_concourrent_delete_enrolled_fingers2(self):
        self.enroll_image('whorl')
        self.call_device_method_async('DeleteEnrolledFingers2', '()', [])
        self.call_device_method_async('DeleteEnrolledFingers2', '()', [])

        accepted_exceptions = ['NoEnrolledPrints']
        if self.device_driver == 'virtual_device_storage':
            accepted_exceptions.append('AlreadyInUse')

        self.wait_for_device_reply_relaxed(expected_replies=2,
            accepted_exceptions=accepted_exceptions)

        self.assertIn(GLib.Variant('()', ()), self.get_all_async_replies())

    def test_concourrent_delete_enrolled_finger(self):
        self.enroll_image('whorl', finger='left-thumb')
        self.enroll_image('tented_arch', finger='right-thumb')
        self.call_device_method_async('DeleteEnrolledFinger', '(s)', ['left-thumb'])
        self.call_device_method_async('DeleteEnrolledFinger', '(s)', ['right-thumb'])

        accepted_exceptions = []
        if self.device_driver == 'virtual_device_storage':
            accepted_exceptions.append('AlreadyInUse')

        self.wait_for_device_reply_relaxed(expected_replies=2,
            accepted_exceptions=accepted_exceptions)

        if self.device_driver == 'virtual_device_storage':
            self.assertIn(GLib.Variant('()', ()), self.get_all_async_replies())
        else:
            self.assertEqual([GLib.Variant('()', ()), GLib.Variant('()', ())],
                self.get_all_async_replies())

    def test_concourrent_release(self):
        self.call_device_method_async('Release', '()', [])
        self.call_device_method_async('Release', '()', [])

        with self.assertFprintError(['AlreadyInUse', 'ClaimDevice']):
            self.wait_for_device_reply(expected_replies=2)

        self.assertIn(GLib.Variant('()', ()), self.get_all_async_replies())

    def test_already_claimed_same_user_delete_enrolled_fingers(self):
        self.enroll_image('whorl')
        self.device.DeleteEnrolledFingers('(s)', 'testuser')

    def test_already_claimed_same_user_delete_enrolled_fingers_no_prints(self):
        with self.assertFprintError('NoEnrolledPrints'):
            self.device.DeleteEnrolledFingers('(s)', 'testuser')

    def test_already_claimed_other_user_delete_enrolled_fingers(self):
        self.device.Release()
        self.enroll_image('whorl', claim_user='nottestuser')
        self.device.Claim('(s)', 'testuser')
        self.device.DeleteEnrolledFingers('(s)', 'nottestuser')

    def test_already_claimed_other_user_delete_enrolled_fingers_no_prints(self):
        with self.assertFprintError('NoEnrolledPrints'):
            self.device.DeleteEnrolledFingers('(s)', 'nottestuser')


class FPrintdVirtualDeviceEnrollTests(FPrintdVirtualDeviceBaseTest):

    def setUp(self):
        super().setUp()
        self._abort = False
        self.device.Claim('(s)', 'testuser')
        self.device.EnrollStart('(s)', 'left-middle-finger')
        self.stop_on_teardown = True

    def tearDown(self):
        if self.stop_on_teardown:
            self.device.EnrollStop()
        self.device.Release()
        super().tearDown()

    def assertEnrollRetry(self, device_error, expected_error):
        self.send_retry(retry_error=device_error)
        self.wait_for_result(expected=expected_error)

    def assertEnrollError(self, device_error, expected_error):
        self.send_error(error=device_error)
        self.wait_for_result(expected=expected_error)

    def test_enroll_retry_general(self):
        self.assertEnrollRetry(FPrint.DeviceRetry.GENERAL, 'enroll-retry-scan')

    def test_enroll_retry_too_short(self):
        self.assertEnrollRetry(FPrint.DeviceRetry.TOO_SHORT, 'enroll-swipe-too-short')

    def test_enroll_retry_remove_finger(self):
        self.assertEnrollRetry(FPrint.DeviceRetry.REMOVE_FINGER, 'enroll-remove-and-retry')

    def test_enroll_retry_center_finger(self):
        self.assertEnrollRetry(FPrint.DeviceRetry.CENTER_FINGER, 'enroll-finger-not-centered')

    def test_enroll_error_general(self):
        self.assertEnrollError(FPrint.DeviceError.GENERAL, 'enroll-unknown-error')

    def test_enroll_error_not_supported(self):
        self.assertEnrollError(FPrint.DeviceError.NOT_SUPPORTED, 'enroll-unknown-error')

    def test_enroll_error_not_open(self):
        self.assertEnrollError(FPrint.DeviceError.NOT_OPEN, 'enroll-unknown-error')

    def test_enroll_error_already_open(self):
        self.assertEnrollError(FPrint.DeviceError.ALREADY_OPEN, 'enroll-unknown-error')

    def test_enroll_error_busy(self):
        self.assertEnrollError(FPrint.DeviceError.BUSY, 'enroll-unknown-error')

    def test_enroll_error_proto(self):
        self.assertEnrollError(FPrint.DeviceError.PROTO, 'enroll-disconnected')

    def test_enroll_error_data_invalid(self):
        self.assertEnrollError(FPrint.DeviceError.DATA_INVALID, 'enroll-unknown-error')

    def test_enroll_error_data_not_found(self):
        if self.has_identification:
            self.assertEnrollError(
                FPrint.DeviceError.DATA_NOT_FOUND, 'enroll-stage-passed')
        self.assertEnrollError(FPrint.DeviceError.DATA_NOT_FOUND, 'enroll-unknown-error')

    def test_enroll_error_data_full(self):
        self.assertEnrollError(FPrint.DeviceError.DATA_FULL, 'enroll-data-full')

    def test_enroll_already_enrolled_finger(self):
        self.enroll_image('whorl', start=False)

        self.stop_on_teardown = False
        with self.assertFprintError('FingerAlreadyEnrolled'):
            self.device.EnrollStart('(s)', 'left-middle-finger')

    def test_enroll_duplicate_image(self):
        self.enroll_image('whorl', finger='left-thumb', start=False)
        self.enroll_image('whorl', finger='right-thumb', stop=False,
            expected_result='enroll-duplicate' if self.has_identification
                else 'enroll-completed')

    def test_enroll_start_during_enroll(self):
        with self.assertFprintError('AlreadyInUse'):
            self.device.EnrollStart('(s)', 'left-thumb')

    def test_verify_start_during_enroll(self):
        self.enroll_image('whorl', start=False)
        self.device.EnrollStart('(s)', 'right-thumb')
        with self.assertFprintError('AlreadyInUse'):
            self.device.VerifyStart('(s)', 'any')

    def test_verify_stop_during_enroll(self):
        with self.assertFprintError('AlreadyInUse'):
            self.device.VerifyStop()

    def test_enroll_stop_from_other_client(self):
        with self.assertFprintError('AlreadyInUse'):
            self.call_device_method_from_other_client('EnrollStop')

    def test_delete_fingers_during_enroll(self):
        with self.assertFprintError('AlreadyInUse'):
            self.device.DeleteEnrolledFingers('(s)', '')

    def test_delete_fingers2_during_enroll(self):
        with self.assertFprintError('AlreadyInUse'):
            self.device.DeleteEnrolledFingers2()

    def test_delete_finger_during_enroll(self):
        with self.assertFprintError('AlreadyInUse'):
            self.device.DeleteEnrolledFinger('(s)', 'left-thumb')

    def test_enroll_concourrent_stop(self):
        self.stop_on_teardown = False
        self.call_device_method_async('EnrollStop', '()', [])
        self.call_device_method_async('EnrollStop', '()', [])

        with self.assertFprintError(['AlreadyInUse', 'NoActionInProgress']):
            self.wait_for_device_reply(method='EnrollStop', expected_replies=2)

        self.assertIn(GLib.Variant('()', ()), self.get_all_async_replies())


class FPrintdVirtualDeviceNoStorageEnrollTests(FPrintdVirtualNoStorageDeviceBaseTest,
                                               FPrintdVirtualDeviceEnrollTests):
    # Repeat the tests for the Virtual device (with no storage)
    pass


class FPrintdVirtualDeviceStorageClaimedTest(FPrintdVirtualStorageDeviceBaseTest,
                                             FPrintdVirtualDeviceClaimedTest):
    # Repeat the tests for the Virtual storage device

    def test_release_waits_for_deletion(self):
        self.enroll_print('new-print')
        self.send_sleep(get_timeout('daemon_stop') * 1000 * 0.5)
        self.call_device_method_async('DeleteEnrolledFingers2', '()', [])
        self.wait_for_result(max_wait=100)
        self.call_device_method_async('Release', '()', [])
        with self.assertFprintError('Internal'):
            self.wait_for_device_reply(method='Release')

        self.assertFalse(self.get_async_replies(
            method='DeleteEnrolledFingers2'))

    def test_delete_enrolled_fingers_device_error(self):
        self.enroll_print('new-print')
        self.send_sleep(10)
        self.send_error(FPrint.DeviceError.BUSY)

        with self.assertFprintError('PrintsNotDeletedFromDevice'):
            self.device.DeleteEnrolledFingers('(s)', 'testuser')

    def test_delete_enrolled_fingers2_device_error(self):
        self.enroll_print('new-print')
        self.send_sleep(10)
        self.send_error(FPrint.DeviceError.BUSY)

        with self.assertFprintError('PrintsNotDeletedFromDevice'):
            self.device.DeleteEnrolledFingers2()

    def test_delete_enrolled_finger_device_error(self):
        self.enroll_print('new-print', finger='left-thumb')
        self.send_sleep(10)
        self.send_error(FPrint.DeviceError.BUSY)

        with self.assertFprintError('PrintsNotDeletedFromDevice'):
            self.device.DeleteEnrolledFinger('(s)', 'left-thumb')

    def test_delete_enrolled_fingers_device_removed(self):
        self.enroll_print('deleted-print')
        self.send_command('REMOVE', 'deleted-print')

        with self.assertFprintError('PrintsNotDeletedFromDevice'):
            self.device.DeleteEnrolledFingers('(s)', 'testuser')

    def test_delete_enrolled_fingers2_device_removed(self):
        self.enroll_print('deleted-print')
        self.send_command('REMOVE', 'deleted-print')

        with self.assertFprintError('PrintsNotDeletedFromDevice'):
            self.device.DeleteEnrolledFingers2()

    def test_delete_enrolled_finger_device_removed(self):
        self.enroll_print('deleted-print', finger='left-thumb')
        self.send_command('REMOVE', 'deleted-print')

        with self.assertFprintError('PrintsNotDeletedFromDevice'):
            self.device.DeleteEnrolledFinger('(s)', 'left-thumb')

    def test_delete_enrolled_fingers_storage_error_has_higher_priority(self):
        self.enroll_print('deleted-print', finger='left-thumb')
        self.send_sleep(get_timeout('device_sleep'))
        self.send_error(FPrint.DeviceError.BUSY)

        self.call_device_method_async('DeleteEnrolledFingers', '(s)', ['testuser'])
        self.wait_for_result(max_wait=get_timeout('device_sleep') / 2)
        self.assertFalse(self.get_all_async_replies())

        self.set_print_not_writable('testuser', FPrint.Finger.LEFT_THUMB)

        with self.assertFprintError('PrintsNotDeleted'):
            self.wait_for_device_reply()

    def test_delete_enrolled_fingers2_storage_error_has_higher_priority(self):
        self.enroll_print('deleted-print', finger='left-thumb')
        self.send_sleep(get_timeout('device_sleep'))
        self.send_error(FPrint.DeviceError.BUSY)

        self.call_device_method_async('DeleteEnrolledFingers2', '()', [])
        self.wait_for_result(max_wait=get_timeout('device_sleep') / 2)
        self.assertFalse(self.get_all_async_replies())

        self.set_print_not_writable('testuser', FPrint.Finger.LEFT_THUMB)

        with self.assertFprintError('PrintsNotDeleted'):
            self.wait_for_device_reply()

    def test_delete_enrolled_finger_storage_error_has_higher_priority(self):
        self.enroll_print('deleted-print', finger='left-thumb')
        self.send_sleep(get_timeout('device_sleep'))
        self.send_error(FPrint.DeviceError.BUSY)

        self.call_device_method_async('DeleteEnrolledFinger', '(s)', ['left-thumb'])
        self.wait_for_result(max_wait=get_timeout('device_sleep') / 2)
        self.assertFalse(self.get_all_async_replies())

        self.set_print_not_writable('testuser', FPrint.Finger.LEFT_THUMB)

        with self.assertFprintError('PrintsNotDeleted'):
            self.wait_for_device_reply()

    def test_release_error(self):
        self.send_error(FPrint.DeviceError.PROTO)

        with self.assertFprintError('Internal'):
            self.device.Release()

    def test_release_fails_while_closing(self):
        self.send_sleep(300)
        self.call_device_method_async('Release', '()', [])
        self.wait_for_result(max_wait=150)
        self.assertFalse(self.get_all_async_replies())

        with self.assertFprintError('AlreadyInUse'):
            self.device.Release()

        self.wait_for_device_reply()
        self.assertIn(GLib.Variant('()', ()), self.get_all_async_replies())

class FPrintdVirtualDeviceVerificationTests(FPrintdVirtualDeviceBaseTest):

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.enroll_finger = 'left-middle-finger'
        cls.verify_finger = cls.enroll_finger
        cls.stop_on_teardown = True
        cls.releases_on_teardown = True

    def setUp(self):
        super().setUp()
        self.device.Claim('(s)', 'testuser')
        self.enroll_image('whorl', finger=self.enroll_finger)
        self.device.VerifyStart('(s)', self.verify_finger)

    def tearDown(self):
        if self.stop_on_teardown:
            self.device.VerifyStop()
        if self.releases_on_teardown:
            self.device.Release()
        super().tearDown()

    def assertVerifyRetry(self, device_error, expected_error):
        self.send_retry(retry_error=device_error)
        self.wait_for_result()
        self.assertFalse(self._verify_stopped)
        self.assertEqual(self._last_result, expected_error)

    def assertVerifyError(self, device_error, expected_error):
        self.send_error(error=device_error)
        self.wait_for_result()
        self.assertTrue(self._verify_stopped)
        self.assertEqual(self._last_result, expected_error)

    def test_verify_retry_general(self):
        self.assertVerifyRetry(FPrint.DeviceRetry.GENERAL, 'verify-retry-scan')

    def test_verify_retry_general_restarted(self):
        self.assertVerifyRetry(FPrint.DeviceRetry.GENERAL, 'verify-retry-scan')
        # Give fprintd time to re-start the request. We can't force the other
        # case (cancellation before restart happened), but we can force this one.
        time.sleep(1)

    def test_verify_retry_too_short(self):
        self.assertVerifyRetry(FPrint.DeviceRetry.TOO_SHORT, 'verify-swipe-too-short')

    def test_verify_retry_remove_finger(self):
        self.assertVerifyRetry(FPrint.DeviceRetry.REMOVE_FINGER, 'verify-remove-and-retry')

    def test_verify_retry_center_finger(self):
        self.assertVerifyRetry(FPrint.DeviceRetry.CENTER_FINGER, 'verify-finger-not-centered')

    def test_verify_error_general(self):
        self.assertVerifyError(FPrint.DeviceError.GENERAL, 'verify-unknown-error')

    def test_verify_error_not_supported(self):
        self.assertVerifyError(FPrint.DeviceError.NOT_SUPPORTED, 'verify-unknown-error')

    def test_verify_error_not_open(self):
        self.assertVerifyError(FPrint.DeviceError.NOT_OPEN, 'verify-unknown-error')

    def test_verify_error_already_open(self):
        self.assertVerifyError(FPrint.DeviceError.ALREADY_OPEN, 'verify-unknown-error')

    def test_verify_error_busy(self):
        self.assertVerifyError(FPrint.DeviceError.BUSY, 'verify-unknown-error')

    def test_verify_error_proto(self):
        self.assertVerifyError(FPrint.DeviceError.PROTO, 'verify-disconnected')

    def test_verify_error_data_invalid(self):
        self.assertVerifyError(FPrint.DeviceError.DATA_INVALID, 'verify-unknown-error')

    def test_verify_error_data_not_found(self):
        self.assertVerifyError(FPrint.DeviceError.DATA_NOT_FOUND, 'verify-unknown-error')

    def test_verify_error_data_full(self):
        self.assertVerifyError(FPrint.DeviceError.DATA_FULL, 'verify-unknown-error')

    def test_multiple_verify(self):
        self.send_image('tented_arch')
        self.assertVerifyNoMatch()
        self.device.VerifyStop()

        self.device.VerifyStart('(s)', self.verify_finger)
        self.send_image('whorl')
        self.assertVerifyMatch()

    def start_verify_with_delayed_stop(self, image):
        with Connection(self.sockaddr) as con:
            self.send_finger_automatic(False, con=con)
            self.send_finger_report(True, con=con)
            self.send_image(image, con=con)

    def test_multiple_verify_cancelled(self):
        self.start_verify_with_delayed_stop('tented_arch')
        self.assertVerifyNoMatch()
        self.device.VerifyStop()

        self.device.VerifyStart('(s)', self.verify_finger)
        self.send_finger_report(False)
        self.send_image('whorl')
        self.assertVerifyMatch()

    def test_verify_start_during_verify(self):
        with self.assertFprintError('AlreadyInUse'):
            self.device.VerifyStart('(s)', self.verify_finger)

    def test_enroll_start_during_verify(self):
        with self.assertFprintError('AlreadyInUse'):
            self.device.EnrollStart('(s)', 'right-thumb')

    def test_enroll_stop_during_verify(self):
        with self.assertFprintError('AlreadyInUse'):
            self.device.EnrollStop()

    def test_verify_stop_from_other_client(self):
        with self.assertFprintError('AlreadyInUse'):
            self.call_device_method_from_other_client('VerifyStop')

    def test_delete_fingers_during_verify(self):
        with self.assertFprintError('AlreadyInUse'):
            self.device.DeleteEnrolledFingers('(s)', '')

    def test_delete_fingers2_during_verify(self):
        with self.assertFprintError('AlreadyInUse'):
            self.device.DeleteEnrolledFingers2()

    def test_delete_finger_during_verify(self):
        with self.assertFprintError('AlreadyInUse'):
            self.device.DeleteEnrolledFinger('(s)', 'left-thumb')

    def test_verify_concourrent_stop(self):
        self.stop_on_teardown = False
        self.call_device_method_async('VerifyStop', '()', [])
        self.call_device_method_async('VerifyStop', '()', [])

        with self.assertFprintError(['AlreadyInUse', 'NoActionInProgress']):
            self.wait_for_device_reply(method='VerifyStop', expected_replies=2)

        self.assertIn(GLib.Variant('()', ()), self.get_all_async_replies())

    def test_verify_error_ignored_after_report(self):
        if self.device_driver != 'virtual_image':
            self.skipTest('Relies on virtual_image driver specifics')

        with Connection(self.sockaddr) as con:
            self.send_finger_automatic(False, con=con)
            self.send_finger_report(True, con=con)
            self.send_image('whorl', con=con)

            self.assertVerifyMatch()
            self.assertTrue(self.finger_present)

            self.send_error(con=con)
            self.wait_for_result(max_wait=200)
            self.assertIsNone(self._last_result)
            self.assertFalse(self.finger_present)

    def test_verify_stop_restarts_immediately(self):
        self.send_image('tented_arch')
        self.assertVerifyNoMatch()

        self.call_device_method_async('VerifyStop', '()', [])
        self.call_device_method_async('VerifyStart', '(s)', [self.verify_finger])

        self.wait_for_device_reply(expected_replies=2)

    def test_verify_stop_waits_for_completion(self):
        self.stop_on_teardown = False

        self.start_verify_with_delayed_stop('tented_arch')
        self.assertVerifyNoMatch()

        self.call_device_method_async('VerifyStop', '()', [])

        def restart_verify(abort=False):
            self.call_device_method_async('VerifyStart', '(s)', [self.verify_finger])
            with self.assertFprintError('AlreadyInUse'):
                self.wait_for_device_reply(method='VerifyStart')

            self.assertFalse(self.get_async_replies(method='VerifyStop'))
            self._abort = abort

        restart_verify()
        GLib.timeout_add(100, restart_verify)
        GLib.timeout_add(300, restart_verify, True)
        self.wait_for_result()

    def test_verify_stop_waits_for_completion_waiting_timeout(self):
        self.test_verify_stop_waits_for_completion()
        self.wait_for_device_reply(method='VerifyStop')
        self.assertTrue(self.get_async_replies(method='VerifyStop'))

    def test_verify_stop_waits_for_completion_is_stopped_by_release(self):
        # During the release here we're testing the case in which
        # while we're waiting for VerifyStop to return, Release stops the
        # verification, making the invocation to return
        self.releases_on_teardown = False
        self.test_verify_stop_waits_for_completion()
        self.assertFalse(self.get_async_replies(method='VerifyStop'))
        self.call_device_method_async('Release', '()', [])
        self.wait_for_device_reply(method='Release')
        self.assertTrue(self.get_async_replies(method='VerifyStop'))


class FPrintdVirtualDeviceStorageVerificationUtils(object):
    def start_verify_with_delayed_stop(self, image, match=None):
        self.send_sleep(50)
        self.send_image(image)
        self.send_sleep(get_timeout('test') * 1000)

    def test_verify_error_ignored_after_report(self):
        self.send_sleep(50)
        self.send_image('whorl')
        self.send_sleep(0)
        self.send_error(FPrint.DeviceError.BUSY)

        self.assertVerifyMatch()

        self.wait_for_result(max_wait=200)
        self.assertIsNone(self._last_result)
        self.assertFalse(self.finger_present)

class FPrintdVirtualDeviceStorageVerificationTests(FPrintdVirtualStorageDeviceBaseTest,
                                                   FPrintdVirtualDeviceStorageVerificationUtils,
                                                   FPrintdVirtualDeviceVerificationTests):
    # Repeat the tests for the Virtual storage device, with specific overrides
    pass

class FPrintdVirtualDeviceNoStorageVerificationTests(FPrintdVirtualNoStorageDeviceBaseTest,
                                                     FPrintdVirtualDeviceStorageVerificationTests):
    # Repeat the tests for the Virtual device (with no storage)
    pass

class FPrintdVirtualDeviceIdentificationTests(FPrintdVirtualDeviceVerificationTests):
    '''This class will just repeat the tests of FPrintdVirtualDeviceVerificationTests
    but with 'any' finger parameter (leading to an identification, when possible
    under the hood).
    '''

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.verify_finger = 'any'


class FPrintdVirtualDeviceStorageIdentificationTests(FPrintdVirtualStorageDeviceBaseTest,
                                                     FPrintdVirtualDeviceStorageVerificationUtils,
                                                     FPrintdVirtualDeviceIdentificationTests):
    # Repeat the tests for the Virtual storage device
    pass

class FPrintdVirtualDeviceNoStorageIdentificationTests(FPrintdVirtualNoStorageDeviceBaseTest,
                                                       FPrintdVirtualDeviceStorageIdentificationTests):
    # Repeat the tests for the Virtual device (with no storage)
    pass


class FPrindConcurrentPolkitRequestsTest(FPrintdVirtualStorageDeviceBaseTest):

    def wait_for_hanging_clients(self):
        while not self._polkitd_obj.HaveHangingCalls():
            pass
        self.assertTrue(self._polkitd_obj.HaveHangingCalls())

    def start_hanging_gdbus_claim(self, user='testuser'):
        gdbus = self.gdbus_device_method_call_process('Claim', [user])
        self.assertIsNone(gdbus.poll())
        self.wait_for_hanging_clients()
        self.addCleanup(gdbus.kill)
        return gdbus

    def test_hanging_claim_does_not_block_new_claim_external_client(self):
        self._polkitd_obj.SetAllowed([
            FprintDevicePermission.set_username,
            FprintDevicePermission.enroll ])
        self._polkitd_obj.SimulateHang(True)
        self._polkitd_obj.SetDelay(0.5)

        gdbus = self.start_hanging_gdbus_claim()

        self._polkitd_obj.SimulateHang(False)
        self.device.Claim('(s)', self.get_current_user())

        self.assertIsNone(gdbus.poll())
        self._polkitd_obj.ReleaseHangingCalls()

        gdbus.wait()
        with self.assertFprintError('AlreadyInUse'):
            raise GLib.GError(gdbus.stdout.read())

        self.device.Release()

    def test_hanging_claim_does_not_block_new_claim(self):
        self._polkitd_obj.SetAllowed([
            FprintDevicePermission.set_username,
            FprintDevicePermission.enroll ])
        self._polkitd_obj.SimulateHang(True)
        self._polkitd_obj.SetDelay(0.5)

        self.call_device_method_async('Claim', '(s)', [''])
        self.wait_for_hanging_clients()

        self._polkitd_obj.SimulateHang(False)
        self.device.Claim('(s)', self.get_current_user())

        self._polkitd_obj.ReleaseHangingCalls()

        with self.assertFprintError('AlreadyInUse'):
            self.wait_for_device_reply()

        self.device.Release()

    def test_hanging_claim_enroll_does_not_block_new_claim(self):
        self._polkitd_obj.SetAllowed([
            FprintDevicePermission.set_username,
            FprintDevicePermission.enroll ])
        self._polkitd_obj.SimulateHangActions([
            FprintDevicePermission.enroll])
        self._polkitd_obj.SetDelay(0.5)

        gdbus = self.start_hanging_gdbus_claim()

        self._polkitd_obj.SimulateHangActions([''])
        self.device.Claim('(s)', self.get_current_user())

        self.assertIsNone(gdbus.poll())
        self._polkitd_obj.ReleaseHangingCalls()

        gdbus.wait()
        with self.assertFprintError('AlreadyInUse'):
            raise GLib.GError(gdbus.stdout.read())

        self.device.Release()

    def test_hanging_claim_does_not_block_new_release(self):
        self._polkitd_obj.SetAllowed([FprintDevicePermission.set_username])
        self._polkitd_obj.SimulateHang(True)

        gdbus = self.gdbus_device_method_call_process('Claim', ['testuser'])
        self.addCleanup(gdbus.kill)

        self.wait_for_hanging_clients()
        with self.assertFprintError('ClaimDevice'):
            self.device.Release()

        self.assertIsNone(gdbus.poll())

    def test_hanging_claim_does_not_block_list(self):
        self._polkitd_obj.SetAllowed([
            FprintDevicePermission.set_username,
            FprintDevicePermission.enroll,
            FprintDevicePermission.verify])

        self.device.Claim('(s)', '')
        self.enroll_image('whorl', finger='left-thumb')
        self.device.Release()

        self._polkitd_obj.SimulateHangActions([
            FprintDevicePermission.set_username])

        gdbus = self.start_hanging_gdbus_claim()

        self.assertEqual(self.device.ListEnrolledFingers('(s)',
            self.get_current_user()), ['left-thumb'])

        self.assertIsNone(gdbus.poll())

    def test_hanging_claim_can_proceed_when_released(self):
        self._polkitd_obj.SetAllowed([
            FprintDevicePermission.set_username,
            FprintDevicePermission.verify])

        self._polkitd_obj.SimulateHangActions([
            FprintDevicePermission.set_username])

        gdbus = self.start_hanging_gdbus_claim()

        self._polkitd_obj.SimulateHangActions([''])
        self.device.Claim('(s)', 'testuser')
        self.device.Release()

        self.assertIsNone(gdbus.poll())

        self._polkitd_obj.ReleaseHangingCalls()
        gdbus.wait()

        self.assertEqual(gdbus.returncode, 0)

    def test_hanging_claim_does_not_block_empty_list(self):
        self._polkitd_obj.SetAllowed([
            FprintDevicePermission.set_username,
            FprintDevicePermission.enroll,
            FprintDevicePermission.verify])

        self._polkitd_obj.SimulateHangActions([
            FprintDevicePermission.set_username])

        gdbus = self.start_hanging_gdbus_claim()

        with self.assertFprintError('NoEnrolledPrints'):
            self.device.ListEnrolledFingers('(s)', self.get_current_user())

        self.assertIsNone(gdbus.poll())

    def test_hanging_claim_does_not_block_verification(self):
        self._polkitd_obj.SetAllowed([
            FprintDevicePermission.set_username,
            FprintDevicePermission.enroll,
            FprintDevicePermission.verify])

        self.device.Claim('(s)', '')
        self.enroll_image('whorl', finger='left-thumb')
        self.device.Release()

        self._polkitd_obj.SimulateHangActions([
            FprintDevicePermission.set_username])

        gdbus = self.start_hanging_gdbus_claim()

        self.device.Claim('(s)', '')
        self.device.VerifyStart('(s)', 'any')
        self.send_image('whorl')
        self.assertVerifyMatch(selected_finger='any')
        self.device.VerifyStop()
        self.device.Release()

        self.assertIsNone(gdbus.poll())


class FPrintdUtilsTest(FPrintdVirtualStorageDeviceBaseTest):

    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        utils = {
            'delete': None,
            'enroll': None,
            'list': None,
            'verify': None,
        }

        for util in utils:
            util_bin = 'fprintd-{}'.format(util)
            if 'FPRINT_BUILD_DIR' in os.environ:
                print('Testing local build')
                build_dir = os.environ['FPRINT_BUILD_DIR']
                path = os.path.join(build_dir, '../utils', util_bin)
            elif 'UNDER_JHBUILD' in os.environ:
                print('Testing JHBuild version')
                jhbuild_prefix = os.environ['JHBUILD_PREFIX']
                path = os.path.join(jhbuild_prefix, 'bin', util_bin)
            else:
                # Assume it is in path
                utils[util] = util_bin
                continue

            assert os.path.exists(path), 'failed to find {} in {}'.format(util, path)
            utils[util] = path

        cls.utils = utils
        cls.utils_proc = {}

    def util_start(self, name, args=[]):
        env = os.environ.copy()
        env['G_DEBUG'] = 'fatal-criticals'
        env['STATE_DIRECTORY'] = self.state_dir
        env['RUNTIME_DIRECTORY'] = self.run_dir

        argv = [self.utils[name]] + args
        valgrind = os.getenv('VALGRIND')
        if valgrind is not None:
            argv.insert(0, 'valgrind')
            argv.insert(1, '--leak-check=full')
            if os.path.exists(valgrind):
                argv.insert(2, '--suppressions=%s' % valgrind)
            self.valgrind = True
        output = OutputChecker()
        self.utils_proc[name] = subprocess.Popen(argv,
                                                 env=env,
                                                 stdout=output.fd,
                                                 stderr=subprocess.STDOUT)
        output.writer_attached()
        self.addCleanup(self.utils_proc[name].wait)
        self.addCleanup(self.utils_proc[name].terminate)
        self.addCleanup(output.assert_closed)
        return self.utils_proc[name], output

    def test_vanished_client_operation_is_cancelled(self):
        self.device.Claim('(s)', self.get_current_user())
        self.enroll_image('whorl')
        self.device.Release()

        verify, output = self.util_start('verify')
        time.sleep(1)
        verify.terminate()
        self.assertLess(verify.wait(), 128)
        time.sleep(1)

        self.device.Claim('(s)', self.get_current_user())
        self.device.Release()

    def test_delete_no_prints(self):
        delete, out = self.util_start('delete', [self.get_current_user()])
        out.check_line('No fingerprints to delete on {}'.format(
            self.driver_name).encode('utf-8'), get_timeout())
        self.assertEqual(delete.wait(), 0)

    def test_delete_already_claimed(self):
        self.device.Claim('(s)', self.get_current_user())
        self.addCleanup(self.try_release)
        self.enroll_image('whorl')

        delete, out = self.util_start('delete', [self.get_current_user()])
        out.check_line('{}.Error.AlreadyInUse'.format(FPRINT_NAMESPACE), get_timeout())
        self.assertNotEqual(delete.wait(), 0)
        self.assertLess(delete.wait(), 128)

    def test_delete_error_claiming(self):
        self.device.Claim('(s)', self.get_current_user())
        self.addCleanup(self.try_release)
        self.set_keep_alive(True)
        self.device.Release()

        self.send_error(FPrint.DeviceError.PROTO)
        delete, out = self.util_start('delete', [self.get_current_user()])
        out.check_line('{}.Error.Internal'.format(FPRINT_NAMESPACE), get_timeout())
        self.assertNotEqual(delete.wait(), 0)
        self.assertLess(delete.wait(), 128)

    def test_delete_error(self):
        self.device.Claim('(s)', self.get_current_user())
        self.addCleanup(self.try_release)
        self.enroll_image('whorl')
        self.set_keep_alive(True)
        self.device.Release()

        self.send_command('IGNORED_COMMAND') # During claim
        self.send_error(FPrint.DeviceError.PROTO)  # During delete

        delete, out = self.util_start('delete', [self.get_current_user()])
        out.check_line('Failed to delete fingerprints', get_timeout())
        self.assertNotEqual(delete.wait(), 0)
        self.assertLess(delete.wait(), 128)

    def test_delete_release_error(self):
        self.device.Claim('(s)', self.get_current_user())
        self.addCleanup(self.try_release)
        self.set_keep_alive(True)
        self.device.Release()

        self.send_command('IGNORED_COMMAND')  # During claim
        self.send_error(FPrint.DeviceError.PROTO)  # During release

        delete, out = self.util_start('delete', [self.get_current_user()])
        out.check_line('Release failed with error', get_timeout())
        self.assertNotEqual(delete.wait(), 0)
        self.assertLess(delete.wait(), 128)

    def test_delete_single_finger(self):
        self.device.Claim('(s)', 'testuser')
        enrolled, enroll_map = self.enroll_multiple_images()
        self.addCleanup(self.try_release)
        self.device.Release()

        finger_name = enrolled[0]
        delete, out = self.util_start('delete', ['testuser',
            '-f', finger_name])

        out.check_line('Using device {}'.format(
            self.device.get_object_path()), get_timeout())

        out.check_line('Fingerprint {} of user {} deleted on {}'.format(
            finger_name, 'testuser', self.driver_name), get_timeout())
        self.assertEqual(delete.wait(), 0)

        remaining = self.device.ListEnrolledFingers('(s)', 'testuser')
        self.assertNotIn(finger_name, remaining)
        self.assertCountEqual(enrolled[1:], remaining)

    def test_delete_multiple_users_single_finger(self):
        self.addCleanup(self.try_release)
        enroll_map, enrolled_prints_info = self.enroll_users_images()
        delete_args = []
        for user, print_info in enroll_map.items():
            for f in print_info:
                delete_args.append(user)
                delete_args.append('-f')
                delete_args.append(f)

        delete, out = self.util_start('delete', delete_args)
        out.check_line('Using device {}'.format(
            self.device.get_object_path()), get_timeout())

        for user, print_info in enroll_map.items():
            for f in print_info:
                out.check_line('Fingerprint {} of user {} deleted on {}'.format(
                    f, user, self.driver_name), get_timeout())

        self.assertEqual(delete.wait(), 0)
        with self.assertFprintError('NoEnrolledPrints'):
            self.device.ListEnrolledFingers('(s)', 'testuser')

    def test_enroll(self):
        self.device.Claim('(s)', self.get_current_user())
        self.set_keep_alive(True)
        self.device.Release()

        # Open (no clear storage as list is supported)
        self.send_command('CONT')

        finger_name = self.get_finger_name(FPrint.Finger.LEFT_THUMB)
        enroll, out = self.util_start('enroll', [self.get_current_user(),
            '-f', finger_name])
        out.check_line('Using device {}'.format(
            self.device.get_object_path()), get_timeout())

        out.check_line('Enrolling {} finger.'.format(finger_name).encode('utf-8'),
            get_timeout())

        self.send_image('print-id')
        out.check_line('Enroll result: enroll-stage-passed', get_timeout())

        self.send_image('print-id')
        out.check_line('Enroll result: enroll-stage-passed', get_timeout())

        self.send_retry(FPrint.DeviceRetry.TOO_SHORT)
        out.check_line('Enroll result: enroll-swipe-too-short', get_timeout())

        self.send_image('print-id')
        out.check_line('Enroll result: enroll-stage-passed', get_timeout())

        self.send_image('print-id')
        out.check_line('Enroll result: enroll-stage-passed', get_timeout())

        self.send_image('print-id')
        out.check_line('Enroll result: enroll-stage-passed', get_timeout())

        self.send_retry(FPrint.DeviceRetry.CENTER_FINGER)
        out.check_line('Enroll result: enroll-finger-not-centered', get_timeout())

        self.send_image('print-id')
        out.check_line('Enroll result: enroll-completed', get_timeout())

        self.assertEqual(enroll.wait(), 0)

    def test_enroll_error(self):
        self.device.Claim('(s)', self.get_current_user())
        self.set_keep_alive(True)
        self.device.Release()

        # Open (no clear storage as list is supported)
        self.send_command('CONT')

        finger_name = self.get_finger_name(FPrint.Finger.LEFT_MIDDLE)
        enroll, out = self.util_start('enroll', [self.get_current_user(),
            '-f', finger_name])
        out.check_line('Using device {}'.format(
            self.device.get_object_path()), get_timeout())

        out.check_line('Enrolling {} finger.'.format(finger_name).encode('utf-8'),
            get_timeout())

        self.send_image('print-id')
        out.check_line('Enroll result: enroll-stage-passed', get_timeout())

        self.send_image('print-id')
        out.check_line('Enroll result: enroll-stage-passed', get_timeout())

        self.send_retry(FPrint.DeviceRetry.TOO_SHORT)
        out.check_line('Enroll result: enroll-swipe-too-short', get_timeout())

        self.send_error(FPrint.DeviceError.PROTO)
        out.check_line('Enroll result: enroll-disconnected', get_timeout())

        self.assertNotEqual(enroll.wait(), 0)
        self.assertLess(enroll.wait(), 128)

    def test_enroll_error_invalid_finger(self):
        finger_name = 'eleventh-hand-finger'
        enroll, out = self.util_start('enroll', [self.get_current_user(),
            '-f', finger_name])

        out.check_line('Invalid finger name \'{}\''.format(finger_name), get_timeout())

        self.assertNotEqual(enroll.wait(), 0)
        self.assertLess(enroll.wait(), 128)

    def run_verify(self, finger, match, error=None):
        self.device.Claim('(s)', 'testuser')
        finger_name = self.get_finger_name(finger)
        override = {} if finger is FPrint.Finger.UNKNOWN else {
            finger_name: 'print-id',
        }
        enrolled, enroll_map = self.enroll_multiple_images(
            images_override=override)
        self.set_keep_alive(True)
        self.device.Release()

        verify, out = self.util_start(
            'verify', ['-f', finger_name, 'testuser'])
        out.check_line('Using device {}'.format(
            self.device.get_object_path()), get_timeout())
        out.check_line('Verify started!', get_timeout())
        out.check_line('Verifying: {}'.format(finger_name), get_timeout())

        if error:
            self.send_error(error)
            self.assertNotEqual(verify.wait(), 0)
            self.assertLess(verify.wait(), 128)
            out.check_line('Verify result: verify-disconnected (done)', get_timeout())
        elif match:
            verify_finger = enrolled[0] if finger is FPrint.Finger.UNKNOWN else finger_name
            self.send_image(enroll_map[verify_finger])
            out.check_line('Verify result: verify-match (done)', get_timeout())
            self.assertEqual(verify.wait(), 0)
        else:
            if finger is FPrint.Finger.UNKNOWN:
                verify_image = 'another-print'
            else:
                enroll_map.pop(finger_name)
                verify_image = list(enroll_map.values())[0]
            self.send_image(verify_image)
            out.check_line('Verify result: verify-no-match (done)', get_timeout())
            self.assertNotEqual(verify.wait(), 0)
            self.assertLess(verify.wait(), 128)

    def test_verify_match(self):
        self.run_verify(finger=FPrint.Finger.RIGHT_THUMB, match=True)

    def test_verify_no_match(self):
        self.run_verify(finger=FPrint.Finger.LEFT_MIDDLE, match=False)

    def test_verify_error(self):
        self.run_verify(finger=FPrint.Finger.RIGHT_THUMB, match=False,
            error=FPrint.DeviceError.PROTO)

    def test_verify_any_finger_match(self):
        self.run_verify(finger=FPrint.Finger.UNKNOWN, match=True)

    def test_verify_any_finger_no_match(self):
        self.run_verify(finger=FPrint.Finger.UNKNOWN, match=False)

    def test_verify_any_finger_error(self):
        self.run_verify(finger=FPrint.Finger.UNKNOWN, match=False,
            error=FPrint.DeviceError.PROTO)


def list_tests():
    import unittest_inspector
    return unittest_inspector.list_tests(sys.modules[__name__])

if __name__ == '__main__':
    if len(sys.argv) == 2 and sys.argv[1] == "list-tests":
        for machine, human in list_tests():
            print("%s %s" % (machine, human), end="\n")
        sys.exit(0)

    prog = unittest.main(verbosity=2, exit=False)
    if prog.result.errors or prog.result.failures:
        sys.exit(1)

    # Translate to skip error
    if prog.result.testsRun == len(prog.result.skipped):
        sys.exit(77)

    sys.exit(0)
