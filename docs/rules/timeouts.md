Search documentation
Ctrl K
Browse documentation
Timeouts
Local runs

unswbc run runs bots as ordinary processes on your machine and places no limit on how much work a turn does. The only limit is a wall clock of 10 seconds per turn. A bot that reaches it, or that exits during a turn, delivers nothing for that turn, so its dragon dies, and the bot is restarted as a new process for the dragons it still controls. As a result, a local match says nothing about whether a bot fits within the judge's budget below.

unswbc run --sandbox runs bots in the judge's sandbox instead, priced in CPU points exactly as the judge prices them. Python bots run on the judge's own interpreter, C and C++ sources are compiled to wasm by the judge's own clang with the judge's flags, and a bot already compiled to wasm runs there too: name a directory holding main.wasm, or the file itself. With -v it prints the points and memory each turn used.

Judge limits

The judge runs bots in a WebAssembly sandbox and measures work in CPU points rather than time. Each instruction has a fixed price, so load on the judge cannot affect results.

Limit	Value
Points per dragon per turn	100 million
Memory per dragon	48 MB
Threads per dragon	1 (thread_spawn is denied)
Backstop	1 s of CPU, 10 s wall; only catches a bot that evades the meter

A turn that exhausts its points is stopped, delivers no reply, and the dragon dies that turn, so leave a margin. The first turn is not exempt: the interpreter and NumPy are preloaded, but your own imports and setup are charged. Spread heavy precomputation across turns or perform it lazily.

A turn's budget runs from reading the round to the flush in end_turn(), and that is the figure -v reports. Work done after end_turn(), before the next round is read, is charged to the turn that follows it, so precomputing for the next turn there costs the same as doing it at the top of that turn.

Instruction prices

This is the whole price list. Anything not named in it costs 2 points.

Instruction	Points
Constants, local.get/set/tee, global.get/set, select, nop, drop, block, loop, end, else, unreachable	1
Comparisons, integer and float	1
Integer and float arithmetic: add, subtract, multiply, bitwise, shifts, rotates, clz, ctz, popcnt, abs, neg, ceil, floor, trunc, nearest, sqrt, min, max, copysign	1
Conversions: wrap, extend, truncate, convert, promote, demote, reinterpret	1
ref.null, ref.is_null, ref.func	1
Loads and stores, of every width	2
memory.size	2
br, br_if, if, return	2
Everything else, including every 128-bit SIMD instruction	2
Division and remainder, integer and float	3
br_table	3
call	4
call_indirect, call_ref, return_call, return_call_indirect, return_call_ref	6
memory.copy, memory.fill, memory.init	10, plus 1 per 8 bytes
table.get/set/size/copy/fill/init, data.drop, elem.drop	10
memory.grow, table.grow	50

Work the judge does for you is charged to the same meter.

Call	Points
A write to stdout or stderr	2,500,000, plus 4,000 per byte
Reading from stdin or a file	6 per byte
Drawing random bytes	40,000, plus 3 per byte
Opening a file or looking a path up	40,000

For reference, parsing one round in the Python helper costs about 10 million points, and a flood fill of a 32×32 map costs about 19 million in Python and about 0.3 million in C++.

Output

Each write costs 2.5 million points plus 4,000 per byte, and stderr is charged like stdout. The helpers flush once per turn, in end_turn(), and reading the next turn also flushes. A bot that flushes after every line pays 2.5 million points per line, so keep logging light and do not add flushes.

The judge's stdout looks like a terminal (isatty returns true), so it is line buffered until something asks otherwise, and every line you print is then its own write. The C and C++ helpers ask otherwise: they call setvbuf for a full buffer in init, so a whole turn's output leaves in the one write that end_turn flushes. Ten log lines cost 2.5 million points that way, against 25 million a line at a time.

So in C and C++, do not set std::unitbuf, do not call sync_with_stdio(false), which gives std::cout a buffer of its own outside the one the helper set, and do not call cin.tie(nullptr) unless you flush at ENDTURN yourself. Use std::clog rather than std::cerr for debugging output. Python needs none of this: its stdout is fully buffered already.

The judge keeps up to 500,000 log, indicator and drawing lines per team per game, and up to 16 MB of their text. Past either, the rest of that team's output is dropped and the replay says so once. An indicator is cut to 512 characters. unswbc run keeps everything unless you pass --sandbox.

Languages

C and C++ are compiled with -O2 -msimd128 against the C standard library and libc++, with the zip's own directory on the include path. __DATE__, __TIME__ and __TIMESTAMP__ are errors, so a bot is the same whenever it is built. No other libraries are available, so any you need must be vendored as source. A SIMD instruction costs 2 points and executes as real SIMD, so 4 or 16 lanes of work cost about what two scalar instructions do. Python is CPython 3.13 with NumPy 2.5 and nothing else; pure-Python dependencies must be vendored. The sandbox drops the parts of both standard libraries that need a network, a second thread or a writable disk, which Standard library lists.

Determinism

Games are reproducible. Random numbers come from a generator seeded per dragon and per game, and the clock advances with points spent, so time.sleep costs no real time and timing your own code measures points.

Each game's seed decides when pearls respawn and what every dragon draws, and its replay shows it. Running unswbc run map a b --sandbox --seed <seed> with the same two bots plays the same game on your machine. A dragon only gets its own numbers if it asks the sandbox for them. C's rand() without srand and a default-constructed std::mt19937 start from a fixed seed, and srand(time(NULL)) reads a clock that starts at the same moment for every dragon, so all three give every dragon the same sequence.

The budget is per turn, not per tile

MOVE NNE covers three tiles with one decision, so a deeper search can commit to several steps at once, paying in segments rather than points.

Previous
Execution Order
Next
Standard Library