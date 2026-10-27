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

# The Velloc UI languages, plus Traditional Chinese; every other system
# language gets the en-US browser UI.
_SHIPPED_LOCALES = {'en-US', 'de', 'es', 'fr', 'ja', 'ko', 'zh-CN', 'zh-TW'}


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

  def test_no_component_extension_payload(self):
    # No bundled extension is listed in extensions/extensions.json, so a
    # resources\custom_extensions dir in the out dir is stale.
    self.assertFalse([o for o in self.options if 'custom_extensions' in o])


if __name__ == '__main__':
  unittest.main()
