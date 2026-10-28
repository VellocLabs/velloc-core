# Copyright 2026 VellocLabs
# Use of this source code is governed by the Apache License, Version 2.0,
# which can be found in the LICENSE file.
"""Tests for custom_browser_installer_wrapper.nsi.

Running the wrapper would act on this machine's real Velloc install (it
reads and edits HKCU\\Software\\Velloc and launches the installed
the browser exe), so these tests compile it and check its structure instead.

Run: python custom_browser/installer/wrapper_nsi_test.py
"""

import json
import os
import re
import shutil
import subprocess
import tempfile
import time
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_WRAPPER = os.path.join(_HERE, 'custom_browser_installer_wrapper.nsi')
# The product's own branding: names and copyright of setup.exe, mini_installer
# and the installed browser (build/util/branding.gni reads it).
_BRANDING = os.path.join(_HERE, '..', 'chromium_overrides', 'velloc',
                         'symlink', 'chrome', 'app', 'theme', 'velloc',
                         'BRANDING')
_CHROME_CONSTANTS = os.path.join(_HERE, '..', '..', 'chrome', 'common',
                                 'chrome_constants.h')


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
  """Offset of the Exec statement that launches the browser exe."""
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


def _chrome_constant(name):
  """The string literal Chromium's chrome_constants.h gives `name`."""
  with open(_CHROME_CONSTANTS, encoding='utf-8') as f:
    source = f.read()
  match = re.search(r'%s\[\]\s*=\s*(?:FILE_PATH_LITERAL\()?L?"([^"]*)"' %
                    re.escape(name), source)
  assert match, 'no %s in chrome_constants.h' % name
  return match.group(1)


class CloseRunningProductTest(unittest.TestCase):
  """Setup closes a running browser before it replaces files."""

  def setUp(self):
    with open(_WRAPPER, encoding='utf-8') as f:
      self.source = f.read()
    self.body = _function_body(self.source, 'CloseRunningProduct')

  def test_runs_first_in_the_install_section(self):
    section = self.source.split('Section "Install"', 1)[1]
    calls = re.findall(r'^\s*Call (\w+)', section, re.MULTILINE)
    self.assertEqual(calls[0], 'CloseRunningProduct')

  def test_asks_before_closing_unless_silent(self):
    self.assertIsNotNone(self.body)
    self.assertLess(self.body.index('${IfNot} ${Silent}'),
                    self.body.index('MB_YESNO'))
    self.assertLess(self.body.index('MB_YESNO'),
                    self.body.index('taskkill $RunningPids'))

  def test_closes_normally_before_forcing(self):
    self.assertLess(self.body.index('taskkill $RunningPids'),
                    self.body.index('taskkill /F /T $RunningPids'))

  def test_only_processes_of_the_install_dir_are_closed(self):
    # Matching by image name alone also caught (and could not end) a
    # developer build's velloc.exe outside the install.
    running = _function_body(self.source, 'IsProductRunning')
    self.assertIn('Push $ProductDir', running)
    self.assertIn('Call FindProductPids', running)
    for body in (running, self.body):
      self.assertNotIn('IMAGENAME', body)
      self.assertNotIn('/IM', body)
    self.assertLess(self.body.index('Call ResolveProductDir'),
                    self.body.index('Call IsProductRunning'))

  def test_product_dir_follows_the_install_mode(self):
    body = _function_body(self.source, 'ResolveProductDir')
    system, user = body.split('${Else}', 1)
    self.assertIn('ReadRegStr $ProductDir HKLM', system)
    self.assertIn('"$PROGRAMFILES64\\\\Velloc\\\\Application"', system)
    self.assertIn('ReadRegStr $ProductDir HKCU', user)
    self.assertIn('"$LOCALAPPDATA\\\\Velloc\\\\Application"', user)

  def test_offers_retry_and_aborts_on_cancel(self):
    self.assertIn('MB_RETRYCANCEL', self.body)
    self.assertRegex(self.body, r'(?s)cancelled:.*SetErrorLevel 1602.*Abort')


