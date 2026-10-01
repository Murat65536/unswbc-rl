Search documentation
Ctrl K
Browse documentation
Movement

Movement is considered a type of action that a dragon can perform on a turn. Note that each dragon must output at least one action per turn (only the last one is actually performed; see Death for a list of death conditions). The only other type of action which can be performed is splitting.

Movement, like all other unit actions, is performed only once the turn is complete.

Standard Movement
R0
Dragons move eastwards.

Each dragon can move one tile in any cardinal direction (north, east, south, west) each turn. This causes the head to move in said direction, with the rest of the body following along.

Dragons can even move backwards, though doing so causes its head to squeeze into its own neck, a move which often proves fatal.

A dragon's facing direction is the direction of its head from its first body (non-head) segment. The facing direction of its body segments is defined similarly (direction to the next segment towards the head).

Python
C
C++
Raw
Copy Code
ct.make_move(unswbc.Direction.EAST)
Eating Pearls
R0
A dragon eats a pearl and grows longer by a unit.

When a dragon moves its head onto a pearl, the pearl is automatically eaten and the tail does not advance that turn. In other words, the dragon grows a body segment.

Eating pearls is the only way to gain length.

Collisions
R0
Everyone dies via various means except the orange dragon.

A dragon dies if its head moves into any obstacles. This includes kelp, any segment of another dragon, or any segment of itself.

Note that this death condition is checked before any actual movement. This means a dragon will die moving into own tail, even if the tail would have moved out of the way in the same turn.

If the destination contains another dragon's head, both dragons die, regardless of their team.

Python
C
C++
Copy Code
ahead = ct.get_tile(here.add_dir(direction))
part = ahead.get_dragon() if ahead is not None else None
if part is not None:
    if part.is_head():
        continue  # both of us would die
    continue      # only I would die

A dragon that dies drops pearls along its body. See Death for more information.

Sprinting
R0
Dragon decides to sprint north + north + east.

Dragons may decide to move several times in one turn in varying directions.

Although all moves happen within a single turn, collision checking (and pearl eating) is performed at every step. This means that in theory, a dragon might die performing NNE, but live with ENN, even though they target the same destination.

A dragon of length L takes its first ceil(L / 4) steps of a turn free, with L fixed when the move starts. Every step after them costs a unit segment of the sprinting dragon. So moving x steps in a turn removes max(0, x - ceil(L / 4)) segments: 5 steps cost a length-4 dragon 4 segments, and a length-12 dragon 2.

Python
C
C++
Raw
Copy Code
ct.make_moves([
    unswbc.Direction.NORTH,
    unswbc.Direction.NORTH,
    unswbc.Direction.EAST,
])
Death by Sprint
R0
Dragon implodes due to oversprinting.

A move with more steps than the dragon can pay for is not truncated. The dragon will perform the steps that can be paid for, and then will pay for the next step with its own life.

Previous
Vision
Next
Splitting