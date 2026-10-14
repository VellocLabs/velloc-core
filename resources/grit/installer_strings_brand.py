# Copyright 2026 VellocLabs
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Brands the installer's embedded strings with the GRIT branding rules.

setup.exe and mini_installer carry their own copy of a few strings from
chrome/app/chromium_strings.grd (installer errors, the firewall rule, the
"Apps" Start-menu folder, service descriptions...), extracted into
installer_util_strings.rc by base/win/embedded_i18n/create_string_rc.py.
That script reads the .grd/.xtb files with its own SAX parser and never runs
GRIT, so grit_patch.py never sees those strings and they kept saying
"Chromium". create_string_rc.py calls make_brander() (an ENABLE_CUSTOM_BROWSER
hook, P-5 chromium-hooks.md) and runs every string through it, with the same
config/branding.json rules GRIT applies to every other catalog.
"""

import os
import sys

_MODULE_DIR = os.path.dirname(os.path.abspath(__file__))
_SRC_ROOT = os.path.normpath(os.path.join(_MODULE_DIR, '..', '..', '..'))


def _read_gn_args(build_dir):
  sys.path.insert(0, os.path.join(_SRC_ROOT, 'build'))
  import gn_helpers  # pylint: disable=import-outside-toplevel
  return gn_helpers.ReadArgsGN(build_dir)


def rc_language_to_locale(rc_language):
  """'ZH_CN' -> 'zh-CN', 'EN_US' -> 'en-US', 'FR' -> 'fr'.

  create_string_rc.py upper-cases the .xtb lang and swaps '-' for '_';
  branding.json is keyed by the GRIT locale.
  """
  parts = rc_language.split('_')
  return '-'.join([parts[0].lower()] + parts[1:])


def make_brander(build_dir):
  """Returns brand(text, rc_language) -> str for this build, or None.

  None when the build is not a custom-branded one — the same test as
  tools/grit/grit.py: enable_custom_browser set and a branding_path_component
  other than "" / "chromium".
  """
  if not os.path.isfile(os.path.join(build_dir, 'args.gn')):
    return None
  args = _read_gn_args(build_dir)
  branding_name = args.get('branding_path_component', '')
  if (not args.get('enable_custom_browser', False)
      or branding_name in ('', 'chromium')):
    return None

  if _MODULE_DIR not in sys.path:
    sys.path.insert(0, _MODULE_DIR)
  from branding.replacer import Replacer  # pylint: disable=import-outside-toplevel
  replacer = Replacer(os.path.join(_MODULE_DIR, 'config', 'branding.json'))

  # The same display name GRIT puts in every other catalog (branding.json),
  # not branding_path_component, which names directories.
  brand_text = replacer.display_name or branding_name

  def brand(text, rc_language):
    locale = rc_language_to_locale(rc_language)
    return replacer.replace(text, locale).replace('%brand%', brand_text)

  return brand
