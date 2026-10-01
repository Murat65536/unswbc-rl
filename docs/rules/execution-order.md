Search documentation
Ctrl K
Browse documentation
Execution order

This page gives the engine loop in the order it runs. Most surprising replays come down to two facts: dragons act one at a time in id order, and a multi-step move is a sequence of separate moves.

The round
Pearls tick. Every tile's countdown decreases by one, top row to bottom, left to right. At zero the tile spawns a pearl if it is free, and draws a new countdown either way. See Pearl spawns.
Each living dragon takes a turn, in ascending id order. A dragon that died earlier this round is skipped. A dragon created by a split this round is appended to the list and takes its turn later this round.
The round ends. The game ends if a team has no dragons, or if this was round 500.
One turn
The dragon's sonar inbox and echo counts are written to the bot's input and emptied.
Defaults are set: action = suicide, sonars = none, indicator = none.
The bot's output is read line by line until ENDTURN.
The action is applied.
Sonars are cast in the order N, E, S, W, if the dragon is alive.
Raw
Copy Code
LOG heading for the pearl at 4,7
DOT 4 7 0 255 0
MOVE NNE
SONAR E 90210
PROTOCOL 3
ENDTURN
Commands

MOVE, SPLIT and INDICATOR each overwrite the previous value, and SONAR overwrites the previous one in the same direction. The last one read takes effect, and none is applied until reading ends. LOG, DOT and LINE are applied immediately, in the order printed. A line that cannot be parsed is skipped, with an engine log entry, and does not change the action already set.

Raw
Copy Code
MOVE N
MOVE E
ENDTURN

That pair moves east.

Applying a move
R0
Three steps east asked for. The third is blocked, so the dragon dies after two.

For each step, in order:

If this is not one of the first ceil(L / 4) steps, where L is the dragon's length when the move starts, check that the dragon can pay for it. A dragon of length 2 cannot, and dies with no valid action.
The dragon's facing is set to the step's direction. The destination is the tile across the edge now faced: kelp kills the dragon, a portal leads out of its partner edge, and otherwise the destination is the adjacent tile, wrapped.
The destination is checked against the dragon's own body first, then against other dragons. Another dragon's head there dies first, then this dragon. Any other segment kills only this dragon.
The head moves. A pearl on the destination is eaten. The tail advances unless a pearl was eaten. If this is not one of the free steps, one further tail segment is removed.

A death on any step discards the remaining steps and the turn's sonar. Steps already taken stand.

The dragon's own tail
R0
Coiling inward until the only tile left is its own tail, which has not moved yet.

The self-collision check in step 3 includes the tail and runs before the tail advances, so the tail's tile is blocked for that step. Within a multi-move, the second step is checked against the body as the first step left it, so a tile that is fatal as a first step may be safe as a second.

Applying a split

The conditions are checked before anything moves: child length at least 2, parent length at least 2, team below UNIT_LIMIT. Failing any of them kills the parent with no valid action. The child is the rear segments in reverse, appended to the live list with the next id, and takes its own turn later this round from a new process.

Casting sonar

After the action, if the dragon survived, one ray for each direction set, in the order N, E, S, W. Each ray leaves the head. A ray opposite to a dragon's direction instead leaves the tail, in the direction the tail points away from the body. Each ray goes through portals, around the wrap, and stops at kelp or at the first living dragon (including the sender's own body). The value is placed in that dragon's inbox and read at the start of its next turn, so a higher id receives it this round and a lower id next round. The sender gets one echo count for what each ray stopped on.

The death sequence
The death is recorded with its reason.
Every second segment of the body as it stands becomes a pearl: head, skip one, next. Pearl countdowns are not changed.
The dragon is removed from the board.
Its remaining steps and its sonars are discarded.

In a head-on collision, the other dragon's sequence runs first, then this dragon's.

When a turn ends

A turn ends at the first of: the bot printing ENDTURN, the bot blocking to read its next turn, the bot exiting, or the bot reaching its limit (see Timeouts). In the first two cases the turn's output is delivered to the engine. In the last two nothing is delivered, the action stays at its default of suicide, and the bot is restarted as a new process with a fresh init block.

Previous
CLI
Next
Timeouts