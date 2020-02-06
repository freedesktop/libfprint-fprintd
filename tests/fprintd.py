#! /usr/bin/env python3
# Copyright © 2017, 2019 Red Hat, Inc
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
import shutil
import socket
import struct
import dbusmock
import gi
from gi.repository import GLib, Gio
import cairo

try:
    from subprocess import DEVNULL
except ImportError:
    DEVNULL = open(os.devnull, 'wb')

SERVICE_FILE = '/usr/share/dbus-1/system-services/net.reactivated.Fprint.service'

def get_timeout(topic='default'):
    vals = {
        'valgrind': {
            'test': 300,
            'default': 20,
            'daemon_start': 60
        },
        'default': {
            'test': 60,
            'default': 3,
            'daemon_start': 5
        }
    }

    valgrind = os.getenv('VALGRIND')
    lut = vals['valgrind' if valgrind is not None else 'default']
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
        fprintd = None

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
        os.environ['FP_VIRTUAL_IMAGE'] = cls.sockaddr

        cls.prints = {}
        for f in glob.glob(os.path.join(imgdir, '*.png')):
            n = os.path.basename(f)[:-4]
            cls.prints[n] = load_image(f)


        cls.test_bus = Gio.TestDBus.new(Gio.TestDBusFlags.NONE)
        cls.test_bus.up()
        try:
            del os.environ['DBUS_SESSION_BUS_ADDRESS']
        except KeyError:
            pass
        os.environ['DBUS_SYSTEM_BUS_ADDRESS'] = cls.test_bus.get_bus_address()
        cls.dbus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)

    @classmethod
    def tearDownClass(cls):
        cls.test_bus.down()
        shutil.rmtree(cls.tmpdir)
        dbusmock.DBusTestCase.tearDownClass()


    def daemon_start(self):
        timeout = get_timeout('daemon_start')  # seconds
        env = os.environ.copy()
        env['G_DEBUG'] = 'fatal-criticals'
        env['STATE_DIRECTORY'] = self.state_dir
        env['RUNTIME_DIRECTORY'] = self.run_dir

        argv = [self.paths['daemon'], '-t']
        valgrind = os.getenv('VALGRIND')
        if valgrind is not None:
            argv.insert(0, 'valgrind')
            argv.insert(1, '--leak-check=full')
            if os.path.exists(valgrind):
                argv.insert(2, '--suppressions=%s' % valgrind)
            self.valgrind = True
        self.daemon = subprocess.Popen(argv,
                                       env=env,
                                       stdout=None,
                                       stderr=subprocess.STDOUT)
        self.device = None

        timeout_count = timeout * 10
        timeout_sleep = 0.1
        while timeout_count > 0:
            time.sleep(timeout_sleep)
            timeout_count -= 1
            try:
                self.manager = Gio.DBusProxy.new_sync(self.dbus,
                                                      Gio.DBusProxyFlags.DO_NOT_AUTO_START,
                                                      None,
                                                      'net.reactivated.Fprint',
                                                      '/net/reactivated/Fprint/Manager',
                                                      'net.reactivated.Fprint.Manager',
                                                      None)

                devices = self.manager.GetDevices()
                # Find the virtual device, just in case it is a local run
                # and there is another usable sensor available locally
                for path in devices:
                    dev = Gio.DBusProxy.new_sync(self.dbus,
                                                 Gio.DBusProxyFlags.DO_NOT_AUTO_START,
                                                 None,
                                                 'net.reactivated.Fprint',
                                                 path,
                                                 'net.reactivated.Fprint.Device',
                                                 None)

                    if 'Virtual image device' in str(dev.get_cached_property('name')):
                        self.device = dev
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
            self.daemon.wait(timeout=2)

        self.daemon = None
        self.client = None

    def polkitd_start(self):
        self._polkitd, self._polkitd_obj = self.spawn_server_template(
            'polkitd', {}, stdout=DEVNULL)

    def polkitd_stop(self):
        if self._polkitd is None:
            return
        self._polkitd.terminate()
        self._polkitd.wait()



    def setUp(self):
        self.test_dir = tempfile.mkdtemp()
        self.state_dir = os.path.join(self.test_dir, 'state')
        self.run_dir = os.path.join(self.test_dir, 'run')

    def tearDown(self):
        shutil.rmtree(self.test_dir)

    # From libfprint tests
    def send_retry(self, retry_error=1):
        # The default (1) is too-short
        with Connection(self.sockaddr) as con:
            con.sendall(struct.pack('ii', -1, retry_error))

    # From libfprint tests
    def send_image(self, image):
        img = self.prints[image]
        with Connection(self.sockaddr) as con:
            mem = img.get_data()
            mem = mem.tobytes()
            self.assertEqual(len(mem), img.get_width() * img.get_height())

            encoded_img = struct.pack('ii', img.get_width(), img.get_height())
            encoded_img += mem

            con.sendall(encoded_img)


class FPrintdVirtualDeviceTest(FPrintdTest):
    def setUp(self):
        super().setUp()

        self.polkitd_start()
        self.daemon_start()

        if self.device is None:
            self.daemon_stop()
            self.polkitd_stop()
            self.skipTest("Need virtual_image device to run the test")

        def timeout_cb(*args):
            # Note: With meson we could just rely on it to kill us
            print("Test timed out, hard exiting")
            sys.exit(1)

        self.test_timeout = GLib.timeout_add(get_timeout('test') * 1000, timeout_cb)

        self._polkitd_obj.SetAllowed(['net.reactivated.fprint.device.setusername',
                                      'net.reactivated.fprint.device.enroll',
                                      'net.reactivated.fprint.device.verify'])

        def signal_cb(proxy, sender, signal, params):
            print(signal, params)
            if signal == 'EnrollStatus':
                self._abort = params[1]
                self._last_result = params[0]

                if not self._abort and self._last_result == 'enroll-stage-passed':
                    self.send_image('whorl')
                elif self._abort:
                    pass
                else:
                    self._abort = True
                    self._last_result = 'Unexpected signal values'
                    print('Unexpected signal values')
            elif signal == 'VerifyFingerSelected':
                pass
            elif signal == 'VerifyStatus':
                self._abort = True
                self._last_result = params[0]
                self._verify_stopped = params[1]
            else:
                self._abort = True
                self._last_result = 'Unexpected signal'

        self.g_signal_id = self.device.connect('g-signal', signal_cb)

    def tearDown(self):
        super().tearDown()

        GLib.source_remove(self.test_timeout)
        self.device.disconnect(self.g_signal_id)

        self.daemon_stop()
        self.polkitd_stop()

    def assertFprintError(self, fprint_error):
        return self.assertRaisesRegex(GLib.Error,
            '.*net\.reactivated\.Fprint\.Error\.{}.*'.format(fprint_error))

    def wait_for_result(self, expected=None):
        self._abort = False
        while not self._abort:
            ctx.iteration(True)

        self.assertTrue(self._abort)
        self._abort = False

        if expected is not None:
            self.assertEqual(self._last_result, expected)

    def enroll_image(self, img, finger='right-index-finger'):
        self.device.EnrollStart('(s)', finger)

        self.send_image(img)
        self.wait_for_result()

        self.device.EnrollStop()
        self.assertEqual(self._last_result, 'enroll-completed')

    def test_allowed_claim(self):
        self._polkitd_obj.SetAllowed(['net.reactivated.fprint.device.setusername',
                                      'net.reactivated.fprint.device.enroll'])
        self.device.Claim('(s)', 'testuser')
        self.device.Release()

    def test_unallowed_claim(self):
        self._polkitd_obj.SetAllowed([''])

        with self.assertFprintError('PermissionDenied'):
            self.device.Claim('(s)', 'testuser')

        self._polkitd_obj.SetAllowed(['net.reactivated.fprint.device.setusername'])

        with self.assertFprintError('PermissionDenied'):
            self.device.Claim('(s)', 'testuser')

        self._polkitd_obj.SetAllowed(['net.reactivated.fprint.device.enroll'])

        with self.assertFprintError('PermissionDenied'):
            self.device.Claim('(s)', 'testuser')

    def test_multiple_claims(self):
        self.device.Claim('(s)', 'testuser')

        with self.assertFprintError('AlreadyInUse'):
            self.device.Claim('(s)', 'testuser')

        self.device.Release()

    def test_unallowed_release(self):
        self.device.Claim('(s)', 'testuser')

        self._polkitd_obj.SetAllowed([''])

        with self.assertFprintError('PermissionDenied'):
            self.device.Release()

        self._polkitd_obj.SetAllowed(['net.reactivated.fprint.device.setusername'])

        with self.assertFprintError('PermissionDenied'):
            self.device.Release()

        self._polkitd_obj.SetAllowed(['net.reactivated.fprint.device.enroll'])
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

    def test_wrong_finger_enroll_start(self):
        self.device.Claim('(s)', 'testuser')

        with self.assertFprintError('InvalidFingername'):
            self.device.EnrollStart('(s)', 'any')

        self.device.Release()

    def test_unclaimed_delete_enrolled_fingers(self):
        self.device.DeleteEnrolledFingers('(s)', 'testuser')

    def test_unclaimed_delete_enrolled_fingers2(self):
        with self.assertFprintError('ClaimDevice'):
            self.device.DeleteEnrolledFingers2()

    def test_unclaimed_list_enrolled_fingers(self):
        with self.assertFprintError('NoEnrolledPrints'):
            self.device.ListEnrolledFingers('(s)', 'testuser')

    def test_enroll_verify_list_delete(self):

        self.device.Claim('(s)', 'testuser')

        with self.assertFprintError('NoEnrolledPrints'):
            self.device.ListEnrolledFingers('(s)', 'testuser')

        with self.assertFprintError('NoEnrolledPrints'):
            self.device.ListEnrolledFingers('(s)', 'nottestuser')

        self.enroll_image('whorl')

        self.assertTrue(os.path.exists(os.path.join(self.state_dir, 'testuser/virtual_image/0/7')))

        with self.assertFprintError('NoEnrolledPrints'):
            self.device.ListEnrolledFingers('(s)', 'nottestuser')

        self.assertEqual(self.device.ListEnrolledFingers('(s)', 'testuser'), ['right-index-finger'])

        # Finger is enrolled, try to verify it
        self.device.VerifyStart('(s)', 'any')

        # Try a wrong print; will stop verification
        self.send_image('tented_arch')
        self.wait_for_result()
        self.assertTrue(self._verify_stopped)
        self.assertEqual(self._last_result, 'verify-no-match')

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

        self.assertEqual(self.device.ListEnrolledFingers('(s)', 'testuser'), ['right-index-finger'])

        # And delete the print(s) again
        self.device.DeleteEnrolledFingers('(s)', 'testuser')

        self.assertFalse(os.path.exists(os.path.join(self.state_dir, 'testuser/virtual_image/0/7')))

        with self.assertFprintError('NoEnrolledPrints'):
            self.device.ListEnrolledFingers('(s)', 'testuser')

        self.device.Release()

    def test_enroll_delete2(self):

        self.device.Claim('(s)', 'testuser')

        self.enroll_image('whorl')

        self.assertTrue(os.path.exists(os.path.join(self.state_dir, 'testuser/virtual_image/0/7')))

        # And delete the print(s) again using the new API
        self.device.DeleteEnrolledFingers2()

        self.assertFalse(os.path.exists(os.path.join(self.state_dir, 'testuser/virtual_image/0/7')))

        self.device.Release()

if __name__ == '__main__':
    if len(sys.argv) == 2 and sys.argv[1] == "list-tests":
        for machine, human in list_tests():
            print("%s %s" % (machine, human), end="\n")
        sys.exit(0)

    unittest.main(verbosity=2)
