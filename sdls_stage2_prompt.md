# Stage 2 implementation prompt: bounded anti-replay, restart adoption, and FSR

Work in both:

- `/workspaces/akira-workspace/modules/lib/ccsds`
- `/workspaces/akira-workspace/ccsds-eye-demo`

Read and follow the repository `AGENTS.md` instructions and
`ccsds-eye-demo/sdls_plan.md` before editing. Continue from the completed and
hardware-verified Stage 1 implementation. Do not commit, tag, push, or begin
Stage 3 unless explicitly requested.

## Goal

Harden the working symmetric USLP/SDLS/COP-1 EYE link with bounded in-session
ARSN anti-replay, deliberate boot/SYNC receive-session adoption, and correct
Frame Security Report handling. Preserve the verified Stage 1 wire composition,
exact COP-1 retransmission, clear feedback/control behavior, image workflows,
20 ms pacing, and unsecured build option.

Stage 2 must remain a working two-board checkpoint. It must not depend on OTAR,
key rotation, persistent counters, or any unfinished Stage 3 feature.

## Non-goals

- Do not implement OTAR, key activation, key rotation, or a coordinator.
- Do not replace the fixed Stage 1 prototype keys.
- Do not persist IV, ARSN, or receive-session state across power cycles.
- Do not secure TFDF-absent feedback frames or COP-1 Type-BC controls.
- Do not redesign COP-1, CFDP, the EYE UI, or the existing SDLS APIs broadly.
- Do not add heap allocation, threads, sockets, or EYE-specific policy to the
  reusable CCSDS library.
- Do not claim cryptographic freshness across reboot while a fixed key is
  reused. Stage 3 fresh-key establishment is required for that stronger claim.

## Preserve the Stage 1 baseline

- Protect only packet-bearing Type-AD frames.
- Keep the exact wire order:

  ```text
  clear USLP primary header
  clear 14-byte SDLS Security Header
  encrypted TFDF header and Space Packet
  16-byte authentication tag
  clear OCF
  ```

- Continue authenticating the USLP primary header with the Stage 1
  USLP-specific mask.
- Apply SDLS once when constructing a new frame, store the complete secured
  frame in the COP-1 Sent Queue, and retransmit it byte-for-byte.
- Classify COP-1 sequence state before SDLS. A COP-1 duplicate must not reach
  replay processing or decryption.
- Advance FARM only after authentication, plaintext validation, and successful
  packet delivery. Backpressure must leave FARM and receive-security state
  uncommitted so the exact retransmission can be accepted.
- Keep TFDF-absent CLCW/FSR feedback and Type-BC UNLOCK/SET V(R) frames clear.
- Preserve clear USLP operation whenever SDLS is disabled or not configured.

## Stage 2 security model

The transmitted 96-bit IV remains:

```text
randomized 64-bit Weyl component | 32-bit ARSN
```

Each boot initializes the upper component using `sys_csrand_get()`. Each newly
protected frame advances the Weyl component and ARSN; an exact COP-1
retransmission advances neither.

The random upper component makes reuse of the complete GCM IV under the fixed
prototype key unlikely when ARSN restarts at zero. It is probabilistic nonce
uniqueness, not a freshness proof. A 64-bit random value has a 2^-64 collision
probability against one specified prior boot value, with birthday risk growing
across many boots.

Stage 2 provides:

- bounded replay rejection within an established receive session;
- an authentication-gated way to adopt a new receive baseline after local boot
  or an explicit local SYNC action;
- no guarantee against replaying a previously recorded, valid fixed-key frame
  while adoption is armed.

Document that last limitation in design comments and Stage 2 handoff notes. Do
not imply that random IV generation authenticates a boot epoch. Stage 3 mitigates
the old-recording case by establishing fresh operational keys.

## Receive-session state and adoption

Add minimal bounded state for each operational USLP receive SA. Use clear names
and keep existing TM, TC, and Extended Procedure behavior compatible.

The receive state must distinguish at least:

- `UNESTABLISHED` or adoption armed;
- `ESTABLISHED` with the previous authenticated ARSN;
- the last accepted upper 64-bit IV component for diagnostics/session state if
  useful, without ever copying it into local transmit state.

### Automatic boot adoption

