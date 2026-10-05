# Copyright 2026 The Custom Browser Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Tests for installer_strings_brand.py.

Run: python custom_browser/resources/grit/installer_strings_brand_test.py
"""

import json
import os
import re
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import installer_strings_brand  # pylint: disable=wrong-import-position

_HERE = os.path.dirname(os.path.abspath(__file__))
_CB_ROOT = os.path.normpath(os.path.join(_HERE, '..', '..'))
_BRANDING_CONSTANTS = os.path.join(_CB_ROOT, 'common', 'branding_constants.h')
_BRANDING = os.path.join(_CB_ROOT, 'chromium_overrides', 'velloc', 'symlink',
                         'chrome', 'app', 'theme', 'velloc', 'BRANDING')
with open(os.path.join(_HERE, 'config', 'branding.json'),
          encoding='utf-8') as _f:
  _DISPLAY_NAME = json.load(_f)['display_name']


class InstallerStringsBrandTest(unittest.TestCase):

  def setUp(self):
    self.build_dir = tempfile.mkdtemp()

  def tearDown(self):
    shutil.rmtree(self.build_dir)

  def _write_args(self, text):
    with open(os.path.join(self.build_dir, 'args.gn'), 'w') as f:
      f.write(text)

  def test_rc_language_to_locale(self):
    to_locale = installer_strings_brand.rc_language_to_locale
    self.assertEqual(to_locale('EN_US'), 'en-US')
    self.assertEqual(to_locale('ZH_CN'), 'zh-CN')
    self.assertEqual(to_locale('FR'), 'fr')
    self.assertEqual(to_locale('ES_419'), 'es-419')

  def test_brands_a_custom_build(self):
    self._write_args('enable_custom_browser = true\n'
                     'branding_path_component = "Velloc"\n')
    brand = installer_strings_brand.make_brander(self.build_dir)
    self.assertIsNotNone(brand)
    self.assertEqual(brand('Chromium Apps', 'EN_US'), 'Velloc Code Apps')
    self.assertEqual(brand('Chromium (mDNS-In)', 'EN_US'),
                     'Velloc Code (mDNS-In)')
    self.assertEqual(brand('Chromium 应用', 'ZH_CN'), 'Velloc Code 应用')
    self.assertEqual(brand('Access the Internet', 'EN_US'),
                     'Access the Internet')

  def test_brands_next_to_cjk_and_hangul(self):
    # Python's \b sees no boundary between "Chromium" and a CJK / Hangul
    # letter; branding.json bounds the name by ASCII word characters.
    self._write_args('enable_custom_browser = true\n'
                     'branding_path_component = "Velloc"\n')
    brand = installer_strings_brand.make_brander(self.build_dir)
    self.assertEqual(brand('Chromium的入站规则', 'ZH_CN'), 'Velloc Code的入站规则')
    self.assertEqual(brand('Chromium이 실행 중', 'KO'), 'Velloc Code이 실행 중')
    self.assertEqual(brand('Chromeを開く', 'JA'), 'Velloc Codeを開く')
    self.assertEqual(brand('ChromeOS', 'EN_US'), 'ChromeOS')
    self.assertEqual(brand('chromium_src', 'EN_US'), 'chromium_src')

  def test_keeps_the_blacklisted_phrases(self):
    self._write_args('enable_custom_browser = true\n'
                     'branding_path_component = "Velloc"\n')
    brand = installer_strings_brand.make_brander(self.build_dir)
    self.assertEqual(brand('Based on Chromium', 'EN_US'), 'Based on Chromium')

  def test_no_brander_for_a_plain_build(self):
    self._write_args('enable_custom_browser = false\n'
                     'branding_path_component = "Velloc"\n')
    self.assertIsNone(installer_strings_brand.make_brander(self.build_dir))
    self._write_args('enable_custom_browser = true\n'
                     'branding_path_component = "chromium"\n')
    self.assertIsNone(installer_strings_brand.make_brander(self.build_dir))

  def test_brand_is_the_display_name_not_the_path_component(self):
    # branding_path_component names directories and registry keys; text a
    # user reads gets branding.json's display_name, which is the product
    # name the C++ side shows (branding_constants.h).
    self._write_args('enable_custom_browser = true\n'
                     'branding_path_component = "Velloc"\n')
    brand = installer_strings_brand.make_brander(self.build_dir)
    self.assertEqual(brand('%brand%', 'EN_US'), _DISPLAY_NAME)
    self.assertNotEqual(_DISPLAY_NAME, 'Velloc')

  def test_display_name_matches_branding_constants(self):
    with open(_BRANDING_CONSTANTS, encoding='utf-8') as f:
      source = f.read()
    match = re.search(
        r'kCustomBrowserProductName\[?\]?\s*=\s*"([^"]*)"', source)
    self.assertIsNotNone(match)
    self.assertEqual(match.group(1), _DISPLAY_NAME)

  def test_display_name_matches_branding_file(self):
    # BRANDING's PRODUCT_FULLNAME names setup.exe, mini_installer and the
    # browser's version resources; the wrapper test pins the wrapper to it.
    with open(_BRANDING, encoding='utf-8') as f:
      values = dict(line.strip().partition('=')[::2] for line in f
                    if '=' in line)
    self.assertEqual(values['PRODUCT_FULLNAME'], _DISPLAY_NAME)

  def test_publisher_matches_branding_file(self):
    # setup.exe's Apps & features publisher (kCustomBrowserPublisherName)
    # and the version resources' CompanyName (COMPANY_FULLNAME) agree.
    with open(_BRANDING_CONSTANTS, encoding='utf-8') as f:
      match = re.search(r'kCustomBrowserPublisherName\s*=\s*"([^"]*)"',
                        f.read())
    self.assertIsNotNone(match)
    with open(_BRANDING, encoding='utf-8') as f:
      values = dict(line.strip().partition('=')[::2] for line in f
                    if '=' in line)
    self.assertEqual(values['COMPANY_FULLNAME'], match.group(1))

  def test_no_brander_without_args_gn(self):
    self.assertIsNone(installer_strings_brand.make_brander(self.build_dir))


if __name__ == '__main__':
  unittest.main()
