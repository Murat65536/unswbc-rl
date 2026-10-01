Search documentation
Ctrl K
Browse documentation
Standard library

The judge runs your bot in a WebAssembly sandbox with no network, no child processes and no writable disk. Modules that only exist to reach one of those are not shipped at all, so they fail at import rather than at the call. Everything else is present. Timeouts covers what the sandbox charges you for.

Versions

Two things run your bot: the judge, and your own machine when you unswbc run. The judge is the one that scores you.

Language	In the judge	On your machine
Python	CPython 3.13, with NumPy 2.5.3	the first of python3, python, py -3 that runs
C	Clang 20 as C17, against wasi-libc	the first of cc, gcc, clang, cl
C++	Clang 20 as C++20, against libc++	the first of c++, g++, clang++, cl

Both sides compile at -std=c17 or -std=c++20, and -O2. The judge adds -msimd128, puts your zip's own directory on the include path, and fails a build that uses __DATE__, __TIME__ or __TIMESTAMP__. Set UNSWBC_PYTHON, CC or CXX to choose the tool a local run uses, and unswbc on its own reports what it found. The toolkit itself needs Python 3.11 or newer, whichever language your bot is in.

A local run uses your interpreter, not the judge's, so a bot that relies on something added in 3.13 can pass in the judge and fail on your machine. bot.toml names the language as py, c or c++, and that is what the judge builds it as.

Python

CPython 3.13 and NumPy 2.5.3, and nothing else. Third-party packages cannot be installed; a pure-Python dependency has to be vendored into your zip. These modules import:

Raw
Copy Code
__future__  abc  argparse  array  ast  atexit  base64  bdb  binascii  bisect  builtins
calendar  cmath  cmd  code  codecs  codeop  collections  colorsys  compileall  configparser  contextlib
contextvars  copy  copyreg  cProfile  csv  ctypes  dataclasses  datetime  decimal  difflib  dis  doctest
encodings  enum  errno  faulthandler  fcntl  filecmp  fileinput  fnmatch  fractions  functools  gc  getopt
getpass  gettext  glob  graphlib  hashlib  heapq  hmac  importlib  inspect  io  itertools  json  keyword
linecache  locale  logging  marshal  math  mmap  modulefinder  netrc  numbers  opcode  operator  optparse  os
pathlib  pdb  pickle  pickletools  pkgutil  platform  posix  posixpath  pprint  profile  pstats  pty  pwd
py_compile  pyclbr  queue  quopri  random  re  reprlib  rlcompleter  runpy  sched  secrets  select
selectors  shlex  shutil  signal  site  stat  statistics  string  stringprep  struct  symtable  sys  sysconfig
tabnanny  tarfile  tempfile  termios  textwrap  time  timeit  token  tokenize  tomllib  trace  traceback
tracemalloc  tty  types  typing  unicodedata  unittest  uuid  warnings  wave  weakref  zipapp  zipfile
zipimport  zoneinfo

Names starting with an underscore are omitted above but behave as they do in any CPython: _thread, _pyrepl, _collections_abc and the rest are all there.

Not shipped
Reason	Modules
No network	socket, socketserver, ssl, http, urllib, xmlrpc, ftplib, smtplib, poplib, imaplib, telnetlib, wsgiref, ipaddress, email, cgi, cgitb, webbrowser
One thread, one process	asyncio, multiprocessing, concurrent.futures
Compression libraries not built	zlib, bz2, lzma, gzip
Parsers and stores not built	xml (all of it), sqlite3, dbm, plistlib, mimetypes, mailbox, importlib.metadata
No terminal, no display	curses, readline, tkinter, turtle
Packaging and test tooling	ensurepip, venv, distutils, lib2to3, test, idlelib
Present, but will not work

These ship, and cannot do the thing you would reach for them for.

Module	What happens
threading	Thread.start() raises RuntimeError: can't start new thread. The lock and event types work, and a timed acquire returns at once, since the clock is virtual.
subprocess	Popen raises OSError 58, ENOTSUP. So does os.fork.
shelve	open fails, because dbm is not shipped.
pydoc	Will not import at all: it reaches for urllib on the way in.

zipfile and tarfile import, but without zlib only stored members read; anything deflated, gzipped or xz'd raises.

C and C++

Clang 20 against wasi-libc and libc++, as C17 or C++20. Every libc++ header is present except <generator>, and so is every POSIX header the sysroot carries, including <sys/socket.h>, <pthread.h> and <spawn.h>. Those compile and link; the calls behind them fail at runtime. socket() returns ENOTSUP, std::thread throws Resource temporarily unavailable, and std::system returns −1. There are no third-party libraries, so anything you need must be vendored as source.

What both languages see
Thing	In the sandbox
Network	Every socket call returns ENOTSUP.
Processes and threads	fork, exec, posix_spawn and thread creation are all denied.
Filesystem	Read-only. You get your own files at /bot, which is the working directory, and the interpreter or sysroot. There is no writable /tmp.
Environment	Five PYTHON* variables and TERM=dumb. getpid() is 1 and os.cpu_count() is 1.
Time

Nothing about time is disabled, but the clock is the judge's, not the machine's. It reads 2026-01-01T00:00:00Z when your dragon has spent nothing, and advances one nanosecond per CPU point spent, plus whatever you asked to sleep.

That makes time.perf_counter and std::chrono::steady_clock budget meters: a turn's 100 million points read as 0.1 seconds, so the difference across a piece of your code is the points it cost, in nanoseconds. A sleep returns immediately and moves the clock by what you asked for, which is why timing across one is meaningless. Two runs of the same match give the same readings.

Randomness

Also the judge's. os.urandom, random, std::random_device and std::mt19937 seeded from it all draw on one stream per dragon per game, so a replayed match draws the same numbers. std::random_device::entropy() reports 0, which is honest. PYTHONHASHSEED is 0, so hash() and set and dict ordering are stable too. Seed your own generator if you want your dragons to differ from each other.

Previous
Timeouts
Next
Helper Reference