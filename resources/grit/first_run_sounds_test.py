# Copyright 2026 VellocLabs
# Use of this source code is governed by the Apache License, Version 2.0,
# which can be found in the LICENSE file.
"""Checks that the first-run sounds stay out of a custom_browser build.

The FLAC files behind IDR_INTRO_SOUND_* are only played by the first-run
window, which Velloc never shows; they are ~4.8 MB of resources.pak.

Run: python custom_browser/resources/grit/first_run_sounds_test.py
"""

import os
import re
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_SRC = os.path.normpath(os.path.join(_HERE, '..', '..', '..'))
sys.path.insert(0, os.path.join(_SRC, 'tools', 'grit'))
from grit.node import base  # pylint: disable=wrong-import-position

_GRD = os.path.join(_SRC, 'chrome', 'browser', 'browser_resources.grd')
_BROWSER_GN = os.path.join(_SRC, 'chrome', 'browser', 'BUILD.gn')
_CONTROLLER = os.path.join(_SRC, 'chrome', 'browser', 'ui', 'views',
                           'profiles', 'first_run_flow_controller.cc')


def _read(path):
  with open(path, encoding='utf-8') as f:
    return f.read()


def _sounds_condition():
  """The <if expr> wrapping the IDR_INTRO_SOUND_* includes."""
  grd = _read(_GRD)
  first = grd.index('IDR_INTRO_SOUND_')
  opening = list(re.finditer(r'<if expr="([^"]*)">', grd[:first]))[-1]
  return opening.group(1).replace('&quot;', '"')


def _packaged(defines):
  return base.Node.EvaluateExpression(_sounds_condition(), defines, 'win32')


class FirstRunSoundsTest(unittest.TestCase):

  def test_custom_browser_build_drops_the_sounds(self):
    self.assertFalse(_packaged({'enable_custom_browser': True}))

  def test_upstream_build_keeps_the_sounds(self):
    self.assertTrue(_packaged({}))

  def test_grit_target_defines_enable_custom_browser(self):
    gn = _read(_BROWSER_GN)
    target = re.search(r'^grit\("resources"\) \{\n(.*?)^\}', gn,
                       re.MULTILINE | re.DOTALL).group(1)
    self.assertRegex(
        target, r'if \(enable_custom_browser\) \{\s*'
        r'defines \+= \[ "enable_custom_browser" \]\s*\}')

  def test_controller_references_the_sounds_only_upstream(self):
    source = _read(_CONTROLLER)
    guard = source.index('#if !BUILDFLAG(ENABLE_CUSTOM_BROWSER)')
    end = source.index('#endif  // !BUILDFLAG(ENABLE_CUSTOM_BROWSER)', guard)
    for match in re.finditer(r'IDR_INTRO_SOUND_\w+', source):
      self.assertTrue(guard < match.start() < end, match.group(0))


if __name__ == '__main__':
  unittest.main()
