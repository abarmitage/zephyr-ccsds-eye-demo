# SDLS integration plan

## Goal

Add optional SDLS authenticated encryption to the symmetric USLP/COP-1 link
between EYE-1 and EYE-2. COP-1 must continue to recover missing frames, while
SDLS authenticates and encrypts each newly transmitted packet-bearing frame.

The implementation spans two repositories:

- `modules/lib/ccsds`: reusable USLP/SDLS composition and tests;
- `ccsds-eye-demo`: configuration, UI, and hardware verification.

Development continues from the current branches of both repositories; it is
not pinned to `demo-uslp-cop1`. When `CONFIG_CCSDS_SDLS` is disabled, the
current unsecured USLP/COP-1 mode must continue to build and work.

Stage 1 is deliberately a bring-up prototype. It proves the secured USLP wire
format, COP-1 composition, directional SPIs and keys, and EYE integration
before adding operational anti-replay, restart recovery, FSR reporting, or
OTAR. It is not the final security profile.

Stages 1 and 2 record the incremental checkpoints used to reach the current
working link. Remaining work is prioritized by the security property it closes;
diagnostics and documentation are finalization activities rather than separate
protocol stages. Each security checkpoint must still build, run, transfer
images in both directions, and recover reliably on the two-board demo.

## Agreed frame processing

- A frame containing a Transfer Frame Data Field is data-bearing and is
  protected by SDLS. The current EYE peer implements packet-bearing Type-AD
  data; the same rule will apply to any future Type-BD data.
- A zero-data feedback frame has no Transfer Frame Data Field and carries only
  an OCF containing a CLCW or, in a later stage, FSR. This EYE profile does not
  add an SDLS Security Header or Trailer to that frame. The OCF is outside SDLS
  protection in any case.
- COP-1 Type-BC link-control frames remain clear, as specified by USLP.
- Encrypt and authenticate the USLP Transfer Frame Data Field, including its
  protocol identifier and Space Packet.
- Keep the USLP primary header and OCF available to COP-1. Add a USLP-specific
  authentication mask derived from the CCSDS 355.0-B-2 defaults; do not reuse
  the existing TM or TC masks. Authenticate VCID and MAP ID while masking
  COP-managed or later-finalized fields such as the frame sequence number.
- Secure a new frame once, then store the complete secured frame in the COP-1
  Sent Queue. Every retransmission repeats those bytes unchanged, including
  the IV, ARSN, ciphertext, and authentication tag.
- On receive, FARM classifies the frame sequence before SDLS. A COP-1 duplicate
  is not passed to SDLS. An expected frame is authenticated and decrypted;
  only then may it be delivered and FARM advanced.
- Authentication failure must not deliver data or advance FARM.

## IV and ARSN model

Use the existing 96-bit IV division:

```text
64-bit Weyl component | 32-bit ARSN
```

- Initialize the upper 64-bit component from a boot seed generated internally
  with Zephyr's `sys_csrand_get()`, then retain the existing Weyl progression
  for each new protected transmission. Applications do not supply a production
  seed; deterministic injection remains test-only.
- Start the transmit ARSN at zero. The complete received IV is used by GCM to
  authenticate/decrypt the frame; the receiver does not have to predict the
  transmitter's boot seed.
- Replace the existing SDLS ARSN comparison with modulo-2^32 subtraction for
  bounded receive windows:

  ```text
  advance = received_arsn - previous_arsn
  accept when advance != 0 and advance <= receive_window
  ```

  This supports natural 32-bit wraparound. The present numeric
  `received_arsn <= previous_arsn` test must not be retained.
- Define `UINT32_MAX` as the explicit no-check receive-window setting. In this
  mode accept every authenticated ARSN, including an identical value,
  wraparound, or a peer restart at zero. COP-1 classifies legitimate duplicate
  frames before SDLS. Stage 1 uses this setting intentionally and therefore
  provides authentication and encryption but no anti-replay protection.
- Stage 1 does not derive, track, or synchronize the initial seed of a peer's
  Weyl sequence and does not provide an authenticated restart protocol. IV
  uniqueness under a reused fixed key is probabilistic after reboot, so Stage
  1 is not an operationally secure mode.

## Stage 1: working USLP/SDLS EYE prototype

### Reusable library work

- Extend the existing SA-role model with operational USLP transmit and receive
  roles. This is additive: retain the existing TM transmit, TC receive, and
  Extended Procedure roles and integrations unchanged.
