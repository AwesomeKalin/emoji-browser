#!/usr/bin/env vpython
# Copyright 2019 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import json
import os
import subprocess
import sys
import unittest

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(
    0, os.path.abspath(os.path.join(THIS_DIR, os.pardir, os.pardir, os.pardir,
                                    'third_party', 'pymock')))

import mock

import merge_results
import merge_steps
import merge_lib as merger


class MergeProfilesTest(unittest.TestCase):

  def __init__(self, *args, **kwargs):
    super(MergeProfilesTest, self).__init__(*args, **kwargs)
    self.maxDiff = None

  def test_merge_script_api_parameters(self):
    """Test the step-level merge front-end."""
    build_properties = json.dumps({
        'some': {
            'complicated': ['nested', {
                'json': None,
                'object': 'thing',
            }]
        }
    })
    task_output_dir = 'some/task/output/dir'
    profdata_dir = '/some/different/path/to/profdata/default.profdata'
    profdata_file = os.path.join(profdata_dir, 'default.profdata')
    args = [
        'script_name', '--output-json', 'output.json', '--build-properties',
        build_properties, '--summary-json', 'summary.json', '--task-output-dir',
        task_output_dir, '--profdata-dir', profdata_dir, '--llvm-profdata',
        'llvm-profdata', 'a.json', 'b.json', 'c.json'
    ]
    with mock.patch.object(merger, 'merge_profiles') as mock_merge:
      mock_merge.return_value = None
      with mock.patch.object(sys, 'argv', args):
        merge_results.main()
        self.assertEqual(
            mock_merge.call_args,
            mock.call(task_output_dir, profdata_file, '.profraw',
                      'llvm-profdata'))

  def test_merge_steps_parameters(self):
    """Test the build-level merge front-end."""
    input_dir = 'some/task/output/dir'
    output_file = '/some/different/path/to/profdata/merged.profdata'
    args = [
        'script_name',
        '--input-dir',
        input_dir,
        '--output-file',
        output_file,
        '--llvm-profdata',
        'llvm-profdata',
    ]
    with mock.patch.object(merger, 'merge_profiles') as mock_merge:
      mock_merge.return_value = None
      with mock.patch.object(sys, 'argv', args):
        merge_steps.main()
        self.assertEqual(
            mock_merge.call_args,
            mock.call(input_dir, output_file, '.profdata', 'llvm-profdata'))

  @mock.patch.object(merger, '_validate_and_convert_profraws')
  def test_merge_profraw(self, mock_validate_and_convert_profraws):
    mock_input_dir_walk = [
        ('/b/some/path', ['0', '1', '2', '3'], ['summary.json']),
        ('/b/some/path/0', [],
         ['output.json', 'default-1.profraw', 'default-2.profraw']),
        ('/b/some/path/1', [],
         ['output.json', 'default-1.profraw', 'default-2.profraw']),
    ]

    mock_validate_and_convert_profraws.return_value = [
        '/b/some/path/0/default-1.profdata',
        '/b/some/path/1/default-2.profdata',
    ], [
        '/b/some/path/0/default-2.profraw',
        '/b/some/path/1/default-1.profraw',
    ]

    with mock.patch.object(os, 'walk') as mock_walk:
      with mock.patch.object(os, 'remove'):
        mock_walk.return_value = mock_input_dir_walk
        with mock.patch.object(subprocess, 'check_output') as mock_exec_cmd:
          merger.merge_profiles('/b/some/path', 'output/dir/default.profdata',
                                '.profraw', 'llvm-profdata')
          self.assertEqual(
              mock.call(
                  [
                      'llvm-profdata',
                      'merge',
                      '-o',
                      'output/dir/default.profdata',
                      '-sparse=true',
                      '/b/some/path/0/default-1.profdata',
                      '/b/some/path/1/default-2.profdata',
                  ],
                  stderr=-2,
              ), mock_exec_cmd.call_args)

    self.assertTrue(mock_validate_and_convert_profraws.called)


  def test_merge_profraw_skip_if_there_is_no_file(self):
    mock_input_dir_walk = [
        ('/b/some/path', ['0', '1', '2', '3'], ['summary.json']),
    ]

    with mock.patch.object(os, 'walk') as mock_walk:
      mock_walk.return_value = mock_input_dir_walk
      with mock.patch.object(subprocess, 'check_output') as mock_exec_cmd:
        merger.merge_profiles('/b/some/path', 'output/dir/default.profdata',
                              '.profraw', 'llvm-profdata')
        self.assertFalse(mock_exec_cmd.called)


  @mock.patch.object(merger, '_validate_and_convert_profraws')
  def test_merge_profdata(self, mock_validate_and_convert_profraws):
    mock_input_dir_walk = [
        ('/b/some/path', ['base_unittests', 'url_unittests'], ['summary.json']),
        ('/b/some/path/base_unittests', [], ['output.json',
                                             'default.profdata']),
        ('/b/some/path/url_unittests', [], ['output.json', 'default.profdata']),
    ]
    with mock.patch.object(os, 'walk') as mock_walk:
      with mock.patch.object(os, 'remove'):
        mock_walk.return_value = mock_input_dir_walk
        with mock.patch.object(subprocess, 'check_output') as mock_exec_cmd:
          merger.merge_profiles('/b/some/path', 'output/dir/default.profdata',
                                '.profdata', 'llvm-profdata')
          self.assertEqual(
              mock.call(
                  [
                      'llvm-profdata',
                      'merge',
                      '-o',
                      'output/dir/default.profdata',
                      '-sparse=true',
                      '/b/some/path/base_unittests/default.profdata',
                      '/b/some/path/url_unittests/default.profdata',
                  ],
                  stderr=-2,
              ), mock_exec_cmd.call_args)

    # The mock method should only apply when merging .profraw files.
    self.assertFalse(mock_validate_and_convert_profraws.called)

  def test_retry_profdata_merge_failures(self):
    mock_input_dir_walk = [
        ('/b/some/path', ['0', '1'], ['summary.json']),
        ('/b/some/path/0', [],
         ['output.json', 'default-1.profdata', 'default-2.profdata']),
        ('/b/some/path/1', [],
         ['output.json', 'default-1.profdata', 'default-2.profdata']),
    ]
    with mock.patch.object(os, 'walk') as mock_walk:
      with mock.patch.object(os, 'remove'):
        mock_walk.return_value = mock_input_dir_walk
        with mock.patch.object(subprocess, 'check_output') as mock_exec_cmd:
          invalid_profiles_msg = (
              'error: /b/some/path/0/default-1.profdata: Malformed '
              'instrumentation profile data.')

          # Failed on the first merge, but succeed on the second attempt.
          mock_exec_cmd.side_effect = [
              subprocess.CalledProcessError(
                  returncode=1, cmd='dummy cmd', output=invalid_profiles_msg),
              None
          ]

          merger.merge_profiles('/b/some/path', 'output/dir/default.profdata',
                                '.profdata', 'llvm-profdata')

          self.assertEqual(2, mock_exec_cmd.call_count)

          # Note that in the second call, /b/some/path/0/default-1.profdata is
          # excluded!
          self.assertEqual(
              mock.call(
                  [
                      'llvm-profdata',
                      'merge',
                      '-o',
                      'output/dir/default.profdata',
                      '-sparse=true',
                      '/b/some/path/0/default-2.profdata',
                      '/b/some/path/1/default-1.profdata',
                      '/b/some/path/1/default-2.profdata',
                  ],
                  stderr=-2,
              ), mock_exec_cmd.call_args)

  @mock.patch('__builtin__.open',
              new_callable=mock.mock_open,
              read_data=json.dumps(
                  {'shards': [{'task_id': '1234567890abcdeff'}]}))
  def test_mark_invalid_shards(self, mo):
    mock_result = mock.mock_open()
    mock_write = mock.MagicMock()
    mock_result.return_value.write = mock_write
    mo.side_effect = (
        mo.return_value,
        mock.mock_open(read_data='{}').return_value,
        mock_result.return_value,
    )
    merge_results.mark_invalid_shards(['1234567890abcdeff'], 'dummy.json',
                                      'o.json')
    written = json.loads(''.join(c[0][0] for c in mock_write.call_args_list))
    self.assertIn('missing_shards', written)
    self.assertEqual(written['missing_shards'], [0])


if __name__ == '__main__':
  unittest.main()
