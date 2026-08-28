# Stage 1 implementation prompt: working USLP/SDLS EYE prototype

Work in both:

```text
/workspaces/akira-workspace/modules/lib/ccsds
/workspaces/akira-workspace/ccsds-eye-demo
```

Use the current working branches; do not pin either repository to a historical
tag. Before editing, read:

```text
/workspaces/akira-workspace/ccsds-eye-demo/sdls_plan.md
```

Implement only Stage 1 of that plan.

## Purpose and security boundary

Produce a working prototype of SDLS authenticated encryption over the existing
symmetric USLP/COP-1 EYE link. Prove the wire composition, cryptography,
directional SPIs/SAs, exact COP-1 retransmission, EYE builds, minimal UI, and
two-board image transfer.

Stage 1 intentionally has no meaningful anti-replay protection. It also omits
authenticated restart synchronization, FSR feedback, EP commands, OTAR, and
key rotation. Do not present it as an operationally secure profile. Those are
later stages.

Stage 1 must nevertheless finish as a separately executable and reasonably
robust two-board demo: both roles build, encrypted image transfer works in both
directions, COP-1 loss recovery works, and either board can be restarted and
resynchronized using the existing link controls. The weakness is the absence
of authenticated anti-replay and restart policy, not an unfinished data path.

This is an additive SDLS integration. Preserve the existing TM transmit, TC
receive, and Extended Procedure APIs, roles, behaviour, and tests. Preserve the
existing clear USLP/COP-1 behaviour whenever SDLS is disabled or not configured.

Keep the reusable implementation allocation-free, use caller-owned bounded
storage, avoid broad refactoring, and follow the existing API and test style.

## Library requirements

### USLP frame security

- Add operational USLP TX and RX SA roles alongside the existing roles. Do not
  rename, remove, or reinterpret existing SDLS roles.
- Protect the Transfer Frame Data Field of packet-bearing Type-AD frames. Do
  not add Type-BD support in this stage.
- Compose a secured packet frame as:

  ```text
  USLP primary header
  SDLS Security Header
  encrypted TFDF header and Space Packet
  SDLS authentication tag
  OCF
  ```

- Leave the USLP primary header clear and authenticate it using a new
  USLP-specific authentication mask derived from CCSDS 355.0-B-2 section
  4.2.2.6.2. Do not reuse TM or TC mask arrays. Authenticate VCID and MAP ID;
  mask COP-managed or later-finalized fields, including frame sequence number.
- Leave the OCF outside SDLS protection.
- Keep TFDF-absent OCF-only feedback frames clear and CLCW-only.
- Keep COP-1 Type-BC UNLOCK and SET V(R) frames clear.
- Extend the codec with a clear-envelope/opaque-protected-data path rather than
  duplicating primary-header parsing inside the peer. Preserve strict clear
  Space Packet decoding.

### COP-1 ordering

- Apply SDLS exactly once when constructing a new packet-bearing Type-AD frame.
- Store the complete secured frame in the COP-1 Sent Queue and retransmit those
  bytes unchanged. Retransmission must not consume another IV/ARSN.
- On receive, validate and decode the clear envelope, obtain the OCF, and
  classify the COP-1 frame sequence before processing SDLS.
- Suppress a negative-window COP-1 duplicate without passing it to SDLS.
- For an expected frame, authenticate/decrypt the TFDF, validate the plaintext
  TFDF and complete Space Packet, deliver it, and only then advance FARM.
- Authentication, replay, SA, key, malformed-plaintext, or delivery failure
  must not advance FARM or expose packet data.
- Preserve current CLCW feedback and backpressure behaviour.

### IV generation and Stage 1 ARSN policy

Keep the 96-bit IV layout:

```text
64-bit Weyl component | 32-bit ARSN
```

- Initialize the upper 64-bit component from an internal `sys_csrand_get()`
  boot seed before the first protected transmission, then retain the existing
  Weyl progression for each new protected transmission. Do not require the
  application to provide a production seed.
- Propagate entropy failure; never silently use a fixed fallback. Permit
  deterministic seed injection only in tests.
- Ensure every path which creates a new GCM/GMAC IV uses initialized transmit
  state. Start the transmit ARSN at zero.
- The receiver authenticates/decrypts with the complete IV carried in the
  Security Header. Do not derive or track the initial seed of the peer's Weyl
  sequence in this stage.
