# Copyright 2026 VellocLabs
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Tests for custom_browser_installer_wrapper.nsi.

Running the wrapper would act on this machine's real Velloc install (it
reads and edits HKCU\\Software\\Velloc and launches the installed
chrome.exe), so these tests compile it and check its structure instead.

Run: python custom_browser/installer/wrapper_nsi_test.py
"""

import os
import re
import shutil
import subprocess
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_WRAPPER = os.path.join(_HERE, 'custom_browser_installer_wrapper.nsi')
# The product's own branding: names and copyright of setup.exe, mini_installer
# and the installed browser (build/util/branding.gni reads it).
_BRANDING = os.path.join(_HERE, '..', 'chromium_overrides', 'velloc',
                         'symlink', 'chrome', 'app', 'theme', 'velloc',
                         'BRANDING')


def _find_makensis():
  found = shutil.which('makensis')
  if found:
    return found
  for root in (os.environ.get('ProgramFiles(x86)'),
               os.environ.get('ProgramFiles')):
    if root:
      candidate = os.path.join(root, 'NSIS', 'makensis.exe')
      if os.path.isfile(candidate):
        return candidate
  return None


def _function_body(source, name):
  match = re.search(r'^Function %s\s*$(.*?)^FunctionEnd' % re.escape(name),
                    source, re.MULTILINE | re.DOTALL)
  return match.group(1) if match else None


def _launch_index(body):
  """Offset of the Exec statement that launches chrome.exe."""
  match = re.search(r"^\s*Exec '", body, re.MULTILINE)
  assert match, 'LaunchInstalledChrome no longer launches with Exec'
  return match.start()


class WrapperStructureTest(unittest.TestCase):

  def setUp(self):
    with open(_WRAPPER, encoding='utf-8') as f:
      self.source = f.read()

  def test_silent_install_returns_before_launching(self):
    body = _function_body(self.source, 'LaunchInstalledChrome')
    self.assertIsNotNone(body)
    silent = re.search(r'\$\{If\} \$\{Silent\}\s*\n(.*?)\$\{EndIf\}', body,
                       re.DOTALL)
    self.assertIsNotNone(silent, 'LaunchInstalledChrome has no ${Silent} guard')
    self.assertRegex(silent.group(1), r'(?m)^\s*Return\s*$')
    self.assertLess(silent.start(), _launch_index(body),
                    'the ${Silent} guard must come before the launch')

  def test_system_install_returns_before_launching(self):
    body = _function_body(self.source, 'LaunchInstalledChrome')
    guard = body.index('${If} $InstallMode == "system"')
    self.assertLess(guard, _launch_index(body))

  def test_launch_runs_only_on_success(self):
    body = _function_body(self.source, '.onInstSuccess')
    self.assertIn('Call LaunchInstalledChrome', body)



def _read_branding():
  values = {}
  with open(_BRANDING, encoding='utf-8') as f:
    for line in f:
      key, sep, value = line.strip().partition('=')
      if sep:
        values[key] = value
  return values


def _wrapper_default(source, name):
  match = re.search(r'^!ifndef %s\s*\n\s*!define %s "([^"]*)"' % (name, name),
                    source, re.MULTILINE)
  assert match, 'no default for %s' % name
  return match.group(1)


class WrapperBrandingTest(unittest.TestCase):
  """The wrapper names the product the way the installed product does.

  The Store listing, the wrapper's file and its version resource must all
  say what Apps & Features says once installed — PRODUCT_FULLNAME in
  BRANDING — and carry the same copyright holder as setup.exe.
  """

  def setUp(self):
    with open(_WRAPPER, encoding='utf-8') as f:
      self.source = f.read()
    self.branding = _read_branding()

  def test_product_name_matches_branding(self):
    product = self.branding['PRODUCT_FULLNAME']
    self.assertEqual(_wrapper_default(self.source, 'PRODUCT_NAME'), product)
    self.assertEqual(_wrapper_default(self.source, 'MINI_PRODUCT_NAME'),
                     product)
    self.assertEqual(_wrapper_default(self.source, 'MINI_FILE_DESCRIPTION'),
                     self.branding['PRODUCT_INSTALLER_FULLNAME'])
    self.assertEqual(_wrapper_default(self.source, 'MINI_COMPANY_NAME'),
                     self.branding['COMPANY_FULLNAME'])
    self.assertEqual(_wrapper_default(self.source, 'PRODUCT_PUBLISHER'),
                     self.branding['COMPANY_FULLNAME'])

  def test_installer_names_carry_the_product_name(self):
    product = self.branding['PRODUCT_FULLNAME']
    for key in ('PRODUCT_INSTALLER_FULLNAME', 'PRODUCT_INSTALLER_SHORTNAME'):
      self.assertEqual(self.branding[key], product + ' Installer', key)

  def test_copyright_matches_branding(self):
    # BRANDING's year is @LASTCHANGE_YEAR@; the wrapper's is literal.
    pattern = re.escape(self.branding['COPYRIGHT']).replace(
        re.escape('@LASTCHANGE_YEAR@'), r'\d{4}')
    self.assertRegex(_wrapper_default(self.source, 'MINI_COPYRIGHT'),
                     '^%s$' % pattern)

@unittest.skipUnless(_find_makensis(), 'makensis not installed')
class WrapperCompilesTest(unittest.TestCase):

  def setUp(self):
    self.work = tempfile.mkdtemp()

  def tearDown(self):
    shutil.rmtree(self.work)

  def _makensis(self, *args, cwd):
    return subprocess.run([_find_makensis(), '/V2', *args], cwd=cwd,
                          capture_output=True, text=True, errors='replace')

  def test_wrapper_compiles(self):
    # A stand-in for mini_installer.exe: the wrapper only embeds it.
    fake = os.path.join(self.work, 'fake.nsi')
    with open(fake, 'w') as f:
      f.write('OutFile "mini_installer.exe"\nSilentInstall silent\n'
              'Section\nSectionEnd\n')
    result = self._makensis(fake, cwd=self.work)
    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    # PRODUCT_NAME carries the temp dir so OutFile lands there, not beside
    # the script (where the real "Velloc Code Setup.exe" lives).
    product = os.path.join(self.work, 'Velloc')
    result = self._makensis(
        '/DPRODUCT_NAME=' + product,
        '/DPRODUCT_VERSION=1.2.3.4',
        '/DMINI_INSTALLER_SOURCE=' +
        os.path.join(self.work, 'mini_installer.exe'),
        '/DMASTER_PREFERENCES_SOURCE=' +
        os.path.join(_HERE, 'master_preferences'),
        _WRAPPER,
        cwd=_HERE)
    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
    self.assertTrue(os.path.isfile(product + ' Setup.exe'))


if __name__ == '__main__':
  unittest.main()