class FirstRunSentinelTest(unittest.TestCase):
  """The wrapper writes the sentinel Chromium reads to skip its first run."""

  def setUp(self):
    with open(_WRAPPER, encoding='utf-8') as f:
      self.source = f.read()
    self.body = _function_body(self.source, 'SkipBrowserFirstRun')
    self.assertIsNotNone(self.body)

  def test_path_matches_chromium_user_data_dir(self):
    # The product dir is the one the install-dir fallback names.
    install_dir = re.search(r'"\$LOCALAPPDATA\\\\(\w+)\\\\Application"',
                            self.source)
    self.assertIsNotNone(install_dir)
    user_data = '$LOCALAPPDATA\\\\%s\\\\%s' % (
        install_dir.group(1), _chrome_constant('kUserDataDirname'))
    self.assertIn('StrCpy $R0 "%s"' % user_data, self.body)
    self.assertIn('FileOpen $R1 "$R0\\\\%s" w' %
                  _chrome_constant('kFirstRunSentinel'), self.body)

  def test_existing_sentinel_is_left_untouched(self):
    guard = re.search(r'\$\{If\} \$\{FileExists\} "\$R0\\\\First Run"\s*\n'
                      r'\s*Return\s*\n', self.body)
    self.assertIsNotNone(guard, 'no FileExists guard returning early')
    self.assertLess(guard.start(), self.body.index('FileOpen'))

  def test_written_before_the_mini_installer_runs(self):
    # setup.exe may launch the browser from inside the mini installer; that
    # launch must already find the sentinel.
    section = re.search(r'^Section "Install"\s*$(.*?)^SectionEnd',
                        self.source, re.MULTILINE | re.DOTALL).group(1)
    self.assertEqual(section.count('Call SkipBrowserFirstRun'), 1)
    self.assertLess(section.index('Call SkipBrowserFirstRun'),
                    section.index('ExecWait'))


class MasterPreferencesTest(unittest.TestCase):
  """The initial preferences handed to the mini installer."""

  def setUp(self):
    with open(os.path.join(_HERE, 'master_preferences'),
              encoding='utf-8') as f:
      self.prefs = json.load(f)

  def test_setup_does_not_launch_the_browser(self):
    # The wrapper's own launch (LaunchInstalledChrome) is the only one.
    self.assertIs(
        self.prefs.get('distribution', {}).get('do_not_launch_chrome'), True)

  def test_holds_no_profile_prefs(self):
    # The browser applies profile prefs from this file on its first run only,
    # and the wrapper's "First Run" sentinel means it never has one; Velloc's
    # profile defaults are registered by the browser instead. Only setup's
    # `distribution` keys belong here.
    self.assertEqual(list(self.prefs), ['distribution'])


_CHANNEL_NSH = os.path.join(_HERE, 'installer_channel.nsh')
_CHANNEL_HEADER = os.path.join(_HERE, '..', 'browser', 'nexus',
                               'nexus_installer_channel.h')


def _install_section(source):
  return re.search(r'^Section "Install"\s*$(.*?)^SectionEnd', source,
                   re.MULTILINE | re.DOTALL).group(1)


class InstallerChannelMarkerTest(unittest.TestCase):
  """A beta-named installer leaves the marker the browser reads (G-3)."""

  def setUp(self):
    with open(_WRAPPER, encoding='utf-8') as f:
      self.source = f.read()
    self.body = _function_body(self.source, 'RecordInstallerChannel')
    self.assertIsNotNone(self.body)

  def test_marker_path_matches_the_browser(self):
    with open(_CHANNEL_HEADER, encoding='utf-8') as f:
      header = f.read()
    name = re.search(r'kInstallerChannelFileName\[\]\s*=\s*'
                     r'FILE_PATH_LITERAL\("([^"]*)"\)', header)
    self.assertIsNotNone(name)
    user_data = '$LOCALAPPDATA\\\\Velloc\\\\%s' % _chrome_constant(
        'kUserDataDirname')
    self.assertIn('!define INSTALLER_CHANNEL_MARKER "%s\\\\%s"' %
                  (user_data, name.group(1)), self.source)
    self.assertIn('FileOpen $R1 "${INSTALLER_CHANNEL_MARKER}" w', self.body)

  def test_channel_comes_from_the_installer_file_name(self):
    self.assertIn(
        '!insertmacro INSTALLER_CHANNEL_FROM_NAME $InstallerChannel '
        '"$EXEFILE"', self.body)

  def test_written_before_the_mini_installer_runs(self):
    section = _install_section(self.source)
    record = section.index('Call RecordInstallerChannel')
    self.assertLess(section.index('Call UninstallLegacySchemeInstall'), record)
    self.assertLess(record, section.index('ExecWait'))

  def test_removed_when_the_install_fails(self):
    section = _install_section(self.source)
    failure = section[section.index('${Else}',
                                    section.index('${If} $MiniExit == 0')):]
    self.assertIn('Delete "${INSTALLER_CHANNEL_MARKER}"', failure)


