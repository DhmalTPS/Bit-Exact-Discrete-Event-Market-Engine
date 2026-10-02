"""Reference-semantics model of the Track-3 ABIDES baseline (pinned f9cbe51 + 4 patches + abides_fork).

A direct, minimal re-statement of exactly the ABIDES code paths the abides_fork scenario agents
exercise, using numpy's legacy RandomState so every random draw is identical by construction.
It is the executable specification the C++ engine is differentially tested against; it is NOT the
submission. Every rule below cites the ABIDES behaviour it reproduces.

Message ids, order ids, the kernel priority key (time, sender, recipient, message_id), per-agent
busy times / computation delays, latency draws, oracle draws and log ordering all follow ABIDES.
"""
from __future__ import annotations

import heapq
import math
from math import exp, sqrt
from typing import Any

import numpy as np

DATE_NS = 1612483200000000000            # pd.to_datetime("20210205").value
MKT_OPEN_OFFSET = 34200 * 10**9          # 09:30:00
DEFAULT_COMPUTATION_DELAY = 50

# message kinds (value = ABIDES class name, used verbatim in the ledger)
WAKEUP = "WakeupMsg"
MKT_CLOSE_PRICE_REQ = "MarketClosePriceRequestMsg"
MKT_HOURS_REQ = "MarketHoursRequestMsg"
MKT_HOURS = "MarketHoursMsg"
MKT_CLOSE_PRICE = "MarketClosePriceMsg"
MKT_CLOSED = "MarketClosedMsg"
QUERY_SPREAD = "QuerySpreadMsg"
QUERY_SPREAD_RESP = "QuerySpreadResponseMsg"
LIMIT_ORDER = "LimitOrderMsg"
CANCEL_ORDER = "CancelOrderMsg"
ORDER_ACCEPTED = "OrderAcceptedMsg"
ORDER_EXECUTED = "OrderExecutedMsg"
ORDER_CANCELLED = "OrderCancelledMsg"
ORDER_MSGS = (LIMIT_ORDER, CANCEL_ORDER)          # OrderMsg subclasses the agents use
QUERY_MSGS = (QUERY_SPREAD,)                      # QueryMsg subclasses the agents use


class Order:
    __slots__ = ("agent_id", "order_id", "is_bid", "qty", "price")

    def __init__(self, agent_id, order_id, is_bid, qty, price):
        self.agent_id, self.order_id, self.is_bid, self.qty, self.price = agent_id, order_id, is_bid, qty, price

    def copy(self):
        return Order(self.agent_id, self.order_id, self.is_bid, self.qty, self.price)


class Msg:
    __slots__ = ("kind", "mid", "order", "payload", "led")

    def __init__(self, sim, kind, order=None, payload=None):
        self.kind, self.order, self.payload = kind, order, payload
        self.mid = sim.next_msg_id
        sim.next_msg_id += 1
        self.led = None  # per-recipient ledger info: dict recipient -> (t_send, t_recv, causal)


class LatencyModel:
    def __init__(self, cfg, rs):
        p = cfg.get("params", {})
        self.model = str(cfg.get("model", "deterministic"))
        self.mean = float(p.get("mean_ns", 0.0))
        self.sigma = float(p.get("sigma", 0.0))
        self.lo = float(p.get("min_ns", 0.0))
        self.hi = float(p.get("max_ns", 1e12))
        self.alpha = float(p.get("alpha", 1.5))
        self.mu = float(np.log(self.mean)) if self.mean > 0 else 0.0
        self.rs = rs

    def get(self, s, r):
        if s == r:
            return 0
        if self.model == "log_normal":
            v = self.rs.lognormal(mean=self.mu, sigma=self.sigma)
        elif self.model == "uniform":
            v = self.rs.uniform(self.lo, self.hi)
        elif self.model == "pareto":
            base = self.lo if self.lo > 0 else 1.0
            v = base * (1.0 + self.rs.pareto(self.alpha))
        else:
            v = self.mean
        return int(round(float(np.clip(v, self.lo, self.hi))))