- Extend the USLP codec with a clear-envelope/opaque-secured-data path while
  preserving strict clear Space Packet decoding.
- Add and test the USLP-specific authentication mask.
- Add optional SDLS configuration to `ccsds_uslp_peer`, including caller-owned
  context and bounded workspace and directional SPI selection.
- Account for the 14-octet SDLS Security Header and 16-octet tag in all frame
  and packet capacity calculations.
- Apply SDLS before placing a new frame in the Sent Queue and retransmit the
  stored bytes without another SDLS call.
- Decode the clear envelope and classify the COP-1 sequence before security
  processing. Pass only expected new packet-bearing frames to SDLS; advance
  FARM only after successful authentication, plaintext validation, and
  delivery.
- Generate the transmit boot seed internally and propagate entropy failure.
- Replace the current non-wrapping ARSN comparison with the modular window
  check above. Configure Stage 1 EYE receive SAs with `UINT32_MAX`.
- Preserve the clear path whenever SDLS is disabled or not configured.

### EYE integration

- Add a simple Kconfig switch for secured versus existing clear USLP/COP-1.
- Enable the required SDLS, PSA Crypto, and entropy configuration.
- Provision fixed demo keys and reciprocal directional SPIs/SAs. Do not add
  EP commands, OTAR, key rotation, FSR, or restart synchronization yet.
- Reduce the maximum CFDP segment size if necessary so one protected Space
  Packet still fits one USLP frame and UDP datagram.
- Add only minimal SDLS state and failure counters to the existing LINK screen;
  no SDLS control actions are required in this stage.
- Keep zero-data feedback CLCW-only. Do not alter or enable the existing FSR
  path in Stage 1.

### Stage 1 verification

- Existing clear USLP, COP-1, SDLS, CFDP, TM, TC, and EP tests still pass.
- A secured Space Packet round-trips between two USLP peers.
- A lost frame is retransmitted byte-for-byte and accepted once.
- A COP-1 duplicate does not produce an SDLS replay error.
- A forged or corrupted expected frame does not deliver data or advance FARM.
- Modular ARSN tests cover normal advance, wraparound, identical-value
  rejection in a bounded window, and complete bypass with the `UINT32_MAX`
  Stage 1 setting.
- Both EYE roles build in clear and secured configurations.
- On hardware, image transfer succeeds in both directions and the existing
  injected frame loss is recovered by COP-1.
- Reset either board independently and confirm that the existing COP-1
  resynchronization restores the secured link and image transfer. Stage 1 does
  not authenticate or constrain this resynchronization; Stage 2 adds that
  security property.

## Stage 2: anti-replay, restart recovery, and FSR

- Select a bounded operational ARSN receive window.
- Derive and track the initial seed of the peer's upper-64-bit Weyl sequence as
  receive-session state without ever adopting it as the local transmit seed.
- Define an authenticated transition that permits a reset peer with a new boot
  seed and ARSN zero to establish new receive state. Failed authentication must
  leave the established state unchanged.
- Arm that transition only after genuine established-peer loss or explicit
  user action, not routine COP-1 retransmission.
- Correct the existing FSR encoder to set the Type-2 Control Word Type bit: a
  valid Version-1 FSR begins with `0xC0`, not `0x40`.
- Add FSR decoding and a deliberate CLCW/FSR OCF scheduling policy. CLCW must
  remain frequent enough for reliable COP-1 operation.
- Report authentication, sequence, and SA failures without turning routine
  COP-1 recovery into a main-screen error.

### Stage 2 completion checkpoint

- Bounded anti-replay accepts normal advance and wraparound while rejecting
  stale, repeated, and excessive-forward-distance ARSN values.
- Cold start and independent reset of either board recover through an
  authenticated transition; a forged transition changes no receive state.
- FSR and CLCW feedback coexist without destabilizing COP-1.
- Image transfer and injected-loss recovery continue to pass in both
  directions after repeated restart tests.

## Final security milestone: automatic post-recovery OTAR

The remaining security issue is the interval in which fixed prototype keys are
still in use after adoption. While adoption is armed, a recorded frame that is
valid under a fixed key can authenticate without matching an established ARSN
baseline. The randomized upper 64 bits of the IV make accidental IV reuse very
unlikely, but do not provide freshness for a recorded frame carrying its
original IV and tag.

Close this opportunity promptly, without introducing the complete ground-led
EP key and SA lifecycle into the autonomous EYE link:

- After cold-start recovery or explicit **SYNC** causes an adopted frame to be
  accepted, automatically attempt OTAR as soon as the link can carry it.
- Use EYE-1 as the deterministic recovery coordinator. Only EYE-1 initiates the
  automatic recovery OTAR, so simultaneous transactions cannot occur.
- Generate fresh keys for both traffic directions and send them in one OTAR
  transaction using the existing EP cryptography and recipient processing.
- Replace the key material used by the existing, already-operational SAs. Do
  not add network Key Verification, Key Activation, SA rekey, SA stop, or SA
  start exchanges merely to reproduce the full operational lifecycle.
- Treat the matching FSR (carrier SPI, low ARSN octet, and no relevant error)
  as the recipient's OTAR-acceptance indication. Permit only one coordinator
  transaction at a time so this deliberately compact correlation is
  unambiguous.
- Carry OTAR on a distinct fixed maintenance SA that is never a destination of
  the OTAR. It must remain usable for exact retransmission until the matching
  FSR is received and across a later independent reset.
- Keep peer-status and normal application traffic available during the short
  transition. This milestone reduces the replay interval through prompt key
  rotation; it does not introduce a general recovery traffic gate.
- Use the first successfully authenticated operational traffic under the new
  key as confirmation of cutover. A failed or timed-out attempt remains visible
  and retryable through **SYNC** or the still-usable OTAR carrier.

This is an autonomous recovery policy layered on the existing EP OTAR and FSR
facilities. It should not be described as implementing the complete EP key and
SA lifecycle.

### Acceptance checkpoint

- An adopted peer-status or other legitimate packet restores communication and
  promptly initiates one OTAR transaction.
- One accepted OTAR installs both directional keys and atomically switches the
  existing operational SAs to them without stopping or restarting those SAs.
- Recipient key replacement is atomic: processing failure changes neither key.
- The recipient reports acceptance through a matching FSR, and the winning
  originator does not cut over its corresponding local keys before observing
  that FSR.
- Loss of the FSR or retransmission of the exact OTAR cannot lock out the
  key-maintenance path or install the keys twice.
- The first authenticated operational exchange under the new keys confirms
  cutover, after which image transfer continues in both directions.
- Cold start and independent reset of either board recover, rotate keys, and
  resume bidirectional image transfer without resetting both boards.
- The implementation and documentation explicitly retain the short residual
  opportunity for a previously recorded valid frame or OTAR to arrive before
  the fresh OTAR completes. A nonce/challenge-bound recovery transaction is a
  possible later hardening step, not part of this milestone.

## Final verification and documentation

Diagnostics and documentation are completion work rather than another protocol
stage:

- Verify that the existing LINK screen continues to distinguish COP-1, SDLS,
  OTAR/key-transition, and CFDP behavior during repeated hardware recovery
  runs.
- Keep `README.md` focused on supported behavior and operator workflows after
  hardware verification; keep incomplete design work in this plan.
- Record reusable recovery/OTAR details in `zephyr-ccsds` design/reference
  documentation.
- Remove this plan when the final completion criteria are met and supported
  behavior is documented.

## Final completion criteria

- SDLS is optional and the unsecured baseline remains supported.
- Packet-bearing USLP frames are encrypted and authenticated in both
  directions.
- COP-1 retransmits the original secured frame successfully.
- Operational anti-replay supports ARSN wraparound and rejects stale traffic.
- Independent peer reset recovers through an authenticated transition to a new
  boot seed and ARSN zero.
- Prompt automatic OTAR replaces both directional fixed keys after recovery,
  with the matching FSR used as the recipient-acceptance indication.
- OTAR retransmission and a lost FSR do not lock out the maintenance path.
- Existing operational SAs continue without a network activation, rekey,
  stop, or start sequence.
- Image send and request workflows pass repeatedly in both directions.
- LINK diagnostics distinguish COP-1, SDLS, and CFDP behaviour.

## Current status

- Bounded ARSN sessions, authenticated adoption, FSR/CLCW feedback, and LINK
  diagnostics are implemented and pass the automated native and board-build
  checks. The recorded two-board Stage 2 recovery checklist remains pending.
- The next implementation item is the prompt single-transaction post-recovery
  OTAR described above. The implementation brief is `sdls_stage3_prompt.md`.
- Until fresh operational keys are installed, fixed-key adoption can
  authenticate a recorded valid frame. This milestone shortens that interval;
  it does not cryptographically prove that the OTAR belongs to the current
  restart epoch.
