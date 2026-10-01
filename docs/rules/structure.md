Search documentation
Ctrl K
Browse documentation
Structure
Rounds and Turns

Each game lasts at most 500 rounds. Within each round, every currently living dragon will perform some number of actions. One of these time steps is known as a turn.

Turns are carried out in ascending order of dragon ID. This means on a given dragon's turn, all dragons with lesser IDs have already acted, and all dragons with greater IDs have not. For more details about the game loop, see Execution Order.

Scoring

Each game has three possible outcomes: team A wins, team B wins, or a draw.

Your team is eliminated when all of your dragons perish. Upon the elimination of any team, the game automatically ends, regardless of the current round number. If exactly one team is eliminated, the other wins. If both teams are eliminated in the same round, the game is a draw. Otherwise, after 500 rounds, the winner is decided by the following tiebreaks in order:

The team with the longer queen dragon. One team has dragon id 0 and the other has dragon id 1; each is its team's queen. A dead queen has a length of 0, so a team whose queen dies loses to a team whose queen lives.
The team with the longest living dragon.
The team with the greatest total length across all of its dragons.
Draw (no winning team decided).
Previous
Submitting via Website
Next
Game Map