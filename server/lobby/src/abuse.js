// In-memory abuse limits of the lobby. Nothing here is written to SQL: a limit that cost a write per hit would defeat
// its purpose.
export const PROTECTION = {
  sessionWindowMs: 10 * 60 * 1000,
  seenRefreshMs: 15 * 60 * 1000,
  rejectWindowMs: 10 * 60 * 1000,
  hello: { burst: 12, perSecond: 1 / 15 },
  roomCreateAccount: { burst: 6, perSecond: 1 / 20 },
  roomCreateAddress: { burst: 20, perSecond: 1 / 6 },
  roomJoinAccount: { burst: 20, perSecond: 1 / 5 },
  publicEndpoint: { burst: 8, perSecond: 1 / 30 },
  partyReportAll: { burst: 30, perSecond: 1 / 30 },
  partyReportAccount: { burst: 3, perSecond: 1 / 120 },
  partyReportAddress: { burst: 12, perSecond: 1 / 30 },
  incidentAll: { burst: 40, perSecond: 1 / 3 },
  incidentAccount: { burst: 8, perSecond: 1 / 60 },
  incidentAddress: { burst: 16, perSecond: 1 / 30 },
  incidentLookupAll: { burst: 200, perSecond: 2 },
  incidentStatusAddress: { burst: 20, perSecond: 1 / 15 },
  incidentReadAddress: { burst: 10, perSecond: 1 / 30 },
  lobbyPublishAccount: { burst: 6, perSecond: 1 / 10 },
  lobbyListAccount: { burst: 6, perSecond: 1 / 5 },
  lobbyListAddress: { burst: 60, perSecond: 3 },
  lobbyRequestAccount: { burst: 10, perSecond: 1 / 15 },
  trackedKeys: 4096,
};

export class RateLimiter {
  constructor(spec, maxKeys = PROTECTION.trackedKeys) {
    this.spec = spec;
    this.maxKeys = maxKeys;
    this.buckets = new Map();
  }

  tokens(key, now) {
    const [tokens, last] = this.buckets.get(key) ?? [this.spec.burst, now];
    return Math.min(this.spec.burst, tokens + (Math.max(0, now - last) / 1000) * this.spec.perSecond);
  }

  allows(key, now) {
    return this.tokens(key, now) >= 1;
  }

  take(key, now) {
    const tokens = this.tokens(key, now);
    if (tokens < 1) return false;
    this.buckets.delete(key);
    if (this.buckets.size >= this.maxKeys) this.prune(now);
    this.buckets.set(key, [tokens - 1, now]);
    return true;
  }

  // Idle keys are as good as new ones; if every key is busy the oldest go.
  prune(now) {
    for (const key of [...this.buckets.keys()]) {
      if (this.tokens(key, now) >= this.spec.burst) this.buckets.delete(key);
    }
    for (const key of this.buckets.keys()) {
      if (this.buckets.size < this.maxKeys) break;
      this.buckets.delete(key);
    }
  }
}
