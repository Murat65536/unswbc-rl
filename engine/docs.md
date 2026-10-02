# engine events

event types: `include/engine/event.h`. state: `include/engine/types.h`.
first the loop, then one made-up game run through it so every event shows up every way it can.
coords are illustrative, don't check them against a real map.

## game loop

- game init
  - for each tile that spawns pearls: draw its first countdown, emit `EventPearlCountdown`
  - for each starting dragon: emit `EventDragonUpdate` (already known from the map, this just confirms it landed)
- for int round = 0; round < 500 round ++
  - emit `EventRoundStart { round }`
  - pearl tick
    - for each spawning tile, top by bottom, then left to right
      - sub 1 from its countdown
      - if still gt 0, continue
      - if the tile is free (no pearl, no dragon): put a pearl there, emit `EventTileChange Pearl`
      - either way: draw a new countdown, emit `EventPearlCountdown`. a countdown with no tile change just before it = blocked
  - for each dragon_id in dragon_ids (live list: a child born by a split this round is appended and takes its own turn later this round)
    - if dead, continue
    - emit `EventTurnStart { id }`
    - empty dragon sonar inbox and echo counts and append to stdin
      - protocol 2 dragon: inbox values above `UINT32_MAX` are left out, and no `ECHOES` line
      - protocol 3 dragon: every inbox value, then `ECHOES kelp ally allyHead enemy enemyHead`; a ray that stopped on nothing adds no count
    - default sonars = none, default action = suicide, default indicator = null
    - set default action to suicide
    - read the bot's reply line by line, return prematurely on exit / timeout
      - discard if line is invalid
      - stop reading if ENDTURN
      - `MOVE` / `SPLIT`: overwrite the action
      - `SONAR <u32>`: overwrite the facing sonar
      - `SONAR <N|E|S|W> <u64>`: overwrite the sonar for that direction
      - `PROTOCOL <n>`: this dragon, and every child it splits off later, now gets protocol n input
      - `INDICATOR`: overwrite the indicator
      - `LOG` / `DOT` / `LINE`: handle IMMEDIATELY, emit `EventDragonLog` / `EventDebugDraw` right there.
      - anything unreadable (such as `SONAR 43 COOKEDBS`) is skipped, and gets an `EventEngineLog` emitted.
      - `DebugOutput` decides which of `LOG`, `INDICATOR`, `DOT`/`LINE` and the unreadable-line complaints are kept at all. A
        channel that is off is read and dropped: no event, no indicator set, and no note budget spent. `judge serve` runs with
        the logs and the complaints off; the CLI keeps everything unless a `--no-` flag says otherwise.
      - `DebugOutput::mLimits` holds each team to `MAX_TEAM_NOTES` events and `MAX_TEAM_TEXT` bytes of bot output a
        game, and cuts an indicator to `MAX_INDICATOR`. Past a cap the team's output is dropped after one
        `EventEngineLog` saying so. The judge always sets it; the CLI only with `--sandbox`.
    - if timeout / error, restart the bot.
    - if indicator set: emit `EventDragonIndicator`
    - emit `EventDragonAction { id, action }`
    - try the action
      - move: for each step
        - if not the first step, check if can pay
          - len == 2: emit `EventEngineLog` "can't pay for step k"
            - death sequence with `NoValidAction`, BREAK
        - work out the destination from the boundary the head is facing
          - kelp: death sequence with `HitWall`, BREAK
          - portal: destination = tile on the far side of the partner boundary, heading unchanged
          - otherwise: next tile over, wrapped (torus)
        - look at what's on the destination
          - own segment (tail included): die with `HitSelf`, BREAK
          - another dragon's head (teammate or not)
            - THAT dragon death sequence with `HitHeadToHead`
            - then this one death sequence with `HitHeadToHead`
          - another dragon's body: die with `HitOtherBody`, BREAK
        - push the head
          - pearl there: eat it, emit `EventTileChange Empty`, tail stays
          - no pearl: tail moves up 1
          - not the first step: now pay 1 tail square
          - so k steps with no pearls costs k-1 segments on top of the usual tail move: length L gets at most L-1 steps, each pearl eaten buys one more
        - emit `EventDragonUpdate { id, facing, head, tail }`
      - split n
        - if the acting team already has `UNIT_LIMIT` living units
          - `EventEngineLog`, die with `NoValidAction`
        - if n < 2 or parent remaining would be < 2
           - `EventEngineLog`, die with `NoValidAction`
        - rear n segments become a new dragon, next id, same team
          - child head = old tail
          - facing = direction from its 2nd segment to its head
        - emit `EventDragonSplit`. 
      - suicide: die with `NoValidAction`
    - sonars, if the dragon is still alive, in the order N, E, S, W
      - the facing sonar goes to the facing the action left it with, unless a directed sonar for that direction is set
      - ray from the head along its direction; a ray opposite the facing instead leaves the tail, along the way the tail points (from the second-last segment to the tail)
      - goes through portals and wraps
      - stops at kelp or at the first dragon it meets, the sender's own body included
      - if hit:
        - value goes to dragon inbox
      - if no hit after width + height steps:
        - 'end' is considered too be the last tile walked
      - hit kind, from the sender's view: empty, kelp, ally, allyHead, enemy, enemyHead
      - add 1 to the sender's echo count for that kind, unless empty
      - emit `EventSonarPing { sender, dir, value, origin, end, hit?, hitKind }`
    - go and process the next dragon
  - round over. game over if a team has no dragons left, or this was the last round


- death sequence, same everywhere
  - emit `EventDragonDeath { id, reason }`
  - every other segment of the body (as of current engine state) turns into a pearl: head, skip one, next, skip one, and so on.
    - a length of L drops ceil(L / 2) pearls, spread evenly along the whole body
    - one `EventTileChange Pearl` each, head towards tail
    - does NOT change pearl countdown
  - remove the dragon from the board
  - whoever was mid-move: the rest of their steps and their sonar are dropped