class Oracle:
    """SparseMeanRevertingOracle + oracle_scheduled_jump patch (single symbol)."""

    def __init__(self, mkt_open, mkt_close, r_bar, kappa, fund_vol, lam, ms_mean, ms_var, rs, jumps):
        self.mkt_open, self.mkt_close = mkt_open, mkt_close
        self.r_bar, self.kappa, self.fund_vol = r_bar, kappa, fund_vol
        self.lam, self.ms_mean, self.ms_var = lam, ms_mean, ms_var
        self.rs, self.jumps = rs, jumps
        self.r = (mkt_open, r_bar)
        ms_dt = np.random.exponential(scale=1.0 / lam)           # GLOBAL rng
        self.mst = self.mkt_open + ms_dt                          # float
        msv = rs.normal(loc=ms_mean, scale=sqrt(ms_var))
        self.msv = msv if rs.randint(2) == 0 else -msv

    def _compute(self, ts, v_adj, pt, pv):
        d = ts - pt
        mu, gamma, theta = self.r_bar, self.kappa, self.fund_vol
        v = self.rs.normal(loc=mu + (pv - mu) * (exp(-gamma * d)),
                           scale=sqrt(((theta ** 2) / (2 * gamma)) * (1 - exp(-2 * gamma * d))))
        v += v_adj
        v = max(0, v)
        v = int(round(v))
        for j in self.jumps:
            if not j["_consumed"] and ts >= j["time_ns"]:
                v = max(0, v + int(j["magnitude"]))
                j["_consumed"] = True
        self.r = (ts, v)
        return v

    def advance(self, t):
        pt, pv = self.r
        if t <= pt:
            return pv
        mst, msv = self.mst, self.msv
        while mst < t:
            v = self._compute(mst, msv, pt, pv)
            pt, pv = mst, v
            mst = pt + int(np.random.exponential(scale=1.0 / self.lam))   # GLOBAL rng
            msv = self.rs.normal(loc=self.ms_mean, scale=sqrt(self.ms_var))
            msv = msv if self.rs.randint(2) == 0 else -msv
            self.mst, self.msv = mst, msv
        return self._compute(t, 0, pt, pv)

    def observe(self, t, rs, sigma_n=1000.0):
        r_t = self.advance(self.mkt_close - 1 if t >= self.mkt_close else t)
        if sigma_n == 0:
            return r_t
        return int(round(rs.normal(loc=r_t, scale=sqrt(sigma_n))))


class Level:
    __slots__ = ("price", "orders")

    def __init__(self, order):
        self.price = order.price
        self.orders = [order]