- An operational USLP RX SA starts with adoption armed after local boot.
- The first COP-1-expected packet-bearing Type-AD frame may be considered as an
  adoption candidate regardless of its ARSN value, including zero, an identical
  value, wraparound, or a value outside the configured bounded window.
- The candidate must use the configured SPI, operational RX role, active key,
  and security mode and must pass AEAD authentication before any receive-session
  state changes.
- Commit the candidate ARSN and any receive-session diagnostic IV state only
  after plaintext validation and successful delivery, at the same transaction
  boundary as FARM advancement.
- Authentication, format, capacity, or delivery failure must not establish or
  alter the session and must not disarm adoption.
- Once one candidate commits, close adoption and enforce the bounded window.

### Explicit local SYNC adoption

- A local operator SYNC action arms adoption for the operational USLP RX SA as
  part of restarting COP-1 synchronization.
- Arm adoption before the first expected secured data frame of the new COP-1
  session can arrive.
- The clear remote UNLOCK or SET V(R) control frame must not by itself arm SDLS
  adoption. The trust decision is the local boot state or explicit local SYNC
  action, followed by successful authentication of the candidate data frame.
- Routine timeout, retransmission, duplicate reception, CLCW processing, or a
  transient authentication failure must not silently arm adoption.
- Repeated local SYNC may re-arm adoption deliberately. Provide a small explicit
  reusable API for this action rather than having the EYE application mutate SA
  internals.
- If both peers reach terminal LINK error, operator SYNC on both sides must be
  able to establish new authenticated receive baselines and restore PEER OK.

## Established-session ARSN policy

Replace the Stage 1 EYE `UINT32_MAX` no-check setting with a bounded window. Pick
the smallest practical value supported by the current COP-1 window, retry, and
loss behavior, explain the choice, and configure both roles identically unless
directional behavior requires otherwise.

For an established session use modulo-2^32 arithmetic:

```text
advance = received_arsn - previous_arsn
accept when advance != 0 and advance <= receive_window
```

- Preserve natural 32-bit wraparound.
- Reject an identical ARSN and an excessive forward distance.
- Preserve `UINT32_MAX` as the explicit reusable no-check option for callers
  outside the Stage 2 EYE profile.
- Do not add an out-of-order replay bitmap; COP-1 ordering remains responsible
  for presenting only the expected Type-AD frame to SDLS.
- Perform all receive-state changes transactionally. A forged frame must not
  change the established ARSN, adoption state, FARM state, or delivered data.
- Keep exact retransmissions viable after delivery backpressure.

Do not require the receiver to predict the sender's random boot seed in order
to authenticate a candidate. GCM processes the complete IV carried in the
Security Header. If upper-component progression is checked as an additional
session invariant, first prove that it remains correct for every supported SA
and interleaving path; otherwise retain it only as diagnostic receive state.

## Frame Security Report

- Correct the Version-1 FSR encoder so the Type-2 Control Word Type bit is set.
  A valid encoded Version-1 FSR begins with `0xC0`, not `0x40`.
- Add strict FSR decoding with exact-length, version, type, reserved-bit, and
  field validation consistent with the existing coding style.
- Preserve the existing four-octet FSR profile and current failure flags unless
  a standards/API mismatch requires one minimal, documented correction.
- Keep FSR outside SDLS protection as an OCF, consistent with Stage 1.
- Add a deliberate CLCW/FSR selection policy. CLCW remains the primary COP-1
  feedback and must not be starved by security reporting.
- Never emit consecutive FSRs indefinitely. Bound FSR frequency, ensure a CLCW
  follows promptly, and prioritize CLCW while COP-1 acknowledgement, WAIT,
  LOCKOUT, retransmission, or synchronization feedback is urgent.
- A routine COP-1 duplicate must not create an SDLS replay alarm or unnecessary
  FSR because it is classified before SDLS.
- Authentication, sequence, SA, and key failures may request an FSR, but the
  EYE main workflow must remain recoverable through the existing LINK controls.

## EYE integration and diagnostics

- Configure a bounded operational ARSN receive window in secured mode.
- Arm receive adoption during SDLS initialization and when the local SYNC action
  begins.