@unittest.skipUnless(_find_makensis(), 'makensis not installed')
class InstallerChannelFromNameTest(unittest.TestCase):
  """Runs INSTALLER_CHANNEL_FROM_NAME in a probe installer under each name."""

  @classmethod
  def setUpClass(cls):
    cls.work = tempfile.mkdtemp()
    probe = os.path.join(cls.work, 'probe.nsi')
    with open(probe, 'w', encoding='utf-8') as f:
      f.write('!include "%s"\n'
              'OutFile "probe.exe"\n'
              'SilentInstall silent\n'
              'RequestExecutionLevel user\n'
              'Var Channel\n'
              'Section\n'
              '  !insertmacro INSTALLER_CHANNEL_FROM_NAME $Channel "$EXEFILE"\n'
              '  FileOpen $0 "$EXEDIR\\channel.txt" w\n'
              '  FileWrite $0 "[$Channel]"\n'
              '  FileClose $0\n'
              'SectionEnd\n' % _CHANNEL_NSH)
    result = subprocess.run([_find_makensis(), '/V2', probe], cwd=cls.work,
                            capture_output=True, text=True, errors='replace')
    assert result.returncode == 0, result.stdout + result.stderr
    cls.probe = os.path.join(cls.work, 'probe.exe')

  @classmethod
  def tearDownClass(cls):
    shutil.rmtree(cls.work)

  def _channel_for(self, name):
    run_dir = tempfile.mkdtemp(dir=self.work)
    exe = os.path.join(run_dir, name)
    shutil.copy(self.probe, exe)
    subprocess.run([exe], check=True, timeout=60)
    with open(os.path.join(run_dir, 'channel.txt'), encoding='utf-8') as f:
      return f.read()

  def test_beta_in_any_case_selects_beta(self):
    for name in ('install-beta.exe', 'Velloc Code Setup Beta.exe',
                 'VELLOC-BETA.exe', 'velloc_beta1.exe'):
      self.assertEqual(self._channel_for(name), '[beta]', name)

  def test_other_names_select_nothing(self):
    for name in ('install.exe', 'Velloc Code Setup.exe', 'bet-a.exe'):
      self.assertEqual(self._channel_for(name), '[]', name)


_RUNNING_NSH = os.path.join(_HERE, 'running_product.nsh')


@unittest.skipUnless(_find_makensis(), 'makensis not installed')
class IsPidArgListTest(unittest.TestCase):
  """Only a list FindProductPids could have built reaches taskkill."""

  VALID = ('', '/PID 12', '/PID 12 /PID 345')
  INVALID = ('Get-CimInstance : Invalid namespace', '/PID', '/PID 12 ',
             ' /PID 12', '/PID  12', '/PID 12x', '/PID -5', '/pid 12',
             '/PID 12 /PID', '/PID 0x1A', '/PID 12 12', '/PID 12$\\r$\\n')

  @classmethod
  def setUpClass(cls):
    cls.work = tempfile.mkdtemp()
    cases = ''.join(
        '  Push "%s"\n'
        '  Call IsPidArgList\n'
        '  Pop $0\n'
        '  FileWrite $1 "$0$\\n"\n' % case
        for case in cls.VALID + cls.INVALID)
    probe = os.path.join(cls.work, 'probe.nsi')
    with open(probe, 'w', encoding='utf-8') as f:
      f.write('!define BROWSER_EXE "velloc.exe"\n'
              '!include "%s"\n'
              'OutFile "probe.exe"\n'
              'SilentInstall silent\n'
              'RequestExecutionLevel user\n'
              'Section\n'
              '  FileOpen $1 "$EXEDIR\\results.txt" w\n'
              '%s'
              '  FileClose $1\n'
              'SectionEnd\n' % (_RUNNING_NSH, cases))
    result = subprocess.run([_find_makensis(), '/V2', probe], cwd=cls.work,
                            capture_output=True, text=True, errors='replace')
    assert result.returncode == 0, result.stdout + result.stderr
    subprocess.run([os.path.join(cls.work, 'probe.exe')], check=True,
                   timeout=60)
    with open(os.path.join(cls.work, 'results.txt'), encoding='utf-8') as f:
      cls.results = f.read().split('\n')

  @classmethod
  def tearDownClass(cls):
    shutil.rmtree(cls.work)

  def test_lists_built_here_are_accepted(self):
    for i, case in enumerate(self.VALID):
      self.assertEqual(self.results[i], '1', repr(case))

  def test_anything_else_is_rejected(self):
    for i, case in enumerate(self.INVALID, len(self.VALID)):
      self.assertEqual(self.results[i], '0', repr(case))


