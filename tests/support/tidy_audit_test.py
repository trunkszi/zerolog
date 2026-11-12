"""The reflection fallback filters parser diagnostics without hiding lint or fatal failures."""
import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from scripts.cli.commands.core import _extract_non_clang_diagnostics, _tidy_failure, tool


def _failed(output, status=1, capable=False):
    return _tidy_failure(output, status, capable)[0]


class TidyAuditTest(unittest.TestCase):
    def test_versioned_tool_fallback(self):
        with patch.dict(os.environ, {}, clear=False), patch('scripts.cli.commands.core.LLVM', Path('/nonexistent-zerolog-tools')), patch('scripts.cli.commands.core.shutil.which', side_effect=lambda name: '/usr/bin/clang-tidy-23' if name == 'clang-tidy-23' else None):
            os.environ.pop('ZEROLOG_CLANG_TIDY_BIN', None)
            self.assertEqual(str(tool('clang-tidy')), '/usr/bin/clang-tidy-23')

    def test_clang_tidy_override_resolves_file_and_path_lookup(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'clang-tidy-override'
            binary.write_text('#!/bin/sh\n')
            binary.chmod(binary.stat().st_mode | stat.S_IEXEC)
            with patch.dict(os.environ, {'ZEROLOG_CLANG_TIDY_BIN': str(binary)}):
                self.assertEqual(str(tool('clang-tidy')), str(binary))
        with patch.dict(os.environ, {'ZEROLOG_CLANG_TIDY_BIN': 'clang-tidy-from-path'}), patch('scripts.cli.commands.core.shutil.which', return_value='/opt/bin/clang-tidy-from-path') as which:
            self.assertEqual(str(tool('clang-tidy')), '/opt/bin/clang-tidy-from-path')
            which.assert_called_with('clang-tidy-from-path')

    def test_clang_tidy_override_missing_fails_closed(self):
        missing = '/nonexistent/zerolog-clang-tidy-missing'
        with patch.dict(os.environ, {'ZEROLOG_CLANG_TIDY_BIN': missing}):
            with self.assertRaises(RuntimeError) as caught:
                tool('clang-tidy')
        self.assertIn(missing, str(caught.exception))

    def test_unsupported_reflection_diagnostics_are_filtered(self):
        output = 'file.cc:1:1: error: unsupported reflection ^^ [clang-diagnostic-error]\n'
        self.assertEqual(_extract_non_clang_diagnostics(output), [])
        self.assertEqual(_tidy_failure(output, 1, False), (False, ''))
        self.assertTrue(_tidy_failure(output, 1, True)[0])

    def test_lint_fatal_and_signal_failures_are_not_filtered(self):
        for output, status in [('file.cc:1: warning: bad name [readability-identifier-naming,-warnings-as-errors]', 1),
                               ('file.cc:1: fatal error: missing header', 1), ('', -11)]:
            with self.subTest(output=output):
                self.assertTrue(_failed(output, status))

    def test_seed_without_direct_reflection_marker_fails_closed(self):
        for message in ('no member named foo in uarch::detail', 'invalid zerolog::format::FormatString'):
            output = f'src/log.cc:4:2: error: {message} [clang-diagnostic-error]\n'
            with self.subTest(message=message):
                self.assertTrue(_failed(output))

    def test_seed_with_direct_reflection_marker_is_filtered(self):
        output = (
            'src/log.cc:1:1: error: reflection is only available with -freflection [clang-diagnostic-error]\n'
            'src/log.cc:4:2: error: no member named foo in uarch::detail [clang-diagnostic-error]\n'
            'src/log.cc:8:2: error: invalid zerolog::format::FormatString [clang-diagnostic-error]\n'
        )
        self.assertFalse(_failed(output))

    def test_note_linked_excess_initializer_cascade_is_filtered(self):
        output = (
            'src/log.cc:3:1: error: unsupported reflection ^^ [clang-diagnostic-error]\n'
            'include/uarch/core/simd/vec.hpp:72:29: error: excess elements in struct initializer [clang-diagnostic-excess-initializers]\n'
            'src/log.cc:3:1: note: in instantiation of member function requested here\n'
        )
        self.assertFalse(_failed(output))

    def test_unlinked_excess_initializer_fails_closed(self):
        output = (
            'src/log.cc:3:1: error: unsupported reflection ^^ [clang-diagnostic-error]\n'
            'include/other.hpp:72:29: error: excess elements in struct initializer [clang-diagnostic-excess-initializers]\n'
        )
        self.assertTrue(_failed(output))
        alone = 'include/other.hpp:72:29: error: excess elements in struct initializer [clang-diagnostic-excess-initializers]\n'
        self.assertTrue(_failed(alone))

    def test_driver_error_without_location_fails_closed(self):
        self.assertTrue(_failed('error: unable to find compilation database\n'))
        self.assertTrue(_failed('clang-tidy: error: no input files\n'))

    def test_driver_error_after_reflection_block_fails_closed(self):
        header = 'file.cc:1:1: error: unsupported reflection ^^ [clang-diagnostic-error]\n'
        for driver in ("error: unable to find compilation database", "error: unknown argument: '-freflection'"):
            with self.subTest(driver=driver):
                self.assertTrue(_failed(header + driver + '\n'))

    def test_same_file_cascade_does_not_need_fixed_regex(self):
        output = (
            'include/uarch/core/bits/bit_word.hpp:27:31: error: type name requires a specifier or qualifier ^^ [clang-diagnostic-error]\n'
            'include/uarch/core/bits/bit_word.hpp:40:8: error: type name requires a specifier or qualifier [clang-diagnostic-error]\n'
        )
        self.assertFalse(_failed(output))

    def test_annotation_token_is_a_direct_reflection_marker(self):
        output = 'include/uring/futex2/futex_word.hpp:8:9: error: expected unqualified-id [[= uarch::layout::register_leaf [clang-diagnostic-error]\n'
        self.assertFalse(_failed(output))

    def test_undeclared_identifier_outside_direct_window_fails_closed(self):
        far = (
            'file.cc:10:1: error: unsupported reflection ^^ [clang-diagnostic-error]\n'
            'file.cc:41:1: error: use of undeclared identifier \'foo\' [clang-diagnostic-error]\n'
        )
        near = (
            'file.cc:10:1: error: unsupported reflection ^^ [clang-diagnostic-error]\n'
            'file.cc:40:1: error: use of undeclared identifier \'foo\' [clang-diagnostic-error]\n'
        )
        self.assertTrue(_failed(far))
        self.assertFalse(_failed(near))

    def test_note_linked_header_cascade_without_fixed_regex_is_filtered(self):
        output = (
            'include/uring/core/proactor.hpp:12:5: error: reflection is only available here [clang-diagnostic-error]\n'
            'include/uring/config/proactor_config.hpp:261:24: error: type name requires a specifier or qualifier [clang-diagnostic-error]\n'
            'include/uring/core/proactor.hpp:80:9: note: in instantiation of member function requested here\n'
        )
        self.assertFalse(_failed(output))


if __name__ == '__main__':
    unittest.main()
