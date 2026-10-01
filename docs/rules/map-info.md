Search documentation
Ctrl K
Browse documentation
Game Map
Dimensions

Games will be played on maps which are two-dimensional rectangular grids. Both width and height will be between 10 and 64 cells inclusive.

The top left corner will be denoted with coordinates (0, 0), with the x-coordinate increasing towards the right and the y-coordinate increasing down. North is oriented upwards.

Dragons can query the dimensions of the map as follows:

Python
C
C++
Copy Code
width, height = game.get_map_size()
Symmetry

To ensure fairness between teams, each map is guaranteed to be symmetric. This will either be via vertical mirroring, horizontal mirroring, or a 180 degree rotation.

Dragons are not able to easily query the symmetry of the map; that one's up to you!

Topology

The map edges 'wrap around'; specifically, a dragon that moves outside the bounds of the map will find itself on the other side.

On any sort of visual representation of a map, this is denoted by dashed lines running down the east and south borders. Nothing renders on these borders (instead, see their equivalent west and north borders respectively).

R0
A dragon can keep moving right past the east border.
Composition

Maps are composed of tiles and edges. Again, note that edges along the east and north map border are equivalent to their west and north counterparts (as dictated by the wrap-around).

See Pearls for information about certain tiles, and Kelp and Portals for information about nonempty edge types.

Previous
Structure
Next
Pearls