@unittest.skipUnless(_find_makensis(), 'makensis not installed')
class FindProductPidsTest(unittest.TestCase):
  """Runs FindProductPids against live stand-in velloc.exe processes."""

  @classmethod
  def setUpClass(cls):
    cls.work = tempfile.mkdtemp()
    # A stand-in browser: with "spawn" it starts one child copy of itself
    # (a renderer), writes spawned.txt, then sleeps.
    stub = os.path.join(cls.work, 'stub.nsi')
    with open(stub, 'w', encoding='utf-8') as f:
      f.write('!include "FileFunc.nsh"\n'
              'OutFile "velloc.exe"\n'
              'SilentInstall silent\n'
              'RequestExecutionLevel user\n'
              'Section\n'
              '  ${GetParameters} $0\n'
              '  StrCmp $0 "spawn" 0 +5\n'
              '  Exec \'"$EXEPATH"\'\n'
              '  FileOpen $1 "$EXEDIR\\spawned.txt" w\n'
              '  FileWrite $1 "1"\n'
              '  FileClose $1\n'
              '  Sleep 60000\n'
              'SectionEnd\n')
    cls._makensis(stub)
    cls.stub = os.path.join(cls.work, 'velloc.exe')
    cls.procs = []

  @classmethod
  def tearDownClass(cls):
    for proc in cls.procs:
      subprocess.run(['taskkill', '/F', '/T', '/PID', str(proc.pid)],
                     capture_output=True)
      proc.wait(timeout=30)
    shutil.rmtree(cls.work, ignore_errors=True)

  @classmethod
  def _makensis(cls, script, *defines):
    result = subprocess.run([_find_makensis(), '/V2', *defines, script],
                            cwd=cls.work, capture_output=True, text=True,
                            errors='replace')
    assert result.returncode == 0, result.stdout + result.stderr

  def _start_browser(self, dir_name):
    run_dir = os.path.join(self.work, dir_name)
    os.makedirs(run_dir)
    exe = os.path.join(run_dir, 'velloc.exe')
    shutil.copy(self.stub, exe)
    proc = subprocess.Popen([exe, 'spawn'])
    self.procs.append(proc)
    marker = os.path.join(run_dir, 'spawned.txt')
    for _ in range(100):
      if os.path.isfile(marker):
        break
      time.sleep(0.1)
    self.assertTrue(os.path.isfile(marker), 'stub never spawned its child')
    return run_dir, proc.pid

  def _pids_for(self, product_dir, *defines):
    probe_dir = tempfile.mkdtemp(dir=self.work)
    probe = os.path.join(probe_dir, 'probe.nsi')
    with open(probe, 'w', encoding='utf-8') as f:
      f.write('!define BROWSER_EXE "velloc.exe"\n'
              '!include "%s"\n'
              'OutFile "probe.exe"\n'
              'SilentInstall silent\n'
              'RequestExecutionLevel user\n'
              'Section\n'
              '  Push "%s"\n'
              '  Call FindProductPids\n'
              '  Pop $0\n'
              '  FileOpen $1 "$EXEDIR\\pids.txt" w\n'
              '  FileWrite $1 "[$0]"\n'
              '  FileClose $1\n'
              'SectionEnd\n' % (_RUNNING_NSH, product_dir))
    result = subprocess.run([_find_makensis(), '/V2', *defines, probe],
                            cwd=probe_dir, capture_output=True, text=True,
                            errors='replace')
    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
    subprocess.run([os.path.join(probe_dir, 'probe.exe')], check=True,
                   timeout=120)
    with open(os.path.join(probe_dir, 'pids.txt'), encoding='utf-8') as f:
      return f.read()

  def test_a_failed_query_falls_back_to_the_image_name(self):
    # A query error is non-terminating in PowerShell; it must still fail the
    # run, or its message would be handed to taskkill.
    self.assertEqual(
        self._pids_for(self.work, '/DRUNNING_PRODUCT_QUERY='
                       'Get-CimInstance -Namespace root/nope Win32_Process'),
        '[/IM "velloc.exe"]')

  def test_lists_only_the_root_of_the_install_dir(self):
    # The quote checks the directory reaches PowerShell intact; the sibling
    # whose name extends the install dir's checks the match is by directory.
    product_dir, root = self._start_browser("Velloc's App")
    self._start_browser("Velloc's App2")
    self._start_browser('dev-build')
    self.assertEqual(self._pids_for(product_dir), '[/PID %d]' % root)
    self.assertEqual(self._pids_for(product_dir + '\\'), '[/PID %d]' % root)

  def test_nothing_running_there_lists_nothing(self):
    self._start_browser('elsewhere')
    empty = os.path.join(self.work, 'not-running')
    os.makedirs(empty)
    self.assertEqual(self._pids_for(empty), '[]')



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
        '/DBROWSER_EXE=velloc.exe',
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
