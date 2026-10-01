Search documentation
Ctrl K
Browse documentation
Game format

By facing off with competitor teams through ranked and unranked games, you will get an ELO score that determines your spot on the leaderboard. Your ELO score increases when you win games, and decreases when you lose games. ELO System sets out the arithmetic and the tiers.

Ranked and unranked

A ranked battle consists of five games and will cause team ELOs to update. Maps are chosen at random by the competition server. Ranked battles come from two places: the ones you request, and the autoscrim draws below, which are ranked as well. In an unranked battle, teams may choose to verse another team on particular maps; one game is played for each map. Unranked battles do not update ELOs nor rankings. Your team keeps the same colour across every game of a battle, and after each battle you will be able to see the replays.

Requesting a battle ('scrim')

You can request a battle against a specific team on the leaderboard or from a team's page in find a team. The battle outcome appears in Battles, which the My team box narrows to your own. Note that all battles are publicly viewable, so be wise about concealing your strategy!

Both teams need an active submission. Unranked battles are then open to every team; ranked ones need the switch on under Settings on both Team pages, and must target a team rated no more than 50 below you, although anyone above you is fair game. That switch waits eight hours between flips, so nobody can duck a rating by turning it off between requests. Your team may start 60 games an hour by hand, which is 12 ranked scrims. Battles against organising teams (marked DEV) have a separate allowance of another 60 games an hour.

Battles you are drawn into ('autoscrim')

Autoscrim battles are ranked. They move your rating exactly as a ranked battle you asked for does, and most of the games your team plays will be these, so your place on the ladder is mostly decided by them.

You do not have to ask for every battle. Every hour, on the UTC boundary, the ladder is sorted and each team is drawn one opponent from the eight places above and the eight below it, so you meet teams near your own rating. The draw favours opponents you have not played for a while, and over a day you see most of the ladder around you.

You are in the draw whenever you have an active submission; the ranked switch in your settings governs only the battles teams request. Each draw is a ranked five-game series on five maps chosen at random, at most one per team per hourly batch, and it does not spend your hourly allowance. You may receive none if no suitable opponent remains.

At times autoscrims run continuously instead, and the sidebar says so. Battles are then queued all the time at a fixed rate, and every team gets the same number of them: whoever has had the fewest in the last 24 hours is drawn next, however quickly their games finish. Opponents come from up to 50 places either side, weighted towards close games and towards teams you have not met for a while. You play one autoscrim at a time.

Submitting a bot

Using either the CLI or the website, you may upload and submit code. Once submissions are made, they cannot be deleted. Only one submission can be 'active' at a time. Your 'active' submission is the one that the game server will use for any incoming / outgoing battles.

Each submission has its own rating, and your team's rating is your active submission's. A new submission starts from your previous one's rating and moves quickly at first, then settles as its games add up. Uploading the same code again picks up exactly where that code left off. See New bots.

Previous
Death
Next
ELO System