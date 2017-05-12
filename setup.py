#! /usr/bin/env python

from setuptools import setup, Extension
from sys import platform, prefix, executable, exit
from subprocess import Popen, PIPE
from os.path import dirname, exists, join, normpath
from tempfile import mkstemp
from os import write, close, remove, environ

import logging
import re

ON_WINDOWS = platform == 'win32'
VERSION = '0.12.2'

include_dirs = []
lib_dirs = []

compilation_search_paths = [
    dirname(executable),
    dirname(dirname(executable)),
]

if ON_WINDOWS:
    compilation_search_paths.append(environ.get("FREETDS", "c:\\freetds"))

for _path in compilation_search_paths:
    include_path = join(_path, "include")
    lib_path = join(_path, "lib")

    if exists(lib_path):
        lib_dirs[:0] = [lib_path]

    if exists(include_path):
        include_dirs[:0] = [include_path]

def extract_constants(freetds_include="sybdb.h", constants_file="bcp_constants.py"):
    """ Extract constant names from sybdb.h to use as python constants """
    fileno, source_file = mkstemp(suffix=".c")
    write(fileno, "#include <{}>".format(freetds_include))
    close(fileno)

    fileno, include_directives = mkstemp(suffix=".txt")
    close(fileno)

    if ON_WINDOWS:
        cmd_template = "cl /E {includes} {source} > {output}"
    else:
        cmd_template = "cpp {includes} '{source}' > '{output}'"

    cmd = cmd_template.format(
        output=normpath(include_directives),
        source=normpath(source_file),

        includes=" ".join(
            "-I{}".format(normpath(_include)) for _include in include_dirs
        )
    )

    fifo = Popen(cmd, shell=True, stdin=None, stdout=None, stderr=None, close_fds=True)
    fifo.communicate()
    fifo.wait()

    remove(source_file)

    if fifo.returncode < 0:
        raise Exception("Cannot run preprocessor step")

    row_regex = re.compile('[\r\n]+')
    field_regex = re.compile('[\s]+')

    with open(include_directives, "r") as fd:
        include_paths = list(
            _filename
            for contents in [fd.read()]
            for _row in row_regex.split(contents) if _row.find(freetds_include) > -1
            for _index, _word in enumerate(field_regex.split(_row)) if _index == 2
            for _filename in [_word.strip('"')] if exists(_filename)
        )

    remove(include_directives)

    for include_file in include_paths:
        with open(include_file, "r") as fd:
            definition_pairs = [
                (_values[1], int(_values[2])) 
                for contents in [fd.read()]
                for _row in row_regex.split(contents)
                for _values in [field_regex.split(_row)] if len(_values) == 3 and _values[0] == "#define" and _values[2].isdigit()
            ]

        if len(definition_pairs):
            with open(constants_file, "w") as output_fd:
                print >> output_fd, "\n".join("%s=%d" % _row for _row in definition_pairs)

            break
    else:
        raise Exception("Couldn't find a freetds include file")

extract_constants(
    freetds_include="sybdb.h",
    constants_file="bcp_constants.py",
)

if ON_WINDOWS:
    bcp_module = Extension(
        'bcp',
        sources = ['pythonbcp.c'],
        include_dirs = include_dirs,
        library_dirs = lib_dirs,
        libraries = ['sybdb'],
    )

    setup(
        name = 'bcp',
        version = VERSION,
        description = 'This package supports bulk transfers to MS SQL and Sybase databases',
        # data_files = [(prefix, ['win32/FreeTDS.dll', 'win32/dblib.dll'])],
        ext_modules = [bcp_module],
        py_modules = ['bcp_constants'],
    )
else:
    bcp_module = Extension(
        'bcp',
        sources = ['pythonbcp.c'],
        include_dirs = include_dirs,
        library_dirs = lib_dirs,
        libraries = ['sybdb', 'iconv'],
        # extra_compile_args=['-m32', '-march=i386'],
        # extra_link_args=['-m32', '-march=i386'],
    )

    setup(
        name = 'bcp',
        version = VERSION,
        description = 'This package supports bulk transfers to MS SQL and Sybase databases',
        ext_modules = [bcp_module],
        py_modules = ['bcp_constants'],
    )
