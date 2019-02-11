# Python BCP

## Overview

**Python BCP** is a python module that facilitates direct BCP data transfers to a SQL Server database

## Build requirements

FreeTDS is required, as are the Python development headers and libraries.

Running setup.py works as you'd normally expect

### Windows

On Windows, Anaconda is recommended

See:

- [Building Packages with Anaconda](https://github.com/ReactionMechanismGenerator/RMG-Py/wiki/Creating-Anaconda-Binary-Packages) and [...the build tutorial](https://conda.io/docs/build_tutorials/windows.html)
- [Installing FreeTDS](http://www.freetds.org/userguide/osissues.htm#WINDOWS)
- [Binaries](https://github.com/ramiro/freetds/releases)

## TODO

* Test suite
* OSX build

http://python3porting.com/cextensions.html
for iconv: apt install libc6-dev

ldd -r build/lib.linux-x86_64-3.7/bcp.cpython-37m-x86_64-linux-gnu.so 

