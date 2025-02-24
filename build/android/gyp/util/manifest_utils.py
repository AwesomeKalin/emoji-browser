# Copyright 2019 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Contains common helpers for working with Android manifests."""

import os
import shlex
import xml.dom.minidom as minidom

from util import build_utils
from xml.etree import ElementTree

ANDROID_NAMESPACE = 'http://schemas.android.com/apk/res/android'
TOOLS_NAMESPACE = 'http://schemas.android.com/tools'
DIST_NAMESPACE = 'http://schemas.android.com/apk/distribution'
EMPTY_ANDROID_MANIFEST_PATH = os.path.abspath(
    os.path.join(os.path.dirname(__file__), '..', '..', 'AndroidManifest.xml'))

_xml_namespace_initialized = False


def _RegisterElementTreeNamespaces():
  global _xml_namespace_initialized
  if _xml_namespace_initialized:
    return
  _xml_namespace_initialized = True
  ElementTree.register_namespace('android', ANDROID_NAMESPACE)
  ElementTree.register_namespace('tools', TOOLS_NAMESPACE)
  ElementTree.register_namespace('dist', DIST_NAMESPACE)


def ParseManifest(path):
  """Parses an AndroidManifest.xml using ElementTree.

  Registers required namespaces, creates application node if missing, adds any
  missing namespaces for 'android', 'tools' and 'dist'.

  Returns tuple of:
    doc: Root xml document.
    manifest_node: the <manifest> node.
    app_node: the <application> node.
  """
  _RegisterElementTreeNamespaces()
  doc = ElementTree.parse(path)
  # ElementTree.find does not work if the required tag is the root.
  if doc.getroot().tag == 'manifest':
    manifest_node = doc.getroot()
  else:
    manifest_node = doc.find('manifest')

  app_node = doc.find('application')
  if app_node is None:
    app_node = ElementTree.SubElement(manifest_node, 'application')

  return doc, manifest_node, app_node


def MakeElement(name, attributes=None):
  _RegisterElementTreeNamespaces()
  element = ElementTree.Element(name)
  if attributes:
    for attribute_name, attribute_val in attributes:
      if attribute_name.startswith('android:'):
        attribute_name = '{%s}%s' % (
            ANDROID_NAMESPACE, attribute_name[attribute_name.find(':') + 1:])
      element.set(attribute_name, attribute_val)
  return element


def SaveManifest(doc, path):
  with build_utils.AtomicOutput(path) as f:
    f.write(ElementTree.tostring(doc.getroot(), encoding='UTF-8'))


def GetPackage(manifest_node):
  return manifest_node.get('package')


def AssertNoUsesSdk(manifest_node):
  """Asserts that manifest has no <uses-sdk> element."""
  assert manifest_node.find('./uses-sdk') is None, 'Must define uses-sdk in GN'


def _SortAndStripElementTree(tree, reverse_toplevel=False):
  for node in tree:
    if node.text and node.text.isspace():
      node.text = None
    _SortAndStripElementTree(node)
  tree[:] = sorted(tree, key=ElementTree.tostring, reverse=reverse_toplevel)


def NormalizeManifest(path):
  with open(path) as f:
    # This also strips comments and sorts node attributes alphabetically.
    root = ElementTree.fromstring(f.read())

  # Sort nodes alphabetically, recursively.
  _SortAndStripElementTree(root, reverse_toplevel=True)

  # Fix up whitespace/indentation.
  dom = minidom.parseString(ElementTree.tostring(root))
  lines = []
  for l in dom.toprettyxml(indent='  ').splitlines():
    if l.strip():
      if len(l) > 100:
        indent = ' ' * l.find('<')
        attributes = shlex.split(l, posix=False)
        lines.append('{}{}'.format(indent, attributes[0]))
        for attribute in attributes[1:]:
          lines.append('{}    {}'.format(indent, attribute))
      else:
        lines.append(l)

  return '\n'.join(lines)