class Sim:
    def __init__(self, scenario: dict, seed: int | None = None):
        self.sc = scenario
        seed = int(scenario["seed"] if seed is None else seed)
        self.seed = seed
        np.random.seed(seed)
        draw = lambda: np.random.RandomState(seed=np.random.randint(low=0, high=2**32, dtype="uint64"))

        ex = scenario["exchange_config"]
        proto = bool(ex.get("protocol_enforcement", False))
        self.stp = str(ex["stp_policy"]) if proto and ex.get("stp_policy") else None
        self.ack_delay = int(ex.get("ack_delay_ns", 0)) if proto else 0
        self.ex_compute_delay = int(ex.get("compute_delay_ns", 0)) if proto else 0
        op = scenario["oracle_config"].get("params", {})
        self.ref_price = int(op.get("initial_price", 100_000))
        horizon = int(scenario["horizon_ns"])
        self.mkt_open = DATE_NS + MKT_OPEN_OFFSET
        self.mkt_close = self.mkt_open + horizon
        self.start_time = DATE_NS
        self.stop_time = self.mkt_close + 10**9

        kps = float(op.get("kappa", 0.0))
        kappa = kps / 1e9 if kps > 0 else 1.67e-16
        jr = float(op.get("jump_intensity", 0.0))
        lam = jr / 1e9 if jr > 0 else 2.77778e-18
        jumps = ([{"time_ns": self.mkt_open + int(op["scheduled_jump"]["time_ns"]),
                   "magnitude": int(op["scheduled_jump"]["magnitude"]), "_consumed": False}]
                 if op.get("scheduled_jump") else [])
        oracle_rs = draw()
        # The oracle is built with the 16:00 "oracle_close", not the scenario's mkt_close.
        oracle_close = DATE_NS + 57600 * 10**9
        self.oracle = Oracle(self.mkt_open, oracle_close, self.ref_price, kappa,
                             float(op.get("sigma", 5e-5)), lam,
                             float(op.get("jump_sigma", 0.0)) or 1000.0, 50_000, oracle_rs, jumps)
        draw()  # exchange random state (unused)

        # agents
        self.agents: list[dict[str, Any]] = [None]  # index 0 = exchange
        for cfg in scenario["agent_configs"]:
            typ = str(cfg["agent_type"])
            p = cfg.get("params", {})
            if "rebalance_interval_ns" in p:
                interval = int(p["rebalance_interval_ns"])
            else:
                hz = float(p.get("arrival_rate_hz", 1.0))
                interval = int(1e9 / hz) if hz > 0 else int(1e9)
            for _ in range(int(cfg["count"])):
                a = dict(type=typ, id=len(self.agents), interval=int(max(1, interval)), rs=draw(),
                         state="AWAITING_WAKEUP", mkt_open=None, mkt_close=None, mkt_closed=False,
                         first_wake=True, orders={}, bid=None, ask=None, hist=[])
                if typ == "NoiseTrader":
                    a.update(size_mean=float(p.get("order_size_mean", 10)), size_std=float(p.get("order_size_std", 2)),
                             offset=int(p.get("price_offset_ticks", 5)))
                elif typ == "MarketMaker":
                    a.update(spread=int(max(2, p.get("spread_ticks", 2))), depth=int(max(1, p.get("depth_levels", 3))),
                             size=int(max(1, p.get("size_per_level", 10))))
                elif typ == "ValueTrader":
                    a.update(size_mean=float(p.get("order_size_mean", 25)), thr=int(p.get("threshold_ticks", 2)))
                elif typ == "MomentumTrader":
                    a.update(size_mean=float(p.get("order_size_mean", 15)), thr=int(p.get("threshold_ticks", 2)),
                             lookback=int(max(1, p.get("lookback", 5))))
                else:
                    raise KeyError(typ)
                self.agents.append(a)
        n = len(self.agents)
        lat = scenario.get("latency_config")
        if not lat:
            raise NotImplementedError("scenario without latency_config")
        self.latency = LatencyModel(lat, draw())
        draw()  # kernel random state (unused)

        # kernel state
        self.next_msg_id = 1
        self.next_order_id = 0
        self.pq: list = []
        self.cur = self.start_time
        self.busy = [self.start_time] * n
        self.cdelay = [DEFAULT_COMPUTATION_DELAY] * n
        self.causal = None
        self.seq = 0
        self.ledger: list[tuple] = []
        # exchange
        self.bids: list[Level] = []
        self.asks: list[Level] = []
        self.close_subs: list[int] = []
        # logs
        self.order_events: list[tuple] = []   # (t, order_id, agent, type, is_bid, price, size)
        self.quote_events: list[tuple] = []   # (t, is_bid, price, size)

    # ---------------- kernel ----------------
    def set_wakeup(self, aid, t):
        if self.cur and t < self.cur:
            raise ValueError("wakeup in past")
        m = Msg(self, WAKEUP)
        heapq.heappush(self.pq, (t, aid, aid, m.mid, m))

    def send(self, s, r, m, delay=0):
        sent = self.cur + self.cdelay[s] + delay
        lat = self.latency.get(s, r)
        at = sent + int(lat)
        if m.led is None:
            m.led = {}
        m.led[r] = (sent, at, self.causal)
        heapq.heappush(self.pq, (at, s, r, m.mid, m))

    def run(self):
        # kernel_initializing: exchange wakes at mkt_close
        self.set_wakeup(0, self.mkt_close)
        # kernel_starting: every agent wakes at start_time
        for aid in range(len(self.agents)):
            self.set_wakeup(aid, self.start_time)
        self.cur = self.start_time
        while self.pq and self.cur and self.cur <= self.stop_time:
            t, s, r, mid, m = heapq.heappop(self.pq)
            self.cur = t
            if self.busy[r] > t:
                heapq.heappush(self.pq, (self.busy[r], s, r, mid, m))
                continue
            self.busy[r] = t
            if m.kind == WAKEUP:
                self.causal = m.mid
                self.ledger.append((self.seq, t, None, 0, r, r, m.mid, "AGENT_WAKEUP", None, None))
                self.seq += 1
                if r == 0:
                    self.ex_wakeup(t)
                else:
                    self.ag_wakeup(self.agents[r], t)
                self.busy[r] += self.cdelay[r]
            else:
                self.busy[r] += self.cdelay[r]
                self.causal = m.mid
                sent, at, cp = m.led[r]
                self.ledger.append((self.seq, at, sent, at - sent, s, r, m.mid, m.kind,
                                    m.order.order_id if m.order is not None else None, cp))
                self.seq += 1
                if r == 0:
                    self.ex_receive(t, s, m)
                else:
                    self.ag_receive(self.agents[r], t, s, m)

    # ---------------- exchange ----------------
    def ex_wakeup(self, t):
        if t >= self.mkt_close:
            m = Msg(self, MKT_CLOSE_PRICE)
            for a in self.close_subs:
                self.send(0, a, m)

    def ex_send(self, r, m):
        if m.kind in (ORDER_ACCEPTED, ORDER_CANCELLED, ORDER_EXECUTED):
            self.send(0, r, m, delay=self.ack_delay)
        else:
            self.send(0, r, m)

    def ex_receive(self, t, s, m):
        self.cdelay[0] = self.ex_compute_delay
        k = m.kind
        if t > self.mkt_close:
            if k in ORDER_MSGS:
                self.ex_send(s, Msg(self, MKT_CLOSED))
                return
            elif k in QUERY_MSGS:
                pass
            else:
                self.ex_send(s, Msg(self, MKT_CLOSED))
                return
        if k == MKT_HOURS_REQ:
            self.cdelay[0] = 0
            self.ex_send(s, Msg(self, MKT_HOURS))
        elif k == MKT_CLOSE_PRICE_REQ:
            self.close_subs.append(s)
        elif k == QUERY_SPREAD:
            bid = self.bids[0].price if self.bids else None
            ask = self.asks[0].price if self.asks else None
            self.ex_send(s, Msg(self, QUERY_SPREAD_RESP, payload=(bid, ask, t > self.mkt_close)))
        elif k == LIMIT_ORDER:
            self.handle_limit(m.order.copy())
        elif k == CANCEL_ORDER:
            self.cancel(m.order.copy())

    def handle_limit(self, o: Order):
        if o.qty <= 0 or o.price < 0:
            return
        while True:
            if self.stp:
                opp = self.asks if o.is_bid else self.bids
                if opp and self._match(opp[0], o):
                    resting = opp[0].orders[0]
                    if resting.agent_id == o.agent_id:
                        if self.stp == "cancel_oldest" and self.cancel(resting):
                            continue
                        if self.stp != "cancel_oldest":
                            self.ex_send(o.agent_id, Msg(self, ORDER_CANCELLED, order=o.copy()))
                            break
            matched = self.execute(o)
            if matched is not None:
                if o.qty <= 0:
                    break
            else:
                self.enter(o.copy())
                self.ex_send(o.agent_id, Msg(self, ORDER_ACCEPTED, order=o))
                break
        t = self.cur
        if self.bids:
            self.quote_events.append((t, True, self.bids[0].price, sum(x.qty for x in self.bids[0].orders)))
        if self.asks:
            self.quote_events.append((t, False, self.asks[0].price, sum(x.qty for x in self.asks[0].orders)))

    @staticmethod
    def _match(level, o):
        return o.price >= level.price if o.is_bid else o.price <= level.price

    def execute(self, o: Order):
        book = self.asks if o.is_bid else self.bids
        if not book or not self._match(book[0], o):
            return None
        head = book[0].orders[0]
        if o.qty >= head.qty:
            matched = book[0].orders.pop(0)
            if not book[0].orders:
                del book[0]
        else:
            matched = head.copy()
            matched.qty = o.qty
            head.qty -= matched.qty
        filled = o.copy()
        filled.qty = matched.qty
        o.qty -= filled.qty
        # matched.fill_price = matched.limit_price; filled.fill_price = same
        self.ex_send(matched.agent_id, Msg(self, ORDER_EXECUTED, order=matched, payload=matched.price))
        self.ex_send(o.agent_id, Msg(self, ORDER_EXECUTED, order=filled, payload=matched.price))
        return matched

    def enter(self, o: Order):
        book = self.bids if o.is_bid else self.asks
        better = (lambda lv: o.price > lv.price) if o.is_bid else (lambda lv: o.price < lv.price)
        if not book:
            book.append(Level(o))
        elif (o.price < book[-1].price) if o.is_bid else (o.price > book[-1].price):
            book.append(Level(o))
        else:
            for i, lv in enumerate(book):
                if better(lv):
                    book.insert(i, Level(o))
                    break
                elif lv.price == o.price:
                    lv.orders.append(o)
                    break

    def cancel(self, o: Order) -> bool:
        book = self.bids if o.is_bid else self.asks
        if not book:
            return False
        for i, lv in enumerate(book):
            if lv.price != o.price:
                continue
            for j, bo in enumerate(lv.orders):
                if bo.order_id == o.order_id:
                    c = lv.orders.pop(j)
                    if not lv.orders:
                        del book[i]
                    self.ex_send(o.agent_id, Msg(self, ORDER_CANCELLED, order=c))
                    return True
        return False

    # ---------------- trading agents ----------------
    def log(self, a, t, typ, o: Order, price=None):
        self.order_events.append((t, o.order_id, a["id"], typ, o.is_bid, o.price if price is None else price, o.qty))

    def ag_wakeup(self, a, t):
        if a["first_wake"]:
            a["first_wake"] = False
            self.send(a["id"], 0, Msg(self, MKT_CLOSE_PRICE_REQ))
        if a["mkt_open"] is None:
            self.send(a["id"], 0, Msg(self, MKT_HOURS_REQ))
        if not a["mkt_open"] or not a["mkt_close"] or a["mkt_closed"]:
            return
        self.set_wakeup(a["id"], t + a["interval"])
        self.send(a["id"], 0, Msg(self, QUERY_SPREAD))
        a["state"] = "AWAITING_SPREAD"

    def ag_receive(self, a, t, s, m):
        had = a["mkt_open"] is not None and a["mkt_close"] is not None
        k = m.kind
        if k == MKT_HOURS:
            a["mkt_open"], a["mkt_close"] = self.mkt_open, self.mkt_close
        elif k == MKT_CLOSE_PRICE:
            pass
        elif k == MKT_CLOSED:
            a["mkt_closed"] = True
        elif k == ORDER_EXECUTED:
            o = m.order
            self.log(a, t, "ORDER_EXECUTED", o, price=m.payload)
            if o.order_id in a["orders"]:
                mine = a["orders"][o.order_id]
                if o.qty >= mine.qty:
                    del a["orders"][o.order_id]
                else:
                    mine.qty -= o.qty
        elif k == ORDER_ACCEPTED:
            self.log(a, t, "ORDER_ACCEPTED", m.order)
        elif k == ORDER_CANCELLED:
            self.log(a, t, "ORDER_CANCELLED", m.order)
            a["orders"].pop(m.order.order_id, None)
        elif k == QUERY_SPREAD_RESP:
            bid, ask, closed = m.payload
            if closed:
                a["mkt_closed"] = True
            a["bid"], a["ask"] = bid, ask
        if not had and a["mkt_open"] is not None and a["mkt_close"] is not None:
            self.set_wakeup(a["id"], a["mkt_open"] + 0)
        # ScheduledAgent.receive_message
        if a["state"] == "AWAITING_SPREAD" and k == QUERY_SPREAD_RESP:
            if not a["mkt_closed"]:
                self.act(a, t)
            a["state"] = "AWAITING_WAKEUP"

    def place(self, a, t, qty, is_bid, price):
        o = Order(a["id"], self.next_order_id, is_bid, qty, price)
        self.next_order_id += 1
        a["orders"][o.order_id] = o.copy()
        self.send(a["id"], 0, Msg(self, LIMIT_ORDER, order=o))
        self.log(a, t, "ORDER_SUBMITTED", o)

    def act(self, a, t):
        bid, ask, rs = a["bid"], a["ask"], a["rs"]
        typ = a["type"]
        if typ == "NoiseTrader":
            size = int(max(1, round(rs.normal(a["size_mean"], a["size_std"]))))
            buy = bool(rs.randint(0, 2))
            off = int(rs.randint(0, a["offset"] + 1))
            if buy:
                anchor = int(ask) if ask else (int(bid) if bid else self.ref_price)
                self.place(a, t, size, True, anchor + off)
            else:
                anchor = int(bid) if bid else (int(ask) if ask else self.ref_price)
                self.place(a, t, size, False, anchor - off)
        elif typ == "MarketMaker":
            mid = int((bid + ask) // 2) if (bid and ask) else self.ref_price
            for o in list(a["orders"].values()):
                self.send(a["id"], 0, Msg(self, CANCEL_ORDER, order=o))
            half = a["spread"] // 2
            for lvl in range(a["depth"]):
                self.place(a, t, a["size"], True, mid - half - lvl)
                self.place(a, t, a["size"], False, mid + half + lvl)
        elif typ == "ValueTrader":
            if bid and ask:
                mid = (int(bid) + int(ask)) / 2.0
            elif bid:
                mid = float(bid)
            elif ask:
                mid = float(ask)
            else:
                return
            fundamental = int(self.oracle.observe(t, rs, sigma_n=1000.0))
            size = int(max(1, round(a["size_mean"])))
            if mid < fundamental - a["thr"] and ask:
                self.place(a, t, size, True, int(ask))
            elif mid > fundamental + a["thr"] and bid:
                self.place(a, t, size, False, int(bid))
        elif typ == "MomentumTrader":
            if bid and ask:
                mid = (int(bid) + int(ask)) / 2.0
            elif bid:
                mid = float(bid)
            elif ask:
                mid = float(ask)
            else:
                return
            h = a["hist"]
            h.append(mid)
            if len(h) > a["lookback"] + 1:
                h.pop(0)
            if len(h) <= a["lookback"]:
                return
            past = h[0]
            size = int(max(1, round(a["size_mean"])))
            if mid > past + a["thr"] and ask:
                self.place(a, t, size, True, int(ask))
            elif mid < past - a["thr"] and bid:
                self.place(a, t, size, False, int(bid))

    # ---------------- outputs (trace.extract_trace semantics) ----------------
    def trace_rows(self):
        ev = self.order_events
        # stable sort by (t, order_id) == ABIDES per-agent concat + stable EventTime sort, then
        # stable (t_ns, order_id) sort; generation order is nondecreasing in t per agent.
        idx = sorted(range(len(ev)), key=lambda i: (ev[i][0], ev[i][1]))
        last_exec = {}
        for pos, i in enumerate(idx):
            if ev[i][3] == "ORDER_EXECUTED":
                last_exec[ev[i][1]] = pos
        orders = []
        for pos, i in enumerate(idx):
            t, oid, ag, typ, isb, price, size = ev[i]
            if typ == "ORDER_EXECUTED":
                typ = "ORDER_FILLED" if last_exec[oid] == pos else "PARTIAL_FILL"
            orders.append((t, ag, typ, "BID" if isb else "ASK", price, size, oid))
        # quotes: keep last per (t, side), order by first appearance
        first, last = {}, {}
        for k, (t, isb, p, q) in enumerate(self.quote_events):
            key = (t, isb)
            if key not in first:
                first[key] = k
            last[key] = (p, q)
        quotes = [(t, 0, "QUOTE_UPDATE", "BID" if isb else "ASK", last[(t, isb)][0], last[(t, isb)][1], -1)
                  for (t, isb) in sorted(first, key=first.get)]
        rows = orders + quotes
        rows.sort(key=lambda r: (r[0], r[6]))   # stable
        return rows


def run_scenario(scenario: dict, seed: int | None = None):
    sim = Sim(scenario, seed)
    sim.run()
    return sim
