# coding: utf-8

# Copyright 2018 Masamitsu MURASE
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

import argparse
import sys
import pyflakes.api
import pycodestyle

__author__ = "Masamitsu MURASE"
__copyright__ = "Copyright 2018, Masamitsu MURASE"
__credits__ = ["Masamitsu MURASE"]
__license__ = "MIT"
__version__ = "1.2.0"


class PyflakesReporter(object):
    """
    Formats the results of pyflakes checks to users.
    """

    def __init__(self):
        self.unexpected_errors = []
        self.syntax_errors = []
        self.flake_errors = []

    def unexpectedError(self, filename, msg):
        """
        An unexpected error occurred trying to process C{filename}.

        @param filename: The path to a file that we could not process.
        @ptype filename: C{unicode}
        @param msg: A message explaining the problem.
        @ptype msg: C{unicode}
        """
        self.unexpected_errors.append((filename, msg))

    def syntaxError(self, filename, msg, lineno, offset, text):
        """
        There was a syntax error in C{filename}.

        @param filename: The path to the file with the syntax error.
        @ptype filename: C{unicode}
        @param msg: An explanation of the syntax error.
        @ptype msg: C{unicode}
        @param lineno: The line number where the syntax error occurred.
        @ptype lineno: C{int}
        @param offset: The column on which the syntax error occurred, or None.
        @ptype offset: C{int}
        @param text: The source code containing the syntax error.
        @ptype text: C{unicode}
        """
        line = text.splitlines()[-1]
        if offset is not None:
            offset = offset - (len(text) - len(line))
        self.syntax_errors.append((filename, msg, lineno, offset, line))

    def flake(self, message):
        """
        pyflakes found something wrong with the code.

        @param: A L{pyflakes.messages.Message}.
        """
        self.flake_errors.append(message)


class dotdict(dict):
    """dot.notation access to dictionary attributes"""
    __getattr__ = dict.get
    __setattr__ = dict.__setitem__
    __delattr__ = dict.__delitem__


class PycodestyleReport(pycodestyle.BaseReport):
    def __init__(self):
        def ignore_code(code):
            return False

        options = {
            "benchmark_keys": pycodestyle.BENCHMARK_KEYS,
            "ignore_code": ignore_code
        }
        super(PycodestyleReport, self).__init__(dotdict(options))

        self.errors_list = []

    def error(self, line_number, offset, text, check):
        """Report an error, according to options."""
        code = super(PycodestyleReport, self).error(line_number, offset,
                                                    text, check)
        if code:
            self.errors_list.append((line_number, offset, code, text[5:]))
        return code


class Flake8Result(object):
    FLAKE8_PYFLAKES_CODES = {
        'UnusedImport': 'F401',
        'ImportShadowedByLoopVar': 'F402',
        'ImportStarUsed': 'F403',
        'LateFutureImport': 'F404',
        'ImportStarUsage': 'F405',
        'ImportStarNotPermitted': 'F406',
        'FutureFeatureNotDefined': 'F407',
        'MultiValueRepeatedKeyLiteral': 'F601',
        'MultiValueRepeatedKeyVariable': 'F602',
        'TooManyExpressionsInStarredAssignment': 'F621',
        'TwoStarredExpressions': 'F622',
        'AssertTuple': 'F631',
        'BreakOutsideLoop': 'F701',
        'ContinueOutsideLoop': 'F702',
        'ContinueInFinally': 'F703',
        'YieldOutsideFunction': 'F704',
        'ReturnWithArgsInsideGenerator': 'F705',
        'ReturnOutsideFunction': 'F706',
        'DefaultExceptNotLast': 'F707',
        'DoctestSyntaxError': 'F721',
        'RedefinedWhileUnused': 'F811',
        'RedefinedInListComp': 'F812',
        'UndefinedName': 'F821',
        'UndefinedExport': 'F822',
        'UndefinedLocal': 'F823',
        'DuplicateArgument': 'F831',
        'UnusedVariable': 'F841',
    }

    def __init__(self, code, filename, lineno, offset, text):
        self.code = code
        self.filename = filename
        self.lineno = lineno
        self.offset = offset
        self.text = text

    def __str__(self):
        return "%s:%d:%d: %s %s" % (self.filename, self.lineno, self.offset,
                                    self.code, self.text)

    def sort_key(self):
        return (self.lineno, self.offset, self.code, self.text)

    def __lt__(self, other):
        return self.sort_key() < other.sort_key()

    @classmethod
    def from_pycodestyle_error(cls, error, filename):
        return cls(error[2], filename, error[0], error[1] + 1, error[3])

    @classmethod
    def from_pyflakes_flake_error(cls, error):
        code = cls.FLAKE8_PYFLAKES_CODES.get(error.__class__.__name__, "F999")
        return cls(code, error.filename, error.lineno,
                   error.col + 1, str(error))

    @classmethod
    def from_pyflakes_unexpected_error(cls, error):
        code = "F000"
        return cls(code, error[0], 1, 1, error[1])

    @classmethod
    def from_pyflakes_syntax_error(cls, error):
        code = "F001"
        return cls(code, error[0], error[2], error[3] + 1, error[1])


def check_pyflakes(filename):
    reporter = PyflakesReporter()
    pyflakes.api.checkPath(filename, reporter)
    a = [Flake8Result.from_pyflakes_unexpected_error(x)
         for x in reporter.unexpected_errors]
    b = [Flake8Result.from_pyflakes_syntax_error(x)
         for x in reporter.syntax_errors]
    c = [Flake8Result.from_pyflakes_flake_error(x)
         for x in reporter.flake_errors]
    return a + b + c


def check_pycodestyle(filename):
    report = PycodestyleReport()
    pycodestyle.Checker(filename, report=report).check_all()
    return [Flake8Result.from_pycodestyle_error(x, filename)
            for x in report.errors_list]


def usage():
    return "python -m flake8_lite [--ignore=E302,E305] [--format=%(filename)s:%(row)d:%(col)d: %(code)s %(text)s] input.py"


def main():
    parser = argparse.ArgumentParser(usage=usage().replace("%", "%%"))
    parser.add_argument("--format", type=str, help="Format of output message",
        default="%(filename)s:%(row)d:%(col)d: %(code)s %(text)s")
    parser.add_argument("--ignore", type=str, help="Ignored errors: e.g. E302,E305",
        default="")
    parser.add_argument("filename")
    args = parser.parse_args()

    output_format = args.format
    ignore_list = args.ignore.split(",")
    filename = args.filename

    result_pyflakes = check_pyflakes(filename)
    result_pycodestyle = check_pycodestyle(filename)

    all_results = sorted(result_pyflakes + result_pycodestyle)
    filtered_results = list(filter(lambda x: (x.code not in ignore_list),
                                   all_results))
    for i in filtered_results:
        print(output_format % {"filename": i.filename, "row": i.lineno,
                               "col": i.offset, "code": i.code,
                               "text": i.text})

    if len(filtered_results) > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
