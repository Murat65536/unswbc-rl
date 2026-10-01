Search documentation
Ctrl K
Browse documentation
Kelp and Portals

Kelp and portals exist on the edges between tiles, not the tiles themselves. This includes the edges along the map border.

R0
This poor dragon travels through a portal, straight into kelp.
Kelp

Kelp is the first type of special edge. Their job is to function as barriers; in particular, a dragon cannot pass from one tile to another across a kelp edge. If it tries, it dies (hit wall) and drops pearls like any other dead dragon.

Kelp is a plant. There are no other special properties of kelp.

Python
C
C++
Copy Code
def is_safe(direction: unswbc.Direction) -> bool:
    here = ct.get_position()
    tile = ct.get_tile(here)
    if tile is None:
        return False

    edge = tile.get_edge(direction)
    if edge.get_edge_type() == unswbc.EdgeType.KELP:
        return False

    ahead = ct.get_tile(here.add_dir(direction))
    return ahead is None or ahead.get_dragon() is None
Portals

Portals are the second type of special edge, and they allow for quick traversal of the map, conditional on your bravery. Each portal has an ID, which is an almost-unique non-negative integer. Exactly one other portal shares the same ID: these two portals are connected.

There are a couple constraints on connected portals. A pair of connected portals are guaranteed to have the same orientation (i.e. both lying along a vertical edge or a horizontal edge). Furthermore, both portals either lie on the map's line of symmetry, or both lie off it. This ensures that each portal maps to exactly one other portal without ambiguity.

Portals are double-sided, so you can enter from either side. Entering a portal from the left causes the head of your dragon to emerge from the right, and vice versa. The corresponding behaviour for vertically oriented portals is left as an exercise for you.

Dragons cannot see through to the other side of portals (see Vision), so it usually won't know what's on the other side.

Sonar travels through portals.

Python
C
C++
Copy Code
edge = tile.get_edge(direction)
if edge.get_edge_type() == unswbc.EdgeType.PORTAL:
    ct.output_log(f"portal {edge.get_portal_id()} to the {direction.value}")
Previous
Pearls
Next
Vision