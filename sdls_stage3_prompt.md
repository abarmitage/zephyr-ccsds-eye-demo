# Final SDLS implementation prompt: symmetric automatic recovery and OTAR

Work in both:

- `/workspaces/akira-workspace/modules/lib/ccsds`
- `/workspaces/akira-workspace/ccsds-eye-demo`

Read and follow the repository `AGENTS.md` instructions and
`ccsds-eye-demo/sdls_plan.md` before editing. Continue from the working bounded
ARSN/adoption/FSR implementation. Do not commit, tag, or push unless explicitly
requested. This prompt supersedes any earlier plan text that assigns EYE-1 or
role A as a permanent recovery or OTAR coordinator.

## Goal

Recover automatically and symmetrically when either EYE starts late or reboots,
then reduce the replay opportunity created by fixed prototype keys and ARSN
adoption by rotating both directional keys through OTAR promptly.

Both EYEs must run the same recovery algorithm. Configuration may differ only
where identity and traffic direction require it; neither EYE has a permanent
coordinator role. Ordinary cold start and independent restart must not require
an operator to press **SYNC** within a short timing window. **SYNC** remains a
fallback explicit recovery action that may be pressed on either EYE.

Use one OTAR message and its matching FSR as the recipient-acceptance
indication. The existing operational SAs are already usable: switch them to the
fresh key material without stopping or restarting them. Do not add the full
network sequence of Key Verification, Key Activation, SA rekey, SA stop, and SA
start.

This is a focused autonomous EYE recovery policy using the existing EP OTAR and
FSR facilities. It is not a redesign of USLP, COP-1, CFDP, or the complete EP
key-management lifecycle.

Implement and verify recovery in this order:

1. transport return and symmetric COP-1 convergence;
2. symmetric OTAR-winner election over the fixed maintenance path;
3. one real OTAR transaction from the elected winner; and
4. FSR-correlated cutover and new-key confirmation.

Do not combine these phases into one callback or state transition.

## Preserve the working baseline

- Keep secured packet-bearing USLP Type-AD frames, clear Type-BC controls, and
  clear TFDF-absent CLCW/FSR feedback.
- Keep the existing USLP authentication mask, randomized upper 64-bit IV
  component with Weyl advancement, bounded ARSN window, deliberate receive
  adoption, strict FSR wire format, and bounded CLCW/FSR scheduling.
- Secure each new Type-AD frame once and retransmit the stored secured bytes
  exactly through COP-1. A retransmission must not consume another IV or ARSN.
- Continue classifying COP-1 duplicates before SDLS. Commit FARM and receive
  security state only after authentication, plaintext validation, and
  successful synchronous delivery.
- Preserve transactional rollback after malformed plaintext, rejected OTAR,
  or delivery backpressure so the exact frame can subsequently be retried.
- Preserve **SYNC** as an explicit local trust decision and fallback. Automatic
  restart recovery may arm the tightly scoped recovery adoption/election path
  only after peer return has been established; routine duplicate frames or an
  isolated authentication failure must not do so.
- Preserve clear USLP operation and both clear role builds when SDLS is
  disabled.

## Symmetric transport and COP-1 recovery

- Treat a rebooting peer and delayed Wi-Fi association as temporary absence.
  The absence may last longer than the current COP-1 retransmission budget.
- COP-1 retry exhaustion may fail the affected queued operation, but it must not
  leave the peer object permanently unable to receive feedback or recover when
  the remote EYE returns.
- Continue bounded ingress processing, peer ticking, CLCW feedback, and recovery
  observation while the UI reports `UNAVAILABLE` or `LINK ERROR`.
- Do not start a Type-BC retry timer merely because the local network interface
  is up. An inbound CLCW proves only peer-to-local delivery; it does not prove
  that a local control frame can reach the peer. Either require evidence of
  both directions before starting a bounded control procedure, or ensure its
  exhaustion remains automatically recoverable when the missing direction
  returns.
- On a clean CLCW, adopt the remote FARM's reported `V(R)` as the local next
  `N(S)`. If feedback reports WAIT, RETRANSMIT, or LOCKOUT, clear the remote
  condition with `UNLOCK`, then adopt `V(R)` from the resulting clean CLCW.
  Do not impose a guessed sequence with `SET V(R)` during ordinary recovery.
- Apply exactly the same convergence rules on both EYEs. Either side may be the
  first to observe peer return and begin recovery.
