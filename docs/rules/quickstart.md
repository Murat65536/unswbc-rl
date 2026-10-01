Search documentation
Ctrl K
Browse documentation
Quickstart

For beginners, we recommend using Python to write your bots. Make sure you have Python preinstalled on your machine.

Install the toolkit

The toolkit, unswbc, is a Python package. If you have uv, install it as a tool. Upgrade later with uv tool upgrade unswbc.

Raw
Copy Code
uv tool install unswbc

If you do not have uv, install it with pip instead. You need Python 3.11 or newer, and the command is then called the same way. Should pip refuse and say externally-managed-environment, as Debian, Ubuntu and Homebrew do, install uv and use the line above.

Raw
Copy Code
pip install unswbc
Check your machine

Run unswbc on its own. It lists the commands and reports what it found here: a Python interpreter, the C and C++ compilers, and the replay viewer. Anything missing is named with the step that installs it.

Raw
Copy Code
unswbc
Create a bot

unswbc init creates a bot project in Python, C or C++. A Python project consists of main.py (your bot), helper.py (the library that reads and writes the engine protocol) and bot.toml (the language and the list of files to ship). C and C++ projects contain helper.h and helper.c, or helper.hpp, in place of helper.py. Maps go into a maps/ folder beside the bot folder, which all your bots share.

Python
C
C++
Copy Code
unswbc init python mybot

After upgrading the toolkit later, unswbc maps adds any newly bundled maps to that folder.

Run a match

unswbc run takes a map and two bot projects, rebuilds both bots, plays the match and writes a .replay file. The two bots may be the same project. arena.map is an 11×11 map to start on.

Raw
Copy Code
unswbc run maps/arena.map mybot mybot
Open the replay

If you use VS Code, Cursor or VSCodium, init has already installed the replay viewer, so clicking the .replay file plays the match. Install it by hand if your editor arrived later.

Raw
Copy Code
unswbc vscode

Otherwise open the visualiser and choose the file. Either way you see the whole board and every line your bots printed, neither of which a bot can see for itself.

Log in

Make an API key on your team page — it starts with bc_ — and hand it to the toolkit. unswbc auth status then names the team the key belongs to.

Raw
Copy Code
unswbc auth set bc_...
Submit

unswbc submit zips the project and uploads it as a new version. -n names that version and -d records what changed. See Submitting via Website for what happens to the build afterwards.

Raw
Copy Code
unswbc submit mybot
Template Structure
Python
C
C++
Copy Code
import helper as unswbc

ct: unswbc.Controller
game: unswbc.Game

def execute_turn() -> None:
    ct.make_move(unswbc.Direction.NORTH)

def main() -> None:
    global ct, game
    ct, game = unswbc.init()

    while unswbc.update(ct, game):
        execute_turn()
        unswbc.end_turn()

if __name__ == "__main__":
    main()

The template provides you with a game loop. init() reads the initial setup information from the engine, and update() will read in information from a new turn (or false, if the game ends or dragon dies). You should make changes to the execute_turn() function, and end_turn() will finish the bot's turn. Advanced teams can choose to write their own interface with the engine (see interface docs).

The starter bot shuffles the four directions from a generator seeded the same way every run, then steps onto the first neighbouring tile that is not blocked by kelp or a dragon. All three starters do this, so a Python bot and a C++ bot behave alike. Take a look through the rest of the docs to improve this simple bot!

Next steps

Be sure to test your bots on multiple maps! It may be useful to create your own maps to test how your bot reacts to specific situations or edge cases. The CLI page lists every command and its options.

Previous
Overview
Next
Submitting via Website