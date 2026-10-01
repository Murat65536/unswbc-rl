Search documentation
Ctrl K
Browse documentation
Overview

The probe has landed at the bottom of the ocean depths, and through the camera you can see hundreds upon thousands of pearls, scattered amongst the sea floor. Deep sea dragons live here too, so in order to blend in, you and your team will be controlling a group of magnificent leviathan-like automata.

Each of your serpentine machines can collect pearls to grow in length; at the end of the mission, your queen, the lowest-id robot of your team, (and its pearls) will be extracted back to the surface.

As such, compete with other teams to stay alive and grow the longest queen dragon!

R0
A game played on a 16 by 16 map. Team A (blue) wins after eliminating all dragons from team B (orange).
Specifications

Each team can control up to 64 dragons at a time. Your team will write exactly one program, which each dragon will execute, running its own copy of your program. A corollary of this is that individual dragons do not share any program memory with other dragons, and hence the only way to communicate dragon-to-dragon is via the sonar system.

Each dragon gets approximately 25 milliseconds at most to compute and execute actions each turn. This is not entirely correct, so for more details about computational limits for each of your dragons, see the Timeouts section.

Gameplay

Your resident games expert notices that the interactions between these sea dragons are similar to the rules of the retro video game Snake, save for some differences:

These dragons can spontaneously split in two.
Dragons only communicate through long-distance sonar.
A dragon can move multiple times within a turn, though at a cost.
Next Steps
You can read about the full rules of the game starting from Structure.
After that, feel free to started with your first bot at Quickstart!
Some more technical information about the engine and turn order starts from IO Protocol.
Once you've submitted your bot to the ladder, see ELO System to understand rating and matches.
Next
Quickstart