- Keep COP-1 convergence independent of SDLS election and OTAR. Do not submit an
  election packet or OTAR until the maintenance carrier can actually be sent.
- A one-way startup interval must remain recoverable: receiving no CLCWs is peer
  absence, while receiving a dirty CLCW is evidence that permits bounded
  control-procedure escalation.
- **SYNC** on either EYE must restart this same symmetric convergence procedure;
  it must not select a coordinator by role.

Verify this phase on hardware with fixed Stage 2 keys before implementing OTAR.

## Symmetric OTAR election and recovery policy

- A legitimate packet such as peer status may be the first authenticated frame
  accepted while `ADOPT` is armed. Keep peer-status and ordinary application
  traffic available; do not introduce a new application traffic gate.
- When recovery reaches the election phase, each EYE generates one fresh random
  96-bit election value and sends a small authenticated candidate announcement
  over its fixed maintenance SA. This announcement is not an OTAR and carries no
  session keys.
- Compare the two election values lexicographically; the lower value wins. Use
  spacecraft ID only to break an exact-value tie. Both EYEs must independently
  reach the same result regardless of message order.
- Candidate announcements are bounded, retryable, and idempotent. Loss,
  duplication, or reordering must not create a second candidate for the same
  attempt or select different winners.
- Bind each announcement to bounded local recovery-attempt state and sender
  identity. Retain one candidate until that attempt completes or is explicitly
  abandoned; a transport retry must resend the same candidate bytes.
- Only the elected winner may generate keys and submit a real OTAR. The losing
  EYE must never enqueue an OTAR that COP-1 could later deliver or retransmit.
- Do not use two already-submitted OTAR transactions as election ballots; COP-1
  cannot safely retract the losing secured frame.
- Allow only one recovery OTAR transaction in flight.
- Do not perform cryptographic work from an unsuitable receive callback or
  block link processing.
- Generate fresh, distinct keys for the two traffic directions using the
  production cryptographic random facility. Deterministic key injection is for
  tests only.
- Construct one recipient-compatible OTAR through reusable library support and
  process it through the existing authenticated EP recipient path. Do not
  duplicate OTAR cryptography in the demo.
- Install the fresh keys in predefined bounded destinations accepted by the
  existing OTAR rules, then atomically switch the existing operational TX/RX
  SAs to those keys as local recovery policy. Those SAs remain operational; do
  not add network stop, start, rekey, activation, or SPI-switch exchanges.
- Treat the FSR corresponding to the successfully processed OTAR carrier as the
  recipient-acceptance indication. Correlate its SPI and low ARSN octet with the
  one full carrier ARSN retained by the elected winner, and require no relevant
  FSR error indication.
- On the recipient, report successful OTAR processing only after both new keys
  have been installed atomically. The surrounding synchronous delivery must
  then be allowed to commit the carrier ARSN, FARM state, and FSR.
- On the elected winner, do not replace the corresponding local keys before the
  matching FSR is observed. After it is observed, install the matching local
  key material and allow normal secured traffic to continue.
- Regard the first successfully authenticated operational traffic under the
  new key as confirmation that cutover completed. This is diagnostic
  confirmation, not another network key-management phase.

## Carrier and retransmission safety

The OTAR transport must remain usable until the matching FSR is observed. If
the OTAR changes the key used to authenticate its own carrier too early, loss
of the FSR can make an exact old-key retransmission unverifiable and lock out
recovery.

Use a predefined fixed maintenance SA, distinct from the operational SAs and
never targeted by the OTAR. A reset peer can therefore recover after the other
peer has already moved away from the original operational keys. Keep this
bounded; do not introduce a general SA-lifecycle framework. The design must
ensure:

- the exact OTAR carrier can be retransmitted after a lost FSR;
- duplicate delivery cannot install keys twice or corrupt key metadata;
- ordinary OTAR success is reported in FSR only after atomic key installation;
- the maintenance/retry path is not destroyed by the key change it coordinates;
- fixed key and SA table capacities are respected across repeated recovery
  cycles.

The FSR is a compact acceptance signal rather than cryptographic proof. Its
ARSN field contains only the low octet and its OCF is clear. One elected winner,
one transaction in flight, exact SPI/ARSN correlation, bounded retry state, and
prompt new-key-authenticated traffic make its intended role explicit. Do not
describe the FSR as an authenticated acknowledgement.

## Reusable-library boundary

Inspect the existing OTAR initiator and recipient APIs before adding anything.
Add only the smallest reusable support genuinely missing for:

