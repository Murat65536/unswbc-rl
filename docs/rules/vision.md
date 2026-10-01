Search documentation
Ctrl K
Browse documentation
Vision

Every dragon can only see the 7 by 7 square of tiles centred on its head; that is, all 49 tiles with a Chebyshev distance no greater than 3. This vision square wraps around the map as well due to its topology.

Again, note that vision does not extend to the far side of a portal, which is visible only if it is within three tiles of the head by ordinary distance.

R0
A visualisation of a dragon's vision square as it moves. Note the interactions with portals and map borders.
Vision Information

For each tile in view, the dragon receives its (absolute) coordinates, its contents (pearls and dragon parts), its pearl countdown (see Pearls) and its four edges (see Kelp and Portals).

Python
C
C++
Copy Code
tile = ct.get_tile(pos)             # None when pos is outside the window

tile.get_position()                 # board coordinates, already wrapped
tile.get_pearl_time()               # rounds until a pearl spawns here; -1 if never
tile.has_pearl()                    # True if a pearl is on it now

edge = tile.get_edge(unswbc.Direction.NORTH)
edge.get_edge_type()                # EdgeType.EMPTY, EdgeType.KELP or EdgeType.PORTAL
edge.get_portal_id()                # id shared with the partner edge; -1 if not a portal

part = tile.get_dragon()            # None if no dragon is on it
if part is not None:
    part.get_team()                 # Team.A or Team.B
    part.get_id()                   # which dragon
    part.is_head()                  # True for the head
    part.get_dir()                  # head: its heading; body: the way to the head

A turn that steps onto a neighbouring pearl when it can see one, and onto a tile with no dragon on it otherwise:

Python
C
C++
Copy Code
def execute_turn() -> None:
    here = ct.get_position()
    fallback = unswbc.Direction.NORTH

    for direction in unswbc.Direction.get_direction_list():
        tile = ct.get_tile(here.add_dir(direction))
        if tile is None:
            continue
        if tile.has_pearl():
            ct.make_move(direction)
            return
        if tile.get_dragon() is None:
            fallback = direction

    ct.make_move(fallback)
Previous
Kelp and Portals
Next
Movement