- Modify the reusable SDLS implementation as necessary to replace the current
  numeric ordering test with modulo-2^32 window arithmetic for bounded
  windows:

  ```c
  uint32_t advance = received_arsn - previous_arsn;

  if (advance == 0u || advance > rx_window) {
      /* replay/window rejection */
  }
  ```

- Preserve the uninitialized receive-SA behaviour: its first successfully
  authenticated frame establishes the previous ARSN.
- Support the full `uint32_t` window range and define `UINT32_MAX` as an
  explicit no-check setting. With that setting, bypass ARSN ordering/replay
  rejection entirely and accept every successfully authenticated ARSN,
  including an identical value, modulo wraparound, or a restarted peer
  returning to zero.
- Configure the EYE receive SAs with `UINT32_MAX`. Legitimate COP-1 duplicate
  frames should still be classified before SDLS rather than decrypted again.
- Do not implement authenticated restart arming, boot-seed synchronization, or
  recovery counters in Stage 1.

### API and capacity constraints

- Add optional SDLS configuration to `ccsds_uslp_peer`, with caller-owned
  context/workspace storage and bounded directional SPI selection.
- Account for the 14-byte Security Header and 16-byte tag in all capacity and
  maximum-payload calculations.
- Do not add heap allocation, sockets, CFDP ownership, threads, work queues, or
  EYE-specific policy to the reusable peer.
- Do not modify or enable FSR handling in this stage. In particular, do not add
  CLCW/FSR alternation merely to complete this prototype.

## EYE requirements

- Add a simple Kconfig choice between secured and existing clear USLP/COP-1.
- Enable the required SDLS, PSA Crypto, and entropy options when secured mode
  is selected.
- Provision fixed demo keys and reciprocal directional TX/RX SPIs/SAs for
  EYE-1 and EYE-2. Clearly identify these as prototype keys.
- Configure each RX SA with `UINT32_MAX` as described above.
- Reduce CFDP segment size if required so one secured Space Packet fits one
  USLP frame and UDP datagram.
- Add a compact SDLS area to the existing LINK screen showing only useful local
  state and failure counts. Do not add SDLS recovery, EP, or key controls.
- Do not rewrite unrelated README sections. Document only configuration or
  user-facing behaviour that is actually completed and verified.

## Verification

Add focused coverage for at least:

1. Exact USLP authentication-mask bits.
2. Internal secure IV seeding, deterministic test seeding, and entropy failure.
3. Modular ARSN normal advance and 32-bit wraparound with a bounded window.
4. Identical ARSN and excessive forward-distance rejection with a bounded
   window.
5. `UINT32_MAX` bypassing ARSN checks, including identical ARSN, wraparound,
   and zero after a nonzero previous value.
6. Secured Space Packet round-trip between two USLP peers.
7. Exact byte-for-byte retransmission without IV/ARSN advancement.
8. COP-1 duplicate suppression before SDLS replay processing.
9. Authentication failure without packet delivery or FARM advancement.
10. Existing clear USLP, COP-1, SDLS, CFDP, TM, TC, and EP tests.
11. Both EYE roles building with SDLS disabled and enabled.

Then verify on both boards:

- authenticated/encrypted image transfer in both directions;
- COP-1 recovery after the existing injected data-frame loss;
- restart and resynchronize either board independently, then resume transfer;
- useful minimal SDLS status on the LINK screen;
- reliable operation at the current 20 ms route interval, changing pacing only
  if measurement demonstrates that it is necessary.

Independent peer restart is a Stage 1 robustness criterion, but authenticated
authorization of that restart is not. Use the existing COP-1 link
resynchronization and the deliberately disabled ARSN check; do not expand Stage
1 into the authenticated restart protocol planned for Stage 2.

Run the focused SDLS, USLP, frame-integration, and USLP/CFDP native tests plus
both EYE builds. If a test or build fails, quote the first real error, classify
it, and make one minimal fix at a time. Finish with `git diff --check` and
report:

- files changed;
- API and wire-format decisions;
- exact tests and results;
- hardware work still required;
- any remaining Stage 1 blocker;
- the next safe step.

Do not commit, tag, push, begin Stage 2, implement FSR, or implement EP/OTAR
unless explicitly requested.

When Stage 1 is genuinely complete, update only the session hand-off checklist
at the end of `ccsds-eye-demo/sdls_plan.md` with completed work, tests, hardware
state, and the next item.