- generating or importing two fresh directional session keys;
- constructing the existing OTAR PDU under the configured master key;
- installing the fresh keys according to the existing OTAR destination-state
  rules, without weakening the general rejection of an Active destination;
- atomically switching the already-running operational SAs to the newly
  installed keys as a local recovery action;
- retaining the elected winner's matching local keys until the FSR commits the
  remote side; and
- rolling back PSA objects and metadata on every partial failure.

Keep EYE recovery timing and election policy in the demo. Keep OTAR codec,
cryptography, bounded key-table operations, atomic replacement, and workspace
wiping reusable. Do not expose or retain plaintext key material beyond the
bounded synchronous operation. Never log keys, OTAR plaintext, cryptographic
workspaces, or other secret material.

Use assertions for programmer buffer and configuration contracts with no
meaningful runtime recovery. Return errors for entropy, PSA, capacity,
protocol, delivery, timeout, and peer failures that can occur during normal
operation.

## Failure and retry behavior

- COP-1 retransmission repeats the stored OTAR frame byte-for-byte.
- A procedure retry must not generate different keys while the original OTAR
  could still be accepted. Retain one bounded pending transaction until it is
  acknowledged, abandoned by explicit recovery reset, or safely timed out.
- A non-matching FSR, an FSR for unrelated traffic, or an FSR with a relevant
  error must not trigger local cutover.
- OTAR rejection or local/remote key-install failure leaves the previously
  usable keys and SAs unchanged.
- Election and OTAR timeouts remain visible in LINK diagnostics and leave a
  usable recovery route. A bounded automatic retry or explicit **SYNC** may
  abandon the attempt and create fresh election state only after the old OTAR
  can no longer be accepted as current.
- Key buffers and temporary PSA objects are wiped or destroyed on success,
  failure, timeout, and cancellation.

## Security boundary and non-goals

This milestone deliberately shortens, but does not eliminate, the interval in
which a recorded frame valid under a fixed key could be accepted during
`ADOPT`. Candidate announcements authenticate under the fixed maintenance keys
but do not by themselves prove freshness. A previously recorded valid OTAR
could also be presented before the fresh transaction completes.

Do not claim that randomized IV upper bits or FSR establish freshness. Document
the residual opportunity accurately and keep a challenge-bound recovery
exchange as a possible later hardening step.

Also do not:

- treat the candidate exchange as a general discovery, membership, or key
  negotiation protocol;
- elect using real OTAR carriers or assign either EYE a permanent coordinator;
- add a general application traffic gate or block peer-status, CFDP, image, or
  command traffic solely for key rotation;
- add network Key Verification, Key Activation, Key Deactivation, SA rekey, SA
  stop, SA start, or operational-probe handshakes;
- persist keys, IVs, ARSNs, pending OTAR transactions, or protocol state;
- implement a general key-management daemon, certificate system, public-key
  exchange, or new cryptographic algorithm;
- secure clear USLP OCF feedback or Type-BC controls;
- add heap allocation, unbounded queues, broad new threads, or broad refactors;
  or
- redesign COP-1, CFDP, the UI, networking, or storage.

## Diagnostics

Add only concise LINK information needed to understand the automatic action:

- COP-1 recovery waiting for peer, converging, or failed;
- election waiting, candidate exchanged, won, lost, or timed out;
- OTAR pending, accepted by matching FSR, cut over, timed out, or failed;
- attempt and failure counts;
- carrier SPI and correlation state, without secret material; and
- whether subsequent operational authentication confirmed cutover.

Do not expose keys, plaintext OTAR contents, master-key data, PSA object values,
or cryptographic workspaces. Keep existing COP-1, adoption, SDLS, FSR, and CFDP
diagnostics intact.

## Required focused tests

Add focused coverage for at least:

1. A peer outage longer than the COP-1 retry budget, including an interval with
   no inbound CLCWs, remains automatically recoverable when feedback returns.
2. A dirty startup CLCW triggers bounded `UNLOCK` recovery, while a clean CLCW
   directly establishes the reported `V(R)` without `SET V(R)`.
3. Both EYEs run the same recovery transitions; either may return first, and
   **SYNC** on either side invokes the same fallback procedure.
4. Two candidate announcements delivered in either order select the lower
   96-bit value, with spacecraft ID resolving only an exact tie.
5. Lost, duplicate, delayed, and reordered announcements remain idempotent and
   do not create split-brain election results.
