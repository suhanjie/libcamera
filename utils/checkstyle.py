#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2018, Google Inc.
#
# Author: Laurent Pinchart <laurent.pinchart@ideasonboard.com>
#
# checkstyle.py - A patch style checker script based on astyle
#
# TODO:
#
# - Support other formatting tools (clang-format, ...)
# - Split large hunks to minimize context noise
# - Improve style issues counting
#

import argparse
import difflib
import re
import shutil
import subprocess
import sys

astyle_options = (
    '-n',
    '--style=linux',
    '--indent=force-tab=8',
    '--attach-namespaces',
    '--attach-extern-c',
    '--pad-oper',
    '--align-pointer=name',
    '--align-reference=name',
    '--max-code-length=120'
)

source_extensions = (
    '.c',
    '.cpp',
    '.h'
)

class Colours:
    Default = 0
    Red = 31
    Green = 32
    Cyan = 36

for attr in Colours.__dict__.keys():
    if attr.startswith('_'):
        continue

    if sys.stdout.isatty():
        setattr(Colours, attr, '\033[0;%um' % getattr(Colours, attr))
    else:
        setattr(Colours, attr, '')


class DiffHunkSide(object):
    """A side of a diff hunk, recording line numbers"""
    def __init__(self, start):
        self.start = start
        self.touched = []
        self.untouched = []

    def __len__(self):
        return len(self.touched) + len(self.untouched)


class DiffHunk(object):
    diff_header_regex = re.compile('@@ -([0-9]+),([0-9]+) \+([0-9]+),([0-9]+) @@')

    def __init__(self, line):
        match = DiffHunk.diff_header_regex.match(line)
        if not match:
            raise RuntimeError("Malformed diff hunk header '%s'" % line)

        self.__from_line = int(match.group(1))
        self.__to_line = int(match.group(3))
        self.__from = DiffHunkSide(self.__from_line)
        self.__to = DiffHunkSide(self.__to_line)

        self.lines = []

    def __repr__(self):
        s = '%s@@ -%u,%u +%u,%u @@\n' % \
                (Colours.Cyan,
                 self.__from.start, len(self.__from),
                 self.__to.start, len(self.__to))

        for line in self.lines:
            if line[0] == '-':
                s += Colours.Red
            elif line[0] == '+':
                s += Colours.Green
            else:
                s += Colours.Default
            s += line

        s += Colours.Default
        return s

    def append(self, line):
        if line[0] == ' ':
            self.__from.untouched.append(self.__from_line)
            self.__from_line += 1
            self.__to.untouched.append(self.__to_line)
            self.__to_line += 1
        elif line[0] == '-':
            self.__from.touched.append(self.__from_line)
            self.__from_line += 1
        elif line[0] == '+':
            self.__to.touched.append(self.__to_line)
            self.__to_line += 1

        self.lines.append(line)

    def intersects(self, lines):
        for line in lines:
            if line in self.__from.touched:
                return True
        return False

    def side(self, side):
        if side == 'from':
            return self.__from
        else:
            return self.__to


def parse_diff(diff):
    hunks = []
    hunk = None
    for line in diff:
        if line.startswith('@@'):
            if hunk:
                hunks.append(hunk)
            hunk = DiffHunk(line)

        elif hunk is not None:
            hunk.append(line)

    if hunk:
        hunks.append(hunk)

    return hunks


def check_file(commit, filename):
    # Extract the line numbers touched by the commit.
    diff = subprocess.run(['git', 'diff', '%s~..%s' % (commit, commit), '--', filename],
                            stdout=subprocess.PIPE).stdout
    diff = diff.decode('utf-8').splitlines(True)
    commit_diff = parse_diff(diff)

    lines = []
    for hunk in commit_diff:
        lines.extend(hunk.side('to').touched)

    # Skip commits that don't add any line.
    if len(lines) == 0:
        return 0

    # Format the file after the commit with astyle and compute the diff between
    # the two files.
    after = subprocess.run(['git', 'show', '%s:%s' % (commit, filename)],
                           stdout=subprocess.PIPE).stdout
    formatted = subprocess.run(['astyle', *astyle_options],
                               input=after, stdout=subprocess.PIPE).stdout

    after = after.decode('utf-8').splitlines(True)
    formatted = formatted.decode('utf-8').splitlines(True)

    diff = difflib.unified_diff(after, formatted)

    # Split the diff in hunks, recording line number ranges for each hunk.
    formatted_diff = parse_diff(diff)

    # Filter out hunks that are not touched by the commit.
    formatted_diff = [hunk for hunk in formatted_diff if hunk.intersects(lines)]
    if len(formatted_diff) == 0:
        return 0

    print('%s---' % Colours.Red, filename)
    print('%s+++' % Colours.Green, filename)
    for hunk in formatted_diff:
        print(hunk)

    return len(formatted_diff)


def check_style(commit):
    # Get the commit title and list of files.
    ret = subprocess.run(['git', 'show', '--pretty=oneline','--name-only', commit],
                         stdout=subprocess.PIPE)
    files = ret.stdout.decode('utf-8').splitlines()
    title = files[0]
    files = files[1:]

    separator = '-' * len(title)
    print(separator)
    print(title)
    print(separator)

    # Filter out non C/C++ files.
    files = [f for f in files if f.endswith(source_extensions)]
    if len(files) == 0:
        print("Commit doesn't touch source files, skipping")
        return

    issues = 0
    for f in files:
        issues += check_file(commit, f)

    if issues == 0:
        print("No style issue detected")
    else:
        print('---')
        print("%u potential style %s detected, please review" % \
                (issues, 'issue' if issues == 1 else 'issues'))


def extract_revlist(revs):
    """Extract a list of commits on which to operate from a revision or revision
    range.
    """
    ret = subprocess.run(['git', 'rev-parse', revs], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)
    if ret.returncode != 0:
        print(ret.stderr.decode('utf-8').splitlines()[0])
        return []

    revlist = ret.stdout.decode('utf-8').splitlines()

    # If the revlist contains more than one item, pass it to git rev-list to list
    # each commit individually.
    if len(revlist) > 1:
        ret = subprocess.run(['git', 'rev-list', *revlist], stdout=subprocess.PIPE)
        revlist = ret.stdout.decode('utf-8').splitlines()
        revlist.reverse()

    return revlist


def main(argv):

    # Parse command line arguments
    parser = argparse.ArgumentParser()
    parser.add_argument('revision_range', type=str, default='HEAD', nargs='?',
                        help='Revision range (as defined by git rev-parse). Defaults to HEAD if not specified.')
    args = parser.parse_args(argv[1:])

    # Check for required dependencies.
    dependencies = ('astyle', 'git')

    for dependency in dependencies:
        if not shutil.which(dependency):
            print("Executable %s not found" % dependency)
            return 1

    revlist = extract_revlist(args.revision_range)

    for commit in revlist:
        check_style(commit)
        print('')


if __name__ == '__main__':
    sys.exit(main(sys.argv))