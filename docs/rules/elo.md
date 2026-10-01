Search documentation
Ctrl K
Browse documentation
ELO System

Ratings are built to predict who wins. Every submission has a rating overall and on each map, every ranked game updates the two bots that played it, and your team's rating is your active submission's.

In short
Each submission (bot) has its own rating, plus an offset on every map for how much better or worse it does there.
Your team's rating is your active submission's, averaged over the maps in the ranked pool. It is the number on the ladder and decides your tier.
Every ranked game updates both bots as soon as it finishes. Your team's rating moves once per ranked battle, by what that battle's own games did.
A new submission starts from the one your team was running when it took over, and moves quickly until its games show where it belongs. A team's first submission starts at 1500.
Switching submissions moves your team's rating to the new one's straight away.
Unranked games don't affect ratings.
What a rating means

A rating is a prediction. The gap between two bots' ratings on a map gives the chance each wins a game there. The bot that moves first gets an extra 20 points.

Ahead by	Wins
0	50% of games
100	64% of games
200	76% of games
400	91% of games
800	99% of games

These are for two bots whose ratings are well known. A bot that hasn't played much is predicted closer to an even game, since its rating could still be off.

Maps

Bots are much better on some maps than others, so each submission also has an offset on every map. Its rating on a map is its overall rating plus that offset. Your team's rating averages over the maps in the ranked pool, which is what a ranked battle's five random maps come to on average. The Submissions page shows each version's rating on every map. When maps join or leave the ranked pool, that average shifts, and your next ranked battle shows the shift along with its own result.

After each game

Every ranked game updates both bots as soon as it finishes: their overall ratings and their offsets on that map. Winning a game you were expected to win moves you a little; an upset moves you a lot.

Each rating also carries how sure it is. A new bot's rating is unsure, so each game moves it a long way. Every game makes it surer, and it moves less. A submission is a fixed program, so once a bot has played a lot its rating barely changes.

Your team's rating on the ladder moves once per ranked battle, when all its games are done. The change shown next to a battle is what that battle's own games did. Games of your other battles still being played count towards those battles when they finish.

The Submissions page shows a bot's rating as of its last game, so while a battle is still being played it can be ahead of your team's rating by that battle's games so far.

New bots

A new submission starts from the one your team was running when it took over: the same overall rating and the same offset on every map, but less sure, so it moves quickly to where it belongs. Your team's first submission starts at 1500 on every map.

Uploading code your team has already played ranked with starts it exactly where that code is. Same code means the same files with the same contents. How you zip them does not matter: timestamps, compression, a single folder wrapping everything, and __MACOSX, .DS_Store and __pycache__ entries are ignored. Changing any file, even a comment, makes it different code.

Activating an older submission again brings back its own rating. Whenever your active submission changes, by an upload finishing its build or by activating one yourself, your team's rating moves to the new one's straight away. Your battles list shows each switch as a row of its own, with the change it made.

If you switch while a ranked battle is still being played, that battle keeps the games played before the switch. Games your old submission plays after it no longer move your team's rating, since your rating is now the new submission's.

Why it works this way

A rating is only useful if it predicts results. Autoscrims pair teams by rating, the ladder and tiers are read from it, and tournaments are seeded by it. So the system was chosen, and its settings tuned, by one test: how well it predicts games it hasn't seen yet.

How it is measured

For each game, take the chance the ratings gave to what actually happened. Log loss is the average of minus the natural log of that chance. Lower is better. Predicting 50% every time scores 0.693. A confident prediction that turns out wrong costs far more than an unsure one, so the score rewards both picking the winner and being honest about how sure you are.

We replayed every ranked game played on the platform, predicting each one only from games that had finished before it. Settings were chosen on games from Sep 23 to 25 and then scored on the 303,725 games from Sep 26 to Oct 1.

System	Log loss	Winner called right
Coin flip	0.6931	50%
Previous team Elo	0.6716	56.4%
This system	0.5705	69.6%

The previous team Elo was 0.022 better than a coin flip. This system is 0.123 better, so it gets about five and a half times as much information out of the same games.

Where the gain comes from
Rating bots, not teams. A submission is a fixed program, so its strength doesn't change. The previous system moved a team's rating by the same K after every battle however long its bot had been playing, so settled bots kept drifting, and a new bot inherited a rating it hadn't earned. Here a bot's rating settles as its games add up.
Maps. This is the biggest part. How well a bot does depends heavily on the map: the same bot's win rate varies a lot from one map to the next. One number per bot can't capture that, and no single-number rating got anywhere near this score, even fitted to the whole week afterwards.
Knowing how sure it is. Predictions about new or rarely played bots stay close to even, and their ratings move fast. Predictions about settled bots can be confident, and their ratings barely move.
Honest predictions

When the system says the favourite wins with a given chance, the favourite wins about that often:

Predicted for the favourite	Favourite won
52%	52%
67%	66%
82%	81%
92%	91%
97%	96%

Wider starting uncertainty predicted very slightly better overall, but it overstated every rating gap, so the ladder spread further than the results justify. The settings below keep the gaps close to what the results show, for almost the same score.

The formula

Each bot has an overall rating R and an offset Om on each map m. Each of these is kept as a mean and a standard deviation σ for how unsure it is. Ratings are in points on the usual Elo scale, where 400 points is 10 to 1.

Setting	Value
Team's first submission, overall	1500 ± 200
Offset on a map with no games yet	0 ± 150
New submission	numbers of the submission it took over from, each with an extra ± 75
Same code as an earlier submission	that submission's numbers exactly
Moving first	+20
Predicting a game

For a game on map m between A and B, with c = ln 10 / 400:

z = c × (RA + OA,m − RB − OB,m + 20), the gap between them
v = c² × (the sum of the four numbers' σ²), how unsure the gap is
s = √(1 + πv / 8)
A wins with chance p = 1 / (1 + e−z / s)

With no uncertainty, s is 1 and this is the usual Elo curve. Uncertainty makes s larger and pulls p towards 50%.

Updating after a game

With y as A's result (1 for a win, ½ for a draw, 0 for a loss) and h = p(1 − p) / s², each of the four numbers moves:

mean: + σ² × c × (y − p) / (s × (1 + hv)) for A's numbers, and the same subtracted for B's
σ²: − σ⁴ × c² × h / (1 + hv)

This is one step of a Kalman filter on the prediction above. A number moves in proportion to how unsure it is, so a new bot's rating moves far and a settled one's barely moves, and every game makes both bots surer.

Your team's rating

Your active submission's overall rating plus the average of its offsets over the maps in the ranked pool, rounded, and never below 0.

Tiers
Tier	Rating
Fishing Boat	3000+
Leviathan	2700–2999
Orca	2300–2699
Shark	1900–2299
Swordfish	1500–1899
Tunafish	1100–1499
Shrimp	700–1099
Plankton	0–699
Previous
Game Format
Next
CLI