6. Exactly one real OTAR is submitted after election. The losing candidate
   never enters the COP-1 packet or Sent Queue.
7. Two distinct directional keys are generated and one recipient-compatible
   OTAR is constructed without retained or logged plaintext.
8. Recipient processing installs both predefined destination keys and
   atomically switches both operational SA key associations; malformed,
   unauthenticated, wrong-master, active-destination, capacity, PSA, and partial
   import failures switch neither.
9. Successful replacement commits the OTAR carrier and produces the expected
   FSR only after the complete recipient operation succeeds.
10. The elected winner cuts over only for the pending carrier's SPI and ARSN low
   octet with no relevant FSR error. Unrelated, stale, ambiguous, and error FSRs
   do not cut over.
11. Loss of the first FSR permits exact byte-for-byte OTAR retransmission and a
   later matching FSR without duplicate installation or lockout.
12. No new key pair is generated during retransmission or retry of the same
   pending transaction.
13. The first operational packet under the new key authenticates and marks the
   transition confirmed; a packet under the prior key then fails.
14. Peer-status and ordinary application traffic remain available during the
   transition, subject to their existing COP-1 and SDLS behavior.
15. Timeout, explicit cancellation/**SYNC**, entropy failure, and local or
    remote installation failure preserve a usable recovery path and release
    every pending PSA object and key buffer.
16. Repeated recovery cycles do not exhaust fixed SA slots, key slots, PSA
    objects, or bounded transaction state.
17. Existing clear USLP, COP-1, SDLS, EP/OTAR, FSR, CFDP, TM, TC, secured-peer,
    and EYE tests continue to pass.
18. Both EYE roles build in clear and secured configurations.

Prefer bounded native state-machine tests and reusable library cryptographic
tests. Native tests must not depend on Wi-Fi timing or hardware entropy.

## Hardware verification

Using the normal workflows:

```sh
./build_both.sh
./flash_both.sh <eye-1-device> <eye-2-device>
```

Verify:

- Start both EYEs together repeatedly in varied order. Both directions converge
  automatically and exactly one OTAR is accepted by a matching FSR.
- Bidirectional image SEND and REQUEST continue across the key transition.
- Reset EYE-1 independently and leave it offline longer than the current COP-1
  retry budget. Do not press **SYNC**. Verify automatic bidirectional recovery,
  election, OTAR acceptance, and new-key traffic. Repeat with EYE-2.
- Repeat with a deliberately one-way startup interval in each direction where
  practical. Inbound CLCWs alone must not be mistaken for bidirectional
  delivery, and neither EYE may become permanently unrecoverable.
- After automatic cases pass, force a stalled attempt and verify that **SYNC**
  on either EYE invokes the same symmetric fallback and recovers both directions.
- Lose or suppress an FSR where practical and confirm exact OTAR retransmission
  remains usable and does not install keys twice.
- Repeat recovery/key-rotation cycles and confirm no key, SA, or PSA-resource
  exhaustion.
- Confirm LINK diagnostics distinguish COP-1 loss, adoption, OTAR pending/FSR
  acceptance, new-key confirmation, SDLS errors, and CFDP behavior.

A deterministic native fault-injection test is sufficient for failure cases
that are impractical to create reliably on hardware.

## Documentation and completion

After implementation and hardware verification:

- Update `README.md` with the supported automatic post-recovery OTAR behavior
  and the remaining brief fixed-key adoption opportunity.
- Document reusable APIs and atomic key-replacement behavior in the CCSDS
  library where appropriate.
- Keep incomplete or speculative hardening, including a receiver challenge, in
  this plan rather than presenting it as supported user behavior.

Run the focused module and EYE tests, the relevant UDP integrations, and both
clear and secured role builds. Follow the repository debugging rule on every
failure: quote the first real error, classify it, and make one minimal fix at a
time.

Finish with `git diff --check` in both repositories and report:

- files and public APIs changed;
- the fixed maintenance carrier design and why it cannot self-lock;
- key-slot, SA, SPI, symmetric election, OTAR, and FSR-correlation decisions;
- atomic replacement, rollback, retry, cancellation, and workspace-wiping
  behavior;
- exact automated and hardware tests performed; and
- the residual pre-OTAR replay limitation.

Do not declare the milestone complete or remove `sdls_plan.md` until the
two-board recovery/key-rotation workflow has been exercised repeatedly on
hardware.
