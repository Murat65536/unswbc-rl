"""PPO on the C++ batched environment (bccore.BatchEnv).

Every step, each game slot has one decision outstanding: the dragon whose
turn it is. The actor acts for all of them in one batch; the environment
plays the turns and writes the next decisions straight into this rollout's
tensors. Rows (one per decision) are linked by the environment: a dragon's
next decision names its previous row and that transition's reward, and
transitions that end without a next decision (the game ended, or the
training horizon cut it) arrive as completions.

A row is trained on once its transition has completed. Rows still waiting
when a rollout ends (a living dragon's last decision, or a dead dragon
waiting for its game's result) are carried into the next rollout and
trained there. Advantages are GAE(lambda) along each dragon's own chain of
decisions, cut at the rollout's edge (the next decision's value still
bootstraps the last step).

League: a fraction of the slots pit the learner, playing one team, against
a frozen earlier snapshot playing the other; only the learner's decisions
are trained on. Level replay (rl/plr.py) picks the level each slot plays
next.
"""
from __future__ import annotations

import copy
import time
from collections import defaultdict

import numpy as np
import torch
from torch.distributions import Categorical

import bccore

from .plr import LevelReplay
from .policy import Actor, Critic, masked_logits

OBS = bccore.OBS_SIZE
ACTIONS = bccore.NUM_ACTIONS
PRIV = bccore.NUM_PRIVILEGED
INFO = bccore.NUM_INFO


class Rollout:
    """The tensors the environment writes into, for T steps of N slots.
    Index t + 1 holds the decisions step t produced; index 0 the decisions
    the rollout starts from (the previous rollout's last ones)."""

    def __init__(self, steps: int, slots: int):
        self.steps, self.slots = steps, slots
        self.obs = torch.zeros((steps + 1, slots, OBS), dtype=torch.uint8)
        self.mask = torch.zeros((steps + 1, slots, ACTIONS), dtype=torch.uint8)
        self.priv = torch.zeros((steps + 1, slots, PRIV), dtype=torch.float32)
        self.prev_row = torch.zeros((steps + 1, slots), dtype=torch.int64)
        self.prev_reward = torch.zeros((steps + 1, slots), dtype=torch.float32)
        self.info = torch.zeros((steps + 1, slots, INFO), dtype=torch.int32)
        self.action = torch.zeros((steps, slots), dtype=torch.int64)
        self.logp = torch.zeros((steps, slots), dtype=torch.float32)
        self.value = torch.zeros((steps, slots), dtype=torch.float32)
        self.learner = torch.zeros((steps, slots), dtype=torch.bool)

    def outputs(self, t: int):
        return (self.obs[t], self.mask[t], self.priv[t], self.prev_row[t], self.prev_reward[t], self.info[t])

    def roll(self):
        """The last decisions become the first of the next rollout."""
        for name in ("obs", "mask", "priv", "prev_row", "prev_reward", "info"):
            getattr(self, name)[0].copy_(getattr(self, name)[self.steps])


class Carry:
    """Learner rows from earlier rollouts whose transitions are still open."""

    FIELDS = ("obs", "mask", "priv", "action", "logp", "value")

    def __init__(self):
        self.rows = np.zeros(0, dtype=np.int64)
        self.data = {
            "obs": torch.zeros((0, OBS), dtype=torch.uint8),
            "mask": torch.zeros((0, ACTIONS), dtype=torch.uint8),
            "priv": torch.zeros((0, PRIV)),
            "action": torch.zeros(0, dtype=torch.int64),
            "logp": torch.zeros(0),
            "value": torch.zeros(0),
        }
        self.level = np.zeros(0, dtype=np.uint64)

    def __len__(self):
        return len(self.rows)

    def find(self, rows: np.ndarray) -> np.ndarray:
        """Index of each row in the carry, or -1."""
        if len(self.rows) == 0 or len(rows) == 0:
            return np.full(len(rows), -1, dtype=np.int64)
        at = np.searchsorted(self.rows, rows)
        at = np.minimum(at, len(self.rows) - 1)
        return np.where(self.rows[at] == rows, at, -1)


