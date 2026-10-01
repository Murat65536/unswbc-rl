Search documentation
Ctrl K
Browse documentation
Helper reference

Everything the helper library gives you, taken from the helper.py that unswbc init put in your project. The helper is yours: you may edit it, or replace it with your own reader of the wire protocol.

This page follows the helper the current toolkit ships. A project made with an earlier one keeps the copy it was given, so if a call listed here is missing from yours, upgrade the toolkit and take the helper from a fresh unswbc init. The three languages offer the same calls under each language's own names.

Python
C
C++
Constants

What the engine is fixed at: 500 rounds, a 7x7 window, length 3 at spawn, length 2 minimum.

Direction

One of the four compass directions. North is up, and y grows downwards.

value() -> str

The protocol letter, N, E, S or W.

get_direction_list() -> list[Direction]

A fresh list of all four, north, east, south, west.

get_offset() -> tuple[int, int]

The (dx, dy) of one step this way, with y growing downwards.

get_opposite() -> Direction

The reverse direction.

get_left() -> Direction

A quarter turn anticlockwise.

get_right() -> Direction

A quarter turn clockwise.

Team

Which side a dragon plays for.

get_enemy_team() -> Team

The other team.

Position

A tile's x and y, with (0, 0) at the top left and y growing downwards.

add_dir(direction: Direction) -> Position

The position one step that way, wrapped around the map edges.

is_in_map()

Whether these coordinates are on the board, which only matters for ones you worked out yourself.

is_in_vision()

Whether this tile is inside your 7x7 window this turn.

Game

The board and the round number, which every dragon sees the same way.

get_round_num() -> int

The round being played; the game ends after 500 of them.

get_map_size() -> tuple[int, int]

The map's (width, height) in tiles.

get_unit_limit() -> int

The most dragons one team may have alive at once, which is what caps splitting.

Vision

The 7x7 block of tiles around your head, parsed as you ask for it.

get_tiles() -> list[Tile]

All 49 tiles, row by row from the top left.

get_tile(pos: Position) -> Tile | None

The tile at pos, or None if pos is outside the window.

Tile

One square of the board, with whatever stands on it and its four edges.

edges() -> dict[Direction, Edge]

The four edges keyed by direction.

get_edge(direction: Direction) -> Edge

The edge you would cross stepping that way.

get_dragon() -> DragonPart | None

The dragon segment standing here, or None if the tile is clear.

has_pearl() -> bool

Whether a pearl is on this tile right now.

get_pearl_time() -> int

Rounds until this tile next tries to spawn a pearl, or -1 if it never does.

get_position() -> Position

This tile's position on the board.

DragonPart

One segment of a dragon, head or body.

get_position() -> Position

Where this segment is.

get_id() -> int

The dragon this segment belongs to.

get_team() -> Team

The team that dragon plays for.

get_dir() -> Direction

The direction this segment faces.

is_head() -> bool

Whether this is the head; stepping into another dragon's head kills both of you.

EdgeType

What lies between two tiles: EMPTY, KELP or PORTAL.

Edge

The boundary between two tiles.

is_passable() -> bool

Whether a dragon can cross this edge; kelp is the only thing that stops one.

is_portal() -> bool

Whether crossing this edge comes out at its partner edge.

get_edge_type() -> EdgeType

Whether this edge is EMPTY, KELP or PORTAL.

get_portal_id() -> int

The id this portal shares with its far edge, or -1 if this edge is not a portal.

Controller

Your dragon: what it sees, how long it is, and the commands it sends this turn.

get_length() -> int

Segments your dragon has, counting the head.

get_unit_count() -> int

Dragons your team has alive, this one included.

get_head() -> DragonPart

Your dragon's head segment.

get_id() -> int

Your dragon's id, which is also its place in the turn order.

get_team() -> Team

The team you play for.

get_dir() -> Direction

The way your head points at the start of this turn.

get_vision() -> Vision

The tiles around your head.

get_tiles() -> list[Tile]

All 49 tiles in view, row by row from the top left.

get_tile(pos: Position) -> Tile | None

The tile at pos, or None if pos is outside the window.

get_position() -> Position

Your head's position, already wrapped into the map.

make_move(direction: Direction)

Steps one tile that way; of everything you send, the last action is the one applied.

make_moves(directions: list[Direction])

Sprints one step per direction in the list.

can_split(child_size: int) -> bool

Whether a child of that many segments is legal this turn.

do_split(child_size: int)

Splits that many segments off your tail.

output_log(*message)

Prints a line that shows against this turn in the replay.

draw_indicator_dot(pos: Position, r: int, g: int, b: int)

Draws a dot on the replay's board, which changes nothing in the game.

draw_indicator_line(start: Position, end: Position, r: int, g: int, b: int)

Draws a line on the replay's board.

set_indicator_string(message: str)

Labels your dragon with this text for the turn.

get_sonar_messages() -> list[int]

Values that reached you since your last turn, in the order they were sent.

get_sonar_echoes() -> SonarEchoes
send_sonar(direction_or_message: Direction | int, message: int | None = None) -> bool
helper

The module itself, imported as unswbc.

init() -> tuple[Controller, Game]

Reads the spawn block and returns your (controller, game), which stay valid all match.

update(controller: Controller, game_state: Game) -> bool

Reads the next turn into the controller, and returns False once the game is over or your dragon has died.

end_turn()

Ends the turn and flushes everything you printed.

Previous
Standard Library
Next
IO Protocol