- Do not arm adoption merely because a peer-status packet is late or absent.
- Extend the LINK screen minimally to distinguish:
  - receive session unestablished/adoption armed versus established;
  - bounded replay/sequence failures;
  - authentication/SA failures;
  - FSR sent/received counts;
  - existing COP-1 timeout, retransmission, duplicate, and CFDP counters.
- Keep normal image SEND and REQUEST workflows unchanged.
- Keep prototype-key and recovery-window limitations off the ordinary user
  workflow but visible in configuration help/logging and development notes.
- Do not add OTAR or key-management controls to the UI.

## Required focused tests

Add focused coverage for at least:

1. Operational USLP RX starts with adoption armed.
2. The first authenticated candidate adopts ARSN zero and closes adoption.
3. Boot adoption also accepts an authenticated nonzero, wrapped, identical, or
   otherwise out-of-window ARSN as the new baseline.
4. A forged or corrupted adoption candidate changes no receive-session state,
   remains undelivered, leaves FARM unchanged, and leaves adoption armed.
5. Plaintext validation and delivery backpressure likewise do not commit
   adoption; the exact retransmission can subsequently succeed.
6. Once established, bounded ARSN checking accepts normal advance and 32-bit
   wraparound while rejecting identical and excessive-forward-distance values.
7. Explicit local SYNC re-arms adoption; routine timeout, retransmission,
   duplicate, CLCW, UNLOCK, and SET V(R) processing do not.
8. COP-1 duplicates are suppressed before SDLS and do not generate replay FSRs.
9. Exact secured retransmission still reuses every stored byte and consumes no
   new IV or ARSN.
10. The FSR encoder produces the correct `0xC0` Version-1 prefix and exact wire
    bytes for representative flag combinations.
11. FSR decoding accepts valid vectors and rejects wrong version/type, reserved
    bits, and incorrect length.
12. CLCW/FSR scheduling never emits an unbounded FSR run and does not prevent
    COP-1 acknowledgement or retransmission recovery.
13. Existing clear USLP, COP-1, SDLS, CFDP, TM, TC, EP, and Stage 1 secured-peer
    tests continue to pass.
14. Both EYE roles build in clear and secured Stage 2 configurations.

Where practical, include a test demonstrating and naming the fixed-key adoption
limitation: a previously captured valid frame can authenticate while adoption
is armed. This is expected Stage 2 behavior, not a reason to weaken established-
session replay checks or to implement Stage 3 prematurely.

## Hardware verification

Using the normal workflows:

```sh
./build_both.sh
./flash_both.sh <eye-1-device> <eye-2-device>
```

Verify:

- PEER OK and secured image transfer in both directions.
- SDLS receive state becomes established after initial authenticated traffic.
- Reset EYE-1 independently, use local SYNC as required, recover PEER OK, and
  transfer successfully without resetting EYE-2.
- Reset EYE-2 independently and repeat without resetting EYE-1.
- Let both peers reach LINK error, apply operator SYNC on both sides, and verify
  recovery.
- Observe timeout/retransmission/duplicate recovery without an authentication
  or replay failure for legitimate COP-1 duplicates.
- Confirm bounded replay/authentication failures are distinguishable from
  normal COP-1 counters and FSR does not destabilize transfer.
- Repeat restart and transfer checks enough times to exercise different random
  boot IV components.

A special deterministic fault-injection firmware build is not required if the
hardware run naturally observes timeout/retransmission/duplicate recovery and
the exact secured retransmission property remains covered by automated tests.

## Completion and reporting

Run the focused SDLS, USLP, frame-integration, USLP/CFDP, TM, TC, EP, and EYE
tests plus both clear and secured role builds. Follow the repository debugging
rule on each failure: quote the first real error, classify it, and make one
minimal fix at a time.

Finish with `git diff --check` in both repositories and report:

- files changed;
- receive-session/adoption API and state-transition decisions;
- selected bounded window and its justification;
- FSR wire and scheduling decisions;
- exact tests and results;
- hardware work performed and still required;
- the fixed-key adoption replay limitation;
- any remaining Stage 2 blocker.

Do not update user-facing README behavior until it is implemented and verified
on hardware. Keep incomplete status and security limitations in
`sdls_plan.md`. When Stage 2 is genuinely complete, update only the session
hand-off checklist at the end of `sdls_plan.md` with completed work, tests,
hardware state, and the next item.
