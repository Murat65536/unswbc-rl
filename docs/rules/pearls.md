Search documentation
Ctrl K
Browse documentation
Pearls

Pearls are the sole resource available, and are used to increase the length of dragons.

Pearl Spawning

Each tile has a countdown which is visible to all dragons which can inspect the tile (see Vision). This countdown represents the number of rounds until said tile next attempts to spawn a pearl. Thus, each round the countdown decrements by 1. Countdowns are decremented at the start of each round, before any dragon moves (see Execution order). When the countdown reaches zero, the tile attempts to spawn a pearl. Regardless of the outcome of this attempt, the countdown resets.

An attempt to spawn a pearl succeeds only when the tile is empty (i.e. no dragon segments nor pearls occupy it).

Resetting the countdown involves drawing uniformly at random from [min_gap, max_gap]. These values are tile-specific properties set on the map, but are not publicly visible to dragons. A tile with max_gap 0 never spawns pearls and always reports a countdown of -1.

R0
The marked tile's countdown reaches zero while it is empty, and so a pearl spawns.
Python
C
C++
Copy Code
soonest = None
for tile in ct.get_tiles():
    if tile.get_pearl_time() < 0:
        continue  # never spawns
    if soonest is None or tile.get_pearl_time() < soonest.get_pearl_time():
        soonest = tile

if soonest is not None:
    where = soonest.get_position()
    ct.set_indicator_string(f"pearl at {where.x},{where.y} in {soonest.get_pearl_time()}")
Symmetric Pearls

On a symmetric map, each tile shares a countdown with its mirror image, so pearls will attempt to spawn symmetrically. Of course, if one of the tiles is blocked when the countdown reaches 0, a pearl will only spawn on the other tile.

Death

When a dragon dies, it will drop half its length in pearls. See Death for more.

Previous
Game Map
Next
Kelp and Portals