class Trainer:
    def __init__(self, args, device: str = "cpu"):
        self.args = args
        self.device = device
        self.rng = np.random.default_rng(args.seed)
        torch.manual_seed(args.seed)

        options = env_options(args)
        self.env = bccore.BatchEnv(args.games, args.seed, options)
        scales = bccore.feature_scales()
        hidden = tuple(args.hidden)
        self.actor = Actor(OBS, ACTIONS, scales, hidden).to(device)
        self.critic = Critic(OBS, PRIV, scales, tuple(args.critic_hidden)).to(device)
        self.optimizer = torch.optim.Adam(
            list(self.actor.parameters()) + list(self.critic.parameters()), lr=args.lr, eps=1e-5)

        self.plr = LevelReplay(self.rng, capacity=args.plr_capacity, replay_prob=args.plr_replay,
                               temperature=args.plr_temperature, staleness=args.plr_staleness) \
            if args.plr_replay > 0 else None
        n = args.games
        self.slot_level = np.zeros(n, dtype=np.uint64)
        self.slot_next = np.zeros(n, dtype=np.uint64)
        self.slot_episode = np.ones(n, dtype=np.int64)
        # The level of each slot's recent episodes, by episode number mod 16.
        self.level_ring = np.zeros((n, 16), dtype=np.uint64)
        for slot in range(n):
            self.slot_level[slot] = self._next_level()
            self.level_ring[slot, 1] = self.slot_level[slot]
            self.env.set_next_level(slot, int(self.slot_level[slot]))
        self.league_slots = int(round(n * args.league_frac))
        if self.league_slots + int(round(n * args.scripted_frac)) > n:
            raise ValueError("--league-frac and --scripted-frac together exceed the slots")
        self.snapshots: list[dict] = []
        self.opponent = None

        self.rollout = Rollout(args.rollout, n)
        self.env.reset(*[x for x in self.rollout.outputs(0)])
        for slot in range(n):
            self._queue_next(slot)
        self.carry = Carry()
        self.iteration = 0
        self.decisions = 0

    # -- levels ---------------------------------------------------------------

    def _next_level(self) -> int:
        if self.plr is not None:
            return self.plr.sample()
        return int(self.rng.integers(0, 2 ** 63, dtype=np.int64))

    def _queue_next(self, slot: int):
        self.slot_next[slot] = self._next_level()
        self.env.set_next_level(slot, int(self.slot_next[slot]))

    # -- acting ---------------------------------------------------------------

    def _learner_rows(self, info: torch.Tensor) -> torch.Tensor:
        """Which of these decisions the learner makes (the rest: the league
        opponent, in the league slots, playing the other team)."""
        learner = torch.ones(info.shape[0], dtype=torch.bool)
        if self.opponent is not None and self.league_slots:
            k = self.league_slots
            slots = torch.arange(k)
            learner_team = (slots + info[:k, 3]) % 2
            learner[:k] = info[:k, 1] == learner_team
        return learner

    @torch.no_grad()
    def _act(self, t: int):
        r = self.rollout
        obs = r.obs[t].to(self.device)
        mask = r.mask[t].to(self.device)
        self.actor.eval()
        logits = masked_logits(self.actor(obs), mask)
        dist = Categorical(logits=logits)
        action = dist.sample()
        learner = self._learner_rows(r.info[t])
        if not learner.all():
            opp = ~learner
            opp_logits = masked_logits(self.opponent(obs[opp]), mask[opp])
            action[opp] = Categorical(logits=opp_logits).sample()
        r.action[t] = action.cpu()
        r.logp[t] = dist.log_prob(action).cpu()
        r.value[t] = self.critic(obs, r.priv[t].to(self.device)).cpu()
        r.learner[t] = learner

    def collect(self):
        """One rollout: T environment steps."""
        args, r, n = self.args, self.rollout, self.args.games
        base = self.env.step_index * n  # row id of rollout row (0, 0)
        done_rows, done_rewards, done_boot_values = [], [], []
        episodes = []
        t_env = 0.0
        for t in range(args.rollout):
            self._act(t)
            actions = r.action[t].to(torch.int32)
            start = time.perf_counter()
            out = self.env.step(actions, *r.outputs(t + 1))
            t_env += time.perf_counter() - start
            if len(out["done_row"]):
                done_rows.append(out["done_row"])
                done_rewards.append(out["done_reward"])
                boot = out["done_boot"]
                values = np.zeros(len(boot), dtype=np.float32)
                if (boot >= 0).any():
                    with torch.no_grad():
                        v = self.critic(torch.from_numpy(out["boot_obs"]).to(self.device),
                                        torch.from_numpy(out["boot_priv"]).to(self.device)).cpu().numpy()
                    values[boot >= 0] = args.gamma * v[boot[boot >= 0]]
                done_boot_values.append(values)
            e = out["episodes"]
            for i in range(len(e["slot"])):
                slot = int(e["slot"][i])
                episodes.append({k: e[k][i].item() for k in e} | {"learner_team": self._episode_team(slot)})
                self.slot_level[slot] = self.slot_next[slot]
                self.slot_episode[slot] += 1
                self.level_ring[slot, self.slot_episode[slot] % 16] = self.slot_level[slot]
                self._queue_next(slot)
        with torch.no_grad():
            self.critic.eval()
            last_value = self.critic(r.obs[args.rollout].to(self.device), r.priv[args.rollout].to(self.device)).cpu()
        self.decisions += args.rollout * n
        cat = lambda xs, dt: np.concatenate(xs) if xs else np.zeros(0, dtype=dt)  # noqa: E731
        return {
            "base": base,
            "done_row": cat(done_rows, np.int64),
            "done_reward": cat(done_rewards, np.float32),
            "done_boot_value": cat(done_boot_values, np.float32),
            "last_value": last_value,
            "episodes": episodes,
            "env_seconds": t_env,
        }

    def _episode_team(self, slot: int):
        """The team the learner played in the episode that just ended there
        (None in self-play slots, where it played both)."""
        if self.opponent is None or slot >= self.league_slots:
            return None
        return int((slot + self.slot_episode[slot]) % 2)

    # -- advantages ------------------------------------------------------------

    def advantages(self, batch: dict) -> dict:
        """Completes rows, computes GAE, and returns the training set."""
        args, r = self.args, self.rollout
        T, n = args.rollout, args.games
        base = batch["base"]
        gamma, lam = args.gamma, args.gae_lambda

        reward = torch.zeros((T, n))
        done = torch.zeros((T, n), dtype=torch.bool)          # completed this rollout
        next_value = torch.zeros((T, n))                        # gamma * V(next), 0 at the end
        next_t = torch.full((T, n), -1, dtype=torch.int64)      # step of the next decision, if one of ours

        carry = self.carry
        c_done = np.zeros(len(carry), dtype=bool)
        c_reward = np.zeros(len(carry), dtype=np.float32)
        c_next_value = np.zeros(len(carry), dtype=np.float32)
        c_next_t = np.full(len(carry), -1, dtype=np.int64)
        c_slot = np.zeros(len(carry), dtype=np.int64)

        values = torch.cat([r.value, batch["last_value"].unsqueeze(0)])  # [T + 1, n]

        # Decisions that complete an earlier one: steps 1..T name their previous row.
        prev = r.prev_row[1:].numpy()           # [T, n]
        prev_reward = r.prev_reward[1:].numpy()
        ts, slots = np.nonzero(prev >= 0)
        rows = prev[ts, slots]
        nt = ts + 1                              # the step of the completing decision
        local = rows - base
        here = (local >= 0) & (local < T * n)
        lt = torch.from_numpy(local[here] // n)
        ls = torch.from_numpy(local[here] % n)
        nth = torch.from_numpy(nt[here])
        reward[lt, ls] = torch.from_numpy(prev_reward[ts[here], slots[here]])
        done[lt, ls] = True
        next_value[lt, ls] = gamma * values[nth, ls]
        next_t[lt, ls] = nth
        old = ~here
        at = carry.find(rows[old])
        ok = at >= 0
        c_done[at[ok]] = True
        c_reward[at[ok]] = prev_reward[ts[old][ok], slots[old][ok]]
        c_next_value[at[ok]] = gamma * values[torch.from_numpy(nt[old][ok]), torch.from_numpy(slots[old][ok])].numpy()
        c_next_t[at[ok]] = nt[old][ok]
        c_slot[at[ok]] = slots[old][ok]

        # Transitions that ended with no next decision.
        rows = batch["done_row"]
        local = rows - base
        here = (local >= 0) & (local < T * n)
        lt = torch.from_numpy(local[here] // n)
        ls = torch.from_numpy(local[here] % n)
        reward[lt, ls] = torch.from_numpy(batch["done_reward"][here])
        next_value[lt, ls] = torch.from_numpy(batch["done_boot_value"][here])
        done[lt, ls] = True
        at = carry.find(rows[~here])
        ok = at >= 0
        c_done[at[ok]] = True
        c_reward[at[ok]] = batch["done_reward"][~here][ok]
        c_next_value[at[ok]] = batch["done_boot_value"][~here][ok]

        # GAE along each dragon's chain, newest first. A row whose next
        # decision is still open (or is not in this rollout) cuts the chain.
        adv = torch.zeros((T + 1, n))
        chain = done.clone()
        slots_idx = torch.arange(n)
        for t in range(T - 1, -1, -1):
            nxt = next_t[t]
            has = (nxt >= 0) & (nxt < T)
            follow = torch.zeros(n)
            follow[has] = adv[nxt[has], slots_idx[has]] * chain[nxt[has], slots_idx[has]].float()
            delta = reward[t] + next_value[t] - r.value[t]
            adv[t] = torch.where(done[t], delta + gamma * lam * follow, torch.zeros(n))
        adv = adv[:T]

        c_adv = np.zeros(len(carry), dtype=np.float32)
        if len(carry):
            follow = np.zeros(len(carry), dtype=np.float32)
            has = (c_next_t >= 0) & (c_next_t < T)
            ct, cs = torch.from_numpy(c_next_t[has]), torch.from_numpy(c_slot[has])
            follow[has] = (adv[ct, cs] * done[ct, cs].float()).numpy()
            c_adv = c_reward + c_next_value - carry.data["value"].numpy() + gamma * lam * follow
            c_adv = np.where(c_done, c_adv, 0.0).astype(np.float32)

        # The training set: completed learner rows, this rollout's and carried.
        train = done & r.learner
        sel = train.flatten()
        batch_out = {
            "obs": torch.cat([r.obs[:T].reshape(T * n, OBS)[sel], carry.data["obs"][c_done]]),
            "mask": torch.cat([r.mask[:T].reshape(T * n, ACTIONS)[sel], carry.data["mask"][c_done]]),
            "priv": torch.cat([r.priv[:T].reshape(T * n, PRIV)[sel], carry.data["priv"][c_done]]),
            "action": torch.cat([r.action.flatten()[sel], carry.data["action"][c_done]]),
            "logp": torch.cat([r.logp.flatten()[sel], carry.data["logp"][c_done]]),
            "value": torch.cat([r.value.flatten()[sel], carry.data["value"][c_done]]),
            "adv": torch.cat([adv.flatten()[sel], torch.from_numpy(c_adv[c_done])]),
        }
        batch_out["return"] = batch_out["adv"] + batch_out["value"]
        batch_out["row"] = torch.cat([(base + torch.arange(T * n))[sel], torch.from_numpy(carry.rows[c_done])])

        # Level scores for replay: mean |advantage| per level, over this
        # rollout's completed learner rows.
        if self.plr is not None:
            level_now = self._row_levels()
            lv = level_now[train.numpy()]
            a = adv[train].abs().numpy()
            if len(lv):
                uniq, inv = np.unique(lv, return_inverse=True)
                sums = np.bincount(inv, weights=a)
                counts = np.bincount(inv)
                for seed, s, c in zip(uniq, sums, counts):
                    self.plr.update(int(seed), float(s / c))

        # Open rows move to the carry: this rollout's learner rows still
        # waiting, and carried rows still waiting.
        keep_new = (~done & r.learner).flatten()
        keep_old = ~c_done
        new_rows = (base + torch.arange(T * n))[keep_new].numpy()
        merged_rows = np.concatenate([carry.rows[keep_old], new_rows])
        order = np.argsort(merged_rows, kind="stable")
        data = {}
        for name, source in (("obs", r.obs[:T].reshape(T * n, OBS)), ("mask", r.mask[:T].reshape(T * n, ACTIONS)),
                             ("priv", r.priv[:T].reshape(T * n, PRIV)), ("action", r.action.flatten()),
                             ("logp", r.logp.flatten()), ("value", r.value.flatten())):
            data[name] = torch.cat([carry.data[name][torch.from_numpy(keep_old)], source[keep_new]])[order]
        self.carry.rows = merged_rows[order]
        self.carry.data = data
        batch_out["carried"] = int(len(self.carry))
        batch_out["trained_from_carry"] = int(c_done.sum())
        return batch_out

    def _row_levels(self) -> np.ndarray:
        """The level seed of each rollout row [T, n]."""
        T, n = self.args.rollout, self.args.games
        episode = self.rollout.info[:T, :, 3].numpy().astype(np.int64)
        return self.level_ring[np.arange(n)[None, :], episode % 16]

    # -- update -----------------------------------------------------------------

    def update(self, data: dict) -> dict:
        args = self.args
        stats = defaultdict(list)
        total = len(data["adv"])
        if total == 0:
            return stats
        adv = data["adv"]
        adv = (adv - adv.mean()) / (adv.std() + 1e-8)
        self.actor.train()
        self.critic.train()
        for _ in range(args.epochs):
            order = torch.randperm(total)
            for start in range(0, total, args.minibatch):
                mb = order[start:start + args.minibatch]
                obs = data["obs"][mb].to(self.device)
                mask = data["mask"][mb].to(self.device)
                logits = masked_logits(self.actor(obs), mask)
                dist = Categorical(logits=logits)
                logp = dist.log_prob(data["action"][mb].to(self.device))
                ratio = torch.exp(logp - data["logp"][mb].to(self.device))
                a = adv[mb].to(self.device)
                policy_loss = -torch.min(ratio * a, torch.clamp(ratio, 1 - args.clip, 1 + args.clip) * a).mean()
                entropy = dist.entropy().mean()
                value = self.critic(obs, data["priv"][mb].to(self.device))
                ret = data["return"][mb].to(self.device)
                old_value = data["value"][mb].to(self.device)
                clipped = old_value + torch.clamp(value - old_value, -args.value_clip, args.value_clip)
                value_loss = 0.5 * torch.max((value - ret) ** 2, (clipped - ret) ** 2).mean()
                loss = policy_loss + args.vf_coef * value_loss - args.ent_coef * entropy
                self.optimizer.zero_grad(set_to_none=True)
                loss.backward()
                torch.nn.utils.clip_grad_norm_(list(self.actor.parameters()) + list(self.critic.parameters()),
                                               args.max_grad_norm)
                self.optimizer.step()
                with torch.no_grad():
                    stats["policy_loss"].append(policy_loss.item())
                    stats["value_loss"].append(value_loss.item())
                    stats["entropy"].append(entropy.item())
                    stats["approx_kl"].append(((ratio - 1) - torch.log(ratio)).mean().item())
                    stats["clip_frac"].append(((ratio - 1).abs() > args.clip).float().mean().item())
        return stats

    # -- league -------------------------------------------------------------------

    def maybe_snapshot(self):
        args = self.args
        if args.league_frac <= 0:
            return
        if self.iteration % args.snapshot_every == 0:
            self.snapshots.append(copy.deepcopy(self.actor.state_dict()))
            self.snapshots = self.snapshots[-args.league_size:]
        if self.snapshots and self.iteration % args.opponent_every == 0:
            # Recent snapshots more often than old ones.
            k = len(self.snapshots)
            weights = np.arange(1, k + 1, dtype=np.float64)
            pick = int(self.rng.choice(k, p=weights / weights.sum()))
            opponent = Actor(OBS, ACTIONS, bccore.feature_scales(), tuple(args.hidden)).to(self.device)
            opponent.load_state_dict(self.snapshots[pick])
            opponent.qat = self.actor.qat
            opponent.eval()
            self.opponent = opponent

    # -- one iteration ------------------------------------------------------------

    def iterate(self) -> dict:
        args = self.args
        self.iteration += 1
        if args.qat_after >= 0 and self.iteration > args.qat_after:
            self.actor.qat = True
        self.maybe_snapshot()
        start = time.perf_counter()
        batch = self.collect()
        data = self.advantages(batch)
        rollout_seconds = time.perf_counter() - start
        stats = self.update(data)
        self.rollout.roll()
        seconds = time.perf_counter() - start

        episodes = batch["episodes"]
        out = {
            "iteration": self.iteration,
            "decisions": self.decisions,
            "trained_rows": len(data["adv"]),
            "carried": data["carried"],
            "episodes": len(episodes),
            "seconds": seconds,
            "rollout_seconds": rollout_seconds,
            "env_seconds": batch["env_seconds"],
            "decisions_per_second": args.rollout * args.games / max(rollout_seconds, 1e-9),
        }
        for key, values in stats.items():
            out[key] = float(np.mean(values))
        if episodes:
            out["mean_rounds"] = float(np.mean([e["rounds"] for e in episodes]))
            out["mean_total_length"] = float(np.mean([e["total_a"] + e["total_b"] for e in episodes]) / 2)
            out["draw_rate"] = float(np.mean([e["outcome"] == 2 for e in episodes]))
            league = [e for e in episodes if e["learner_team"] is not None and e["outcome"] in (0, 1, 2)]
            if league:
                out["league_win_rate"] = float(np.mean([e["outcome"] == e["learner_team"] for e in league]))
                out["league_games"] = len(league)
            scripted = [e for e in episodes if e["scripted_team"] >= 0 and e["outcome"] in (0, 1, 2)]
            if scripted:
                out["scripted_win_rate"] = float(np.mean([e["outcome"] == 1 - e["scripted_team"] for e in scripted]))
                out["scripted_games"] = len(scripted)
        if self.plr is not None:
            out["plr_levels"] = self.plr.size
        return out

    # -- checkpoints ----------------------------------------------------------------

    def state_dict(self) -> dict:
        return {
            "actor": self.actor.state_dict(),
            "critic": self.critic.state_dict(),
            "optimizer": self.optimizer.state_dict(),
            "iteration": self.iteration,
            "decisions": self.decisions,
            "qat": self.actor.qat,
            "hidden": list(self.actor.hidden),
            "snapshots": self.snapshots,
            "plr": self.plr.state_dict() if self.plr is not None else None,
            "args": vars(self.args),
            "obs_size": OBS,
            "num_actions": ACTIONS,
            "mask_level": self.args.mask_level,
        }

    def load_state_dict(self, state: dict):
        self.actor.load_state_dict(state["actor"])
        self.critic.load_state_dict(state["critic"])
        self.optimizer.load_state_dict(state["optimizer"])
        self.iteration = state["iteration"]
        self.decisions = state.get("decisions", 0)
        self.actor.qat = state.get("qat", False)
        self.snapshots = state.get("snapshots", [])
        if self.plr is not None and state.get("plr"):
            self.plr.load_state_dict(state["plr"])


def env_options(args) -> dict:
    options = {
        "threads": args.threads,
        "mask_level": args.mask_level,
        "max_rounds": args.max_rounds,
        "gamma": args.gamma,
        "shaping": args.shaping,
        "queen_weight": args.queen_weight,
        "width": tuple(args.width),
        "height": tuple(args.height),
        "dragons_per_team": tuple(args.dragons_per_team),
        "kelp": tuple(args.kelp),
        "portal_pairs": tuple(args.portal_pairs),
        "scripted_frac": args.scripted_frac,
    }
    if args.official_maps > 0:
        options["maps"] = official_maps()
        options["map_prob"] = args.official_maps
    return options


def official_maps() -> list[str]:
    import pathlib

    try:
        import unswbc
    except ImportError as error:
        raise SystemExit("--official-maps needs the unswbc toolkit installed (it ships the maps)") from error
    folder = pathlib.Path(unswbc.__file__).resolve().parent / "templates" / "maps"
    return [p.read_text() for p in sorted(folder.glob("*.map"))]
