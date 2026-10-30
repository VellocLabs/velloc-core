# Copyright 2026 VellocLabs
# Use of this source code is governed by the Apache License, Version 2.0,
# which can be found in the LICENSE file.
"""Tests for the installer payload list, chrome.release.

create_installer_archive.py skips an entry that matches no file without
failing, so a misspelt locale would silently drop out of the installer.

Run: python custom_browser/installer/chrome_release_test.py
"""

import configparser
import os
import re
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_SRC = os.path.normpath(os.path.join(_HERE, '..', '..'))
_RELEASE = os.path.join(_SRC, 'chrome', 'installer', 'mini_installer',
                        'chrome.release')
_LOCALES_GNI = os.path.join(_SRC, 'build', 'config', 'locales.gni')
_STRINGS_GRD = os.path.join(_HERE, '..', 'resources',
                            'custom_browser_strings.grd')
_INSTALLER_BUILD_GN = os.path.join(_HERE, 'BUILD.gn')
_BUNDLED = os.path.join(_HERE, 'bundled')

# Each third-party binary the installer ships, and the licence text that
# must be installed beside it.
_BUNDLED_LICENCES = {
    'rg.exe': 'rg-LICENSE-MIT',
    'conpty.dll': 'conpty-LICENSE-MIT',
    'OpenConsole.exe': 'conpty-LICENSE-MIT',
}

# The languages Velloc's own strings ship in (pt as pt-BR), plus Traditional
# Chinese; every other system language gets the en-US browser UI.
_SHIPPED_LOCALES = {'en-US', 'bn', 'de', 'es', 'fr', 'hi', 'ja', 'ko',
                    'pt-BR', 'ru', 'zh-CN', 'zh-TW'}


def _general_options():
  # Parsed the way create_installer_archive.py parses it.
  config = configparser.ConfigParser(
      {'ChromeDir': 'C', 'VersionDir': 'V'})
  config.read(_RELEASE, encoding='utf-8')
  return config.options('GENERAL')


def _chromium_locales():
  with open(_LOCALES_GNI, encoding='utf-8') as f:
    gni = f.read()
  block = re.search(r'^all_chrome_locales =\s*\[(.*?)\]', gni,
                    re.MULTILINE | re.DOTALL).group(1)
  return set(re.findall(r'"([^"]+)"', block))


def _custom_string_paks():
  # Every data pack the strings .grd builds.
  with open(_STRINGS_GRD, encoding='utf-8') as f:
    grd = f.read()
  return set(re.findall(
      r'<output filename="(custom_browser_strings_[^"]+\.pak)"\s+'
      r'type="data_package"', grd))


class ChromeReleaseTest(unittest.TestCase):

  def setUp(self):
    self.options = _general_options()

  def _locale_entries(self):
    return [o for o in self.options if o.startswith('locales\\')]

  def test_ships_only_the_velloc_locales(self):
    shipped = {o[len('locales\\'):-len('.pak')] for o in self._locale_entries()}
    # ConfigParser lowercases option names; the archive globs insensitively.
    self.assertEqual(shipped, {l.lower() for l in _SHIPPED_LOCALES})

  def test_no_wildcard_locale_entry(self):
    for option in self._locale_entries():
      self.assertNotIn('*', option)

  def test_every_shipped_locale_is_a_chromium_locale(self):
    self.assertTrue(_SHIPPED_LOCALES <= _chromium_locales(),
                    _SHIPPED_LOCALES - _chromium_locales())

  def test_ships_every_custom_string_pak(self):
    paks = _custom_string_paks()
    self.assertIn('custom_browser_strings_en-US.pak', paks)
    self.assertGreater(len(paks), 1)
    shipped = {o for o in self.options
               if o.startswith('custom_browser_strings_')}
    self.assertEqual(shipped, {p.lower() for p in paks})

  def test_every_custom_string_language_has_a_browser_locale(self):
    # A custom pak is picked by the browser's locale (custom_browser_strings
    # hook), so a language without a shipped browser pak never shows.
    shipped = {l.split('-')[0] for l in _SHIPPED_LOCALES}
    for pak in _custom_string_paks():
      lang = pak[len('custom_browser_strings_'):-len('.pak')].split('-')[0]
      self.assertIn(lang, shipped, pak)

  def test_no_component_extension_payload(self):
    # No bundled extension is listed in extensions/extensions.json, so a
    # resources\custom_extensions dir in the out dir is stale.
    self.assertFalse([o for o in self.options if 'custom_extensions' in o])

  def test_ships_the_licence_of_every_bundled_binary(self):
    # The archive skips an entry with no file in the out dir, so each licence
    # must also be copied there by installer/BUILD.gn.
    with open(_INSTALLER_BUILD_GN, encoding='utf-8') as f:
      copied = set(re.findall(r'"bundled/([^"]+)"', f.read()))
    for binary, licence in _BUNDLED_LICENCES.items():
      self.assertIn(binary.lower(), self.options, binary)
      self.assertIn(licence.lower(), self.options, licence)
      self.assertIn(licence, copied, licence)
      self.assertTrue(os.path.isfile(os.path.join(_BUNDLED, licence)), licence)


if __name__ == '__main__':
  unittest.main()
