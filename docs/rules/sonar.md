Search documentation
Ctrl K
Browse documentation
Sonar
Sending
R0
Two teammates. Each turn the ray leaves the sender and stops at the first body it meets.

Each turn a dragon may send up to four sonars, one in each direction: N, E, S and W. Each sonar is an unsigned 64-bit integer. After the dragon's action has been applied, if the dragon is still alive, the sonars are cast in the order N, E, S, W. Each one travels in a straight line from the head. Sonars wraps around the board, passes through portals, and stops at the first kelp or dragon part it hits. If it hits a dragon, the message is delivered to that dragon, which may be the sender itself. If it reaches no dragon within WIDTH + HEIGHT tiles, it is lost.

Python
C
C++
Raw
Copy Code
ct.send_sonar(Direction.EAST, 1806)
ct.send_sonar(Direction.NORTH, 2 ** 40)

for message in ct.get_sonar_messages():
    ct.output_log(f"heard {message}")

echoes = ct.get_sonar_echoes()
ct.output_log(f"enemy heads found: {echoes.enemy_head}")
Edge cases

Dragons have extremely high refractive indices, exhibiting Total Internal Refraction. Thus, sending a sonar into its own body will cause the sonar to exit at the tail, following the direction that the tail is pointing. Example: a east-ward facing dragon issuing a westward sonar will have its sonar emitted through its tail instead, directed at whichever direction the tail is facing.

If you send two sonars in the same direction, only the last one is sent.

Receiving
R0
The first message reaches our friend, but then an enemy gets in the way and intercepts all our later messages.

Sonars are read at the start of the recipient's next turn. A dragon with a higher id than the sender therefore receives the signal later in the same round, and a dragon with a lower id receives it in the following round. Several sonars may arrive in one turn, in the order they were sent. A signal is lost if the sender died during its move, since no signal is cast, or if it reached nothing. The replay shows the ray in either case. A signal may be received by a dragon on either team. No information about the sender or team is carried.

Echoes

At the start of its next turn, the sender gets the counts for each kind of thing its sonars stopped on. A sonar that reached nothing adds no count.

Kind	The sonar stopped because
kelp	it hit kelp
ally	it hit the body of a dragon on your team, your own included
ally_head	it hit the head of a dragon on your team
enemy	it hit the body of a dragon on the other team
enemy_head	it hit the head of a dragon on the other team

As an example, if a dragon sent 4 sonar beams, 2 of them hit kelp, and one of them hit and ally's body, and the last sonar beam hit nothing, the counts will be 2, 1, 0, 0, 0 respectively.

A note on protocol

This only works on protocol 3. Please ensure your helper is up to date.

Previous
Splitting
Next
Death