# Redis Notification-Loss Recovery

Normal updates retain active/latest-pending session snapshots and reject in-use
`(observation_domain_id, template_id)` collisions without changing installed
owners. Unknown data is dropped under the best-effort exporter contract.

## Recovery Boundary

`SubscriberStateTable::pops()` can consume notifications and then fail on a
non-hash row. Its exception discards the successful batch prefix, including
Delete/recreate or pending-cancellation boundaries. The final Redis rows alone
cannot identify which colliding registrations were previously accepted.

On a recognized HGETALL WRONGTYPE, SWSS drains the cached remainder, collects the
current rows plus previously observed removals, and sends **one reconciliation
envelope**. It does not delete/reinstall owners as separate messages. Collection
errors other than the recognized row error remain failures; a partial scan is
not published as an authoritative envelope.

The IPFIX actor compiles the candidates without modifying its current registry,
then resolves ownership using only that registry and this envelope:

| Situation | Result |
|---|---|
| Missing, disabled, deleted, or invalid owner | Remove its previous session. |
| Current owner still claims its installed ID in a valid candidate | Reserve that ID for this incumbent, even if its own metadata changed. Reject other owners' conflicting snapshots. |
| Candidate exactly matches an existing pending snapshot | Preserve the complete active/pending session and reserve all its installed IDs. Recovery is not a data-triggered cutover. |
| Released/unowned ID has one claimant | Admit that claimant, subject to whole-snapshot rejection below. |
| Released/unowned ID has multiple claimants | Reject all claimants' complete snapshots. Never choose by iteration order. |
| Valid candidate is rejected and has a previous session | Preserve its entire previous active/pending state, including omitted keys. |
| Valid candidate is rejected and has no previous session | Install nothing. |
| Changed candidate is accepted | Install the complete authoritative snapshot as active, clearing obsolete pending state. |

Whole-snapshot rejection can preserve additional old keys. A monotone worklist
propagates their conflicts to other candidates. Rejected candidates are never
reaccepted during the same batch: removing one losing claim must not manufacture
an arbitrary winner elsewhere. Every owner is rejected at most once. No permanent
ID history, rejected-row cache, retry queue, self-exec, or replay is involved.

Finally, the actor builds and checks the resulting session/installed maps and
replaces both synchronously, with no `await`. Data sees the old or new registry,
not a reset gap or partially installed snapshot.

## Coverage and Limits

Tests include incumbent versus rejected row, unchanged pending plus a rejected
claimant, lost same-owner Delete/recreate, accepted pending cancellation and
supersession, absent/invalid owners, unowned ambiguity, multi-key snapshot
rejection, transitive retained-key conflicts, simultaneous ID swaps, separate
domains, owner-order permutations, repeated recovery, subsequent normal
cancellation/cutover, and malformed envelopes. Real Redis/IPFIX tests verify
decoded object names, not only event membership.

This is atomic only inside the IPFIX actor. Redis rows are read separately;
concurrent writes rely on subsequent notifications. Same-owner historical data
cannot be distinguished after an authoritative metadata replacement without a
wire generation identifier; short transition loss remains accepted. Runtime
WRONGTYPE recognition uses the precise exception format exposed by current
swss-common bindings; startup-constructor failure is outside this recovery path.
