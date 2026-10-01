Search documentation
Ctrl K
Browse documentation
API

Everything a team does on Submissions and Battles, and everything readable on the site, is available over JSON. Team settings, membership and accounts are changed on the site only.

Keys

Each member makes one key on Team. It acts as that member, so uploads and challenges are recorded against their name. The key is shown once. Making a new one replaces the old one, leaving the team deletes it, and the leader can revoke any member's key.

Raw
Copy Code
curl https://game.battlecode.au/api/v1/me \
  -H "Authorization: Bearer bc_..."
Conventions

The base URL is https://game.battlecode.au/api/v1. Requests and responses are JSON, except the upload (multipart) and the two downloads. Errors carry the status and a message:

Raw
Copy Code
{ "error": "That API key is not valid. Make a new one on your team page." }

The limits are 120 requests a minute per key, and 30 a minute for /leaderboard and /ratings, the two expensive reads. Over either limit the response is 429 with a Retry-After header. An unknown path under /api/v1 is a 404 in the same shape, never an HTML page.

You and your team
GET /me your user, team and key.
GET /team your team with members, record, rank and pending join requests.
Submissions
GET /submissions every version with wins, draws and losses.
POST /submissions upload a version. Fields: name, language (python, c, cpp), description, zip. The zip is 4 MB at most, and a team can upload 12 versions an hour.
GET /submissions/:id one version with its build log.
GET /submissions/:id/download the zip you uploaded.
POST /submissions/:id/activate make it the version that plays.
Raw
Copy Code
curl https://game.battlecode.au/api/v1/submissions \
  -H "Authorization: Bearer $KEY" \
  -F name=v12 -F language=python -F description="wider search" -F zip=@mybot.zip
Raw
Copy Code
curl -X POST https://game.battlecode.au/api/v1/submissions/42/activate \
  -H "Authorization: Bearer $KEY"
Battles
GET /battles?limit=50 your battles, newest first, up to 200.
POST /battles challenge a team. Body: teamId, ranked, mapIds (unranked only, optional). Ranked is a five-game series on random maps. Same limits as the site: 60 games an hour, and a separate 60 against dev teams.
GET /battles/:id one battle with its games and, if unplayed, its place in the queue. For your own battles it also carries log, which says why a game crashed or timed out.
GET /battles/:id/replay the replay file, once the game has finished. Answers with a redirect to a temporary download link, so follow redirects (curl -L). The link carries its own signature, so your key must not be sent on to it. curl drops the header across hosts for you; some clients keep it, Python's urllib among them, and the download is refused.
Raw
Copy Code
curl https://game.battlecode.au/api/v1/battles \
  -H "Authorization: Bearer $KEY" -H "Content-Type: application/json" \
  -d '{ "teamId": 7, "ranked": false, "mapIds": [3] }'
Raw
Copy Code
{ "ids": [1201] }
Raw
Copy Code
curl -L -o 1201.replay https://game.battlecode.au/api/v1/battles/1201/replay \
  -H "Authorization: Bearer $KEY"
Reads
GET /leaderboard the ladder as the Leaderboard page shows it: rating history, members, institutions and prize eligibility.
GET /ratings the same ladder, ratings and history only.
GET /teams teams listed on Find a team.
GET /teams/:id a team's public profile and recent battles.
GET /tournaments and GET /tournaments/:id tournaments and brackets.
GET /maps the maps in play, with their text.
GET /queue live judge capacity and queue length.
Previous
Map Files