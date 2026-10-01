Search documentation
Ctrl K
Browse documentation
Death

A dragon dies when its head collides with something, or when its program fails to produce a valid action on its turn.

Causes of Death
Reason	Cause
Hit Wall	Moved into kelp.
Hit Self	Moved into its own body, including its tail.
Hit Other Body	Moved into a segment of another dragon from any team.
Head to Head	Moved into another dragon's head, causing both dragons to die.
No Valid Action	Requested a sprint it could not pay for, requested an illegal split, produced a malformed command, or outputted no action.
Mandatory Action

The engine treats a turn with no valid action as suicide. Any dragon whose program crashes or outputs nothing (or garbage) will be killed. Dragons which fail to end their turn before the time limit but have outputted at least one valid action will experience a different fate (see Timeouts).

Pearl Dropping

Upon death, dragons exhibit a startling phenomenon: starting from their head, every second body segment crystallises into pearls. A dragon of length L therefore drops ⌈L/2⌉ pearls.

R0
A dragon dies and drops pearls at every second segment.
Previous
Sonar
Next
Game Format