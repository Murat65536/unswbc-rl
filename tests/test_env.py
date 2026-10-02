"""The batched training environment (core/batch_env.h).

The central check replays the environment's own levels and actions on a
bccore.Match, which builds each dragon's round block at the moment the engine
plays its turn: every observation and mask the environment handed out must
equal the one the bot would compute from that block. That proves each
decision saw the exact board of its turn (the staleness problem), that split
children are asked in their birth round, and that actions decode the same
way. The rest checks the reward bookkeeping: the shaping telescopes, results
arrive discounted, and the training horizon cuts games with bootstrap
observations that match the board at the cut.
"""
import pathlib
import sys

import numpy as np
import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

bccore = pytest.importorskip("bccore", reason="build the C++ core first (see README)")

OPTIONS = {"width": (12, 24), "height": (12, 24), "dragons_per_team": (1, 3), "start_length": (3, 8),
           "portal_pairs": (0, 4), "kelp": (0.0, 0.15), "unit_limit": 6, "threads": 2}


class Buffers:
    def __init__(self, n):
        self.obs = np.zeros((n, bccore.OBS_SIZE), np.uint8)
        self.mask = np.zeros((n, bccore.NUM_ACTIONS), np.uint8)
        self.priv = np.zeros((n, bccore.NUM_PRIVILEGED), np.float32)
        self.prev_row = np.zeros(n, np.int64)
        self.prev_reward = np.zeros(n, np.float32)
        self.info = np.zeros((n, bccore.NUM_INFO), np.int32)

    def args(self):
        return self.obs, self.mask, self.priv, self.prev_row, self.prev_reward, self.info

    def copy(self):
        return {k: getattr(self, k).copy() for k in ("obs", "mask", "priv", "prev_row", "prev_reward", "info")}


def random_actions(rng, mask, split_bias=0.0):
    actions = np.zeros(len(mask), np.int32)
    for i, m in enumerate(mask):
        allowed = np.flatnonzero(m)
        if split_bias and m[12] and rng.random() < split_bias:
            actions[i] = 12
        else:
            actions[i] = rng.choice(allowed)
    return actions


def run(env, steps, rng, split_bias=0.05, plan=None):
    """Steps `env` with random allowed actions. records[k] is (the decisions
    after k steps, k = 0 being reset(); the actions then played; the step
    output that produced those decisions). plan(t), if given, names the next
    level of each slot before step t, so a game that ends in step t is
    followed by the level plan(t) set."""
    buf = Buffers(env.num_games)
    out = env.reset(*buf.args())
    records = [[buf.copy(), None, out]]
    for t in range(steps):
        if plan is not None:
            for slot, seed in plan(t).items():
                env.set_next_level(slot, seed)
        actions = random_actions(rng, buf.mask, split_bias)
        records[-1][1] = actions
        out = env.step(actions, *buf.args())
        records.append([buf.copy(), None, out])
    return records


def episode_seeds(records, first_seed, base):
    """The level seed of each episode of slot 0, under plan(t) = base + t."""
    seeds = {1: first_seed}
    for k, (buf, actions, out) in enumerate(records):
        if k and len(out["episodes"]["slot"]):
            seeds[int(buf["info"][0][3])] = base + k - 1
    return seeds


def replay(seed, options, decisions):
    """A Match on that level, with the (decisions, action) pairs played."""
    text, match_seed, _ = bccore.level_map(seed, options)
    match = bccore.Match(text, match_seed)
    match.begin()
    for buf, action in decisions:
        did = int(buf["info"][0][0])
        init, block = match.init_block(did), match.round_block(did)
        match.reply(bccore.decode_action(init, block, action) + "\nENDTURN\n")
    return match


def test_every_decision_saw_its_turns_board():
    rng = np.random.default_rng(1)
    env = bccore.BatchEnv(1, 0, OPTIONS)
    env.set_next_level(0, 123)
    records = run(env, 1500, rng, split_bias=0.15, plan=lambda t: {0: 50_000 + t})
    seeds = episode_seeds(records, 123, 50_000)

    by_episode = {}
    for buf, actions, out in records:
        if actions is not None:
            by_episode.setdefault(int(buf["info"][0][3]), []).append((buf, int(actions[0])))
    finished = [e for e in sorted(by_episode) if e + 1 in by_episode]
    assert len(finished) >= 3
    births = 0
    for episode in finished:
        text, match_seed, _ = bccore.level_map(seeds[episode], OPTIONS)
        match = bccore.Match(text, match_seed)
        match.begin()
        split_round = {}
        for buf, action in by_episode[episode]:
            did, team, round_num, _ = (int(v) for v in buf["info"][0])
            assert match.current() == did and match.round == round_num
            init, block = match.init_block(did), match.round_block(did)
            obs, mask0, mask1 = bccore.features_from_blocks(init, block)
            assert np.array_equal(obs, buf["obs"][0])
            assert np.array_equal(mask1, buf["mask"][0])
            if did in split_round:
                assert split_round.pop(did) == round_num  # a child acts in its birth round
                births += 1
            if action == 12:
                split_round[len(match.dragons())] = round_num
            match.reply(bccore.decode_action(init, block, action) + "\nENDTURN\n")
        assert match.current() is None  # the game ended where the environment said
    assert births > 0


def phi(priv, shaping=0.5, queen_weight=0.5):
    qa, qb = priv[6] * 32, priv[7] * 32
    ta, tb = priv[2] * 64, priv[3] * 64
    return shaping * (queen_weight * (qa - qb) / (qa + qb + 2) + (1 - queen_weight) * (ta - tb) / (ta + tb + 2))


@pytest.mark.parametrize("gamma,scripted", [(1.0, 0.0), (0.95, 0.0), (0.95, 0.5)])
def test_shaping_telescopes_and_results_arrive_discounted(gamma, scripted):
    """Over each dragon's whole trajectory, sum_i gamma^i r_i equals
    gamma^K * result - phi(first decision), K the rounds from its first
    decision to the end: the shaping only ever subtracts the start."""
    options = dict(OPTIONS, gamma=gamma, shaping=0.5, queen_weight=0.5, scripted_frac=scripted)
    n = 16
    env = bccore.BatchEnv(n, 3, options)
    rng = np.random.default_rng(3)
    records = run(env, 1500, rng)

    rows = {}       # row -> (slot, episode, dragon, team, round, phi)
    reward = {}     # row -> reward of the transition it starts
    nxt = {}        # row -> next row of the same dragon, or None at the end
    results = {}    # (slot, episode) -> (outcome, rounds)
    for t, (buf, actions, out) in enumerate(records):
        for slot in range(n):
            row = t * n + slot
            did, team, round_num, episode = (int(v) for v in buf["info"][slot])
            rows[row] = (slot, episode, did, team, round_num, phi(buf["priv"][slot]))
            prev = int(buf["prev_row"][slot])
            if prev >= 0:
                reward[prev] = float(buf["prev_reward"][slot])
                nxt[prev] = row
        for row, r, boot in zip(out["done_row"], out["done_reward"], out["done_boot"]):
            assert boot < 0  # max_rounds is the real 500: nothing is cut
            reward[int(row)] = float(r)
            nxt[int(row)] = None
        e = out["episodes"]
        for i in range(len(e["slot"])):
            slot = int(e["slot"][i])
            episode = int(buf["info"][slot][3]) - 1
            results[(slot, episode)] = (int(e["outcome"][i]), int(e["rounds"][i]))

    starts = set(rows) - set(v for v in nxt.values() if v is not None)
    checked = 0
    for start in starts:
        slot, episode, did, team, round0, phi0 = rows[start]
        if (slot, episode) not in results:
            continue
        total, discount, row = 0.0, 1.0, start
        while row is not None:
            assert row in reward, "every transition of a finished game completes"
            total += discount * reward[row]
            discount *= gamma
            row = nxt[row]
        outcome, rounds = results[(slot, episode)]
        result = 0.0 if outcome == 2 else (1.0 if outcome == team else -1.0)
        expected = gamma ** (rounds - round0) * result - phi0
        assert total == pytest.approx(expected, abs=2e-4), (rows[start], total, expected)
        checked += 1
    assert checked > 200


def test_training_horizon_cuts_with_bootstrap_views():
    options = dict(OPTIONS, max_rounds=12)
    env = bccore.BatchEnv(1, 0, options)
    rng = np.random.default_rng(5)
    env.set_next_level(0, 77)
    records = run(env, 600, rng, split_bias=0.1, plan=lambda t: {0: 1000 + t})
    seeds = episode_seeds(records, 77, 1000)
    cuts = [k for k, (buf, actions, out) in enumerate(records)
            if len(out["episodes"]["slot"]) and out["episodes"]["outcome"][0] == 3]
    assert cuts, "some game should reach the horizon"
    for k in cuts[:3]:
        out = records[k][2]
        assert out["episodes"]["rounds"][0] == 12
        episode = int(records[k - 1][0]["info"][0][3])
        decisions = [(b, int(a[0])) for b, a, o in records[:k] if int(b["info"][0][3]) == episode]
        match = replay(seeds[episode], options, decisions)
        # The engine has started round 12; the environment cut the game there
        # and gave every living dragon the view it would have had.
        assert match.round == 12 and match.current() is not None
        alive = [d[0] for d in match.dragons() if d[2]]
        boots = out["done_boot"]
        assert len(alive) == int((boots >= 0).sum()) == len(out["boot_obs"])
        views = sorted(bytes(match.features(d)[0]) for d in alive)
        assert views == sorted(bytes(o) for o in out["boot_obs"])


def test_levels_are_reproducible_by_seed():
    for seed in (1, 2, 2**63 + 5):
        assert bccore.level_map(seed, OPTIONS) == bccore.level_map(seed, OPTIONS)
    assert bccore.level_map(1, OPTIONS) != bccore.level_map(2, OPTIONS)
    text = bccore.generate_map(16, 16, "xy", seed=4)
    mixed = dict(OPTIONS, maps=[text], map_prob=1.0)
    assert bccore.level_map(9, mixed)[0] == text and bccore.level_map(9, mixed)[2] == 0


def test_threads_do_not_change_the_games():
    def play(threads):
        env = bccore.BatchEnv(24, 11, dict(OPTIONS, threads=threads))
        rng = np.random.default_rng(0)
        records = run(env, 200, rng)
        return [(b["obs"].tobytes(), b["prev_reward"].tobytes(), o["done_row"].tobytes()) for b, a, o in records]

    assert play(1) == play(4)


def test_outputs_must_be_the_callers_own_arrays():
    env = bccore.BatchEnv(4, 0, OPTIONS)
    buf = Buffers(4)
    with pytest.raises(TypeError):
        env.reset(buf.obs.astype(np.int16), buf.mask, buf.priv, buf.prev_row, buf.prev_reward, buf.info)
    with pytest.raises(TypeError):
        env.reset(np.asfortranarray(buf.obs), buf.mask, buf.priv, buf.prev_row, buf.prev_reward, buf.info)
    env.reset(*buf.args())
    assert buf.obs.any()


def test_scripted_slots_only_ask_the_learner():
    """In the scripted share of the slots, one team is played inside the
    environment by the random-safe player; only the other team's decisions
    come out, and the team the learner plays alternates by episode."""
    n = 8
    env = bccore.BatchEnv(n, 5, dict(OPTIONS, scripted_frac=0.5))
    rng = np.random.default_rng(2)
    records = run(env, 600, rng)
    scripted_slots = range(n // 2, n)
    episodes_seen = set()
    for buf, actions, out in records:
        for slot in scripted_slots:
            did, team, round_num, episode = (int(v) for v in buf["info"][slot])
            assert team == (slot + episode) % 2
            episodes_seen.add((slot, episode))
        e = out["episodes"]
        for i in range(len(e["slot"])):
            slot = int(e["slot"][i])
            if slot in scripted_slots:
                assert e["scripted_team"][i] in (0, 1)
            else:
                assert e["scripted_team"][i] == -1
    assert len({episode % 2 for slot, episode in episodes_seen}) == 2
