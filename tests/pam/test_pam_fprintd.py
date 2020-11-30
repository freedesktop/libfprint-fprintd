#!/usr/bin/python3

# This program is free software; you can redistribute it and/or modify it under
# the terms of the GNU Lesser General Public License as published by the Free
# Software Foundation; either version 3 of the License, or (at your option) any
# later version.  See http://www.gnu.org/copyleft/lgpl.html for the full text
# of the license.

__author__ = 'Bastien Nocera'
__email__ = 'hadess@hadess.net'
__copyright__ = '(c) 2020 Red Hat Inc.'
__license__ = 'LGPL 3+'

import tempfile
import unittest
import sys
import subprocess
import dbus
import fcntl
import glob
import os
import shutil
import time
try:
    import dbusmock
    import pypamtest
except Exception as e:
    print("Missing dependencies: %s" % str(e))
    sys.exit(77)

PAM_SUCCESS = 0
PAM_AUTH_ERR = 7
PAM_AUTHINFO_UNAVAIL = 9
PAM_USER_UNKNOWN = 10
PAM_MAXTRIES = 11

class TestPamFprintd(dbusmock.DBusTestCase):
    '''Test pam_fprintd'''

    @classmethod
    def start_monitor(klass):
        '''Start dbus-monitor'''

        workdir = os.environ['TOPBUILDDIR'] + '/tests/pam/'
        klass.monitor_log = open(os.path.join(workdir, 'dbus-monitor.log'), 'wb', buffering=0)
        klass.monitor = subprocess.Popen(['dbus-monitor', '--monitor', '--system'],
                                         stdout=klass.monitor_log,
                                         stderr=subprocess.STDOUT)

    @classmethod
    def stop_monitor(klass):
        '''Stop dbus-monitor'''

        assert klass.monitor
        klass.monitor.terminate()
        klass.monitor.wait()

        klass.monitor_log.flush()
        klass.monitor_log.close()

    @classmethod
    def setUpClass(klass):
        klass.start_system_bus()
        klass.start_monitor()
        klass.dbus_con = klass.get_dbus(True)

        template_path = './'
        if 'TOPSRCDIR' in os.environ:
            template_path = os.environ['TOPSRCDIR'] + '/tests/'
        klass.template_name = template_path + 'dbusmock/fprintd.py'
        print ('Using template from %s' % klass.template_name)

    @classmethod
    def tearDownClass(klass):
        klass.stop_monitor()

        # Remove pam wrapper files, as they may break other tests
        [shutil.rmtree(i) for i in glob.glob('/tmp/pam.[0-9A-z]')]

    def setUp(self):
        (self.p_mock, self.obj_fprintd_manager) = self.spawn_server_template(
            self.template_name, {}, stdout=subprocess.PIPE)
        # set log to nonblocking
        flags = fcntl.fcntl(self.p_mock.stdout, fcntl.F_GETFL)
        fcntl.fcntl(self.p_mock.stdout, fcntl.F_SETFL, flags | os.O_NONBLOCK)
        self.obj_fprintd_mock = dbus.Interface(self.obj_fprintd_manager, 'net.reactivated.Fprint.Manager.Mock')

    def tearDown(self):
        self.p_mock.terminate()
        self.p_mock.wait()

    def setup_device(self):
        device_path = self.obj_fprintd_mock.AddDevice('FDO Trigger Finger Laser Reader', 3, 'swipe')
        self.device_mock = self.dbus_con.get_object('net.reactivated.Fprint', device_path)
        self.device_mock.SetEnrolledFingers('toto', ['left-little-finger', 'right-little-finger'])

    def test_pam_fprintd_identify_error(self):
        self.setup_device()
        script = [
            ( 'verify-unknown-error', True, 2 )
        ]
        self.device_mock.SetVerifyScript(script)

        tc = pypamtest.TestCase(pypamtest.PAMTEST_AUTHENTICATE, expected_rv=PAM_AUTHINFO_UNAVAIL)
        res = pypamtest.run_pamtest("toto", "fprintd-pam-test", [tc], [ 'unused' ])

        self.assertRegex(res.info[0], r'Swipe your left little finger across the fingerprint reader')
        self.assertEqual(len(res.errors), 0)

    def test_pam_fprintd_identify_error2(self):
        self.setup_device()
        script = [
            ( 'verify-disconnected', True, 2 )
        ]
        self.device_mock.SetVerifyScript(script)

        tc = pypamtest.TestCase(pypamtest.PAMTEST_AUTHENTICATE, expected_rv=PAM_AUTHINFO_UNAVAIL)
        res = pypamtest.run_pamtest("toto", "fprintd-pam-test", [tc], [ 'unused' ])

        self.assertRegex(res.info[0], r'Swipe your left little finger across the fingerprint reader')
        self.assertEqual(len(res.errors), 0)

    def test_pam_fprintd_identify_error3(self):
        self.setup_device()
        script = [
            ( 'verify-INVALID', True, 2 )
        ]
        self.device_mock.SetVerifyScript(script)

        tc = pypamtest.TestCase(pypamtest.PAMTEST_AUTHENTICATE, expected_rv=PAM_AUTH_ERR)
        res = pypamtest.run_pamtest("toto", "fprintd-pam-test", [tc], [ 'unused' ])

        self.assertRegex(res.info[0], r'Swipe your left little finger across the fingerprint reader')
        self.assertEqual(len(res.errors), 1)
        self.assertRegex(res.errors[0], r'An unknown error occurred')

    def test_pam_fprintd_auth(self):
        self.setup_device()
        script = [
            ( 'verify-match', True, 2 )
        ]
        self.device_mock.SetVerifyScript(script)

        tc = pypamtest.TestCase(pypamtest.PAMTEST_AUTHENTICATE, expected_rv=PAM_SUCCESS)
        res = pypamtest.run_pamtest("toto", "fprintd-pam-test", [tc], [ 'unused' ])

        self.assertRegex(res.info[0], r'Swipe your left little finger across the fingerprint reader')
        self.assertEqual(len(res.errors), 0)

    def test_pam_fprintd_dual_reader_auth(self):
        device_path = self.obj_fprintd_mock.AddDevice('FDO Sandpaper Reader', 3, 'press')
        sandpaper_device_mock = self.dbus_con.get_object('net.reactivated.Fprint', device_path)
        sandpaper_device_mock.SetEnrolledFingers('toto', ['left-middle-finger', 'right-middle-finger'])
        script = [
            ( 'verify-match', True, 2 )
        ]
        sandpaper_device_mock.SetVerifyScript(script)

        # Add a 2nd device
        self.setup_device()

        tc = pypamtest.TestCase(pypamtest.PAMTEST_AUTHENTICATE, expected_rv=PAM_SUCCESS)
        res = pypamtest.run_pamtest("toto", "fprintd-pam-test", [tc], [ 'unused' ])

        self.assertRegex(res.info[0], r'Place your left middle finger on FDO Sandpaper Reader')
        self.assertEqual(len(res.errors), 0)

    def test_pam_fprintd_last_try_auth(self):
        self.setup_device()
        script = [
            ( 'verify-no-match', True, 1 ),
            ( 'verify-no-match', True, 1 ),
            ( 'verify-match', True, 1 ),
        ]
        self.device_mock.SetVerifyScript(script)

        tc = pypamtest.TestCase(pypamtest.PAMTEST_AUTHENTICATE, expected_rv=PAM_SUCCESS)
        res = pypamtest.run_pamtest("toto", "fprintd-pam-test", [tc], [ 'unused' ])

        self.assertRegex(res.info[0], r'Swipe your left little finger across the fingerprint reader')
        self.assertEqual(len(res.errors), 2)
        self.assertRegex(res.errors[0], r'Failed to match fingerprint')
        self.assertRegex(res.errors[1], r'Failed to match fingerprint')

    def test_pam_fprintd_failed_auth(self):
        self.setup_device()
        script = [
            ( 'verify-no-match', True, 1 ),
            ( 'verify-no-match', True, 1 ),
            ( 'verify-no-match', True, 1 ),
        ]
        self.device_mock.SetVerifyScript(script)

        tc = pypamtest.TestCase(pypamtest.PAMTEST_AUTHENTICATE, expected_rv=PAM_MAXTRIES)
        res = pypamtest.run_pamtest("toto", "fprintd-pam-test", [tc], [ 'unused' ])

        self.assertRegex(res.info[0], r'Swipe your left little finger across the fingerprint reader')
        self.assertEqual(len(res.errors), 3)
        self.assertRegex(res.errors[0], r'Failed to match fingerprint')
        self.assertRegex(res.errors[1], r'Failed to match fingerprint')
        self.assertRegex(res.errors[2], r'Failed to match fingerprint')

    def test_pam_timeout(self):
        self.setup_device()

        tc = pypamtest.TestCase(pypamtest.PAMTEST_AUTHENTICATE, expected_rv=PAM_AUTHINFO_UNAVAIL)
        res = pypamtest.run_pamtest("toto", "fprintd-pam-test", [tc], [ 'unused' ])
        self.assertRegex(res.info[1], r'Verification timed out')

if __name__ == '__main__':
    if 'PAM_WRAPPER_SERVICE_DIR' not in os.environ:
        print('Cannot run test without environment set correctly, run "meson test" instead')
        sys.exit(1)
    # set stream to sys.stderr to get debug output
    unittest.main(testRunner=unittest.TextTestRunner(stream=sys.stdout, verbosity=2))
