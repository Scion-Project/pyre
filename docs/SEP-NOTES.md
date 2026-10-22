# SEP notes: iOS 14.8 credential/keystore errors on t8030

These are working notes on SEP request failures seen with the real SEP ROM and
sepOS running on the emulated SEP core (`hw/arm/sep/`). They are written from
the emulator side only. Nothing here comes from disassembling sepOS.

## Symptom

On the iPhone 11 (t8030) iOS 14.8 boot to userland with real SEP inputs:

```
AppleCredentialManager: handleSEPMessage: SEP returned error, request = 4, info = 94, length = 16384.
AppleCredentialManager: sendSEPCommand: SEP cmd(25) took 7 ms -> ioErr=0x0 acmErr=94.
AppleSEPKeyStore:10649:91: operation failed (sel: 17 ret: e00002c2)
AppleCredentialManager: sendSEPCommand: SEP cmd(36) took 7 ms -> ioErr=0x0 acmErr=94.   (x2)
```

What the log establishes:

- The transport works. `ioErr=0x0`, the reply arrives in 7–12 ms, and the
  mailbox/OOL path returns a full 16 KiB response buffer. The error is sepOS's
  own answer, carried in the response, not a timeout or a dropped message.
- Other requests on the same endpoints succeed in the same boot: ACM cmd 10;
  AKS keybag load, media-key unwrap and migration; xART/gigalocker reads and
  writes (`gl_rec_write`). So sepOS key unwrap and the SEP-xART path work at
  least once in this boot.
- `e00002c2` is `kIOReturnBadArgument`.
- ACM `info = 94` is an ACM-internal status. Its meaning is not published, so
  nothing below assumes one.
- The failing operations are ACM `GetEnvironmentVariable`-style queries
  (cmd 25 runs inside `LibCall_ACMGetEnvironmentVariable`) and one AKS
  selector, all issued after the data volume is mounted. None of them is
  fatal: boot continues.

## Why this is not reproduced here

The shared bench kit (`~/inferno/research14` on the Mac mini) uses a
placeholder `sep-fw` and an idle (`wfi`) SEP ROM, and it boots only the
restore ramdisk. sepOS never runs there, so the failing requests cannot be
replayed, and no real t8030 SEP inputs exist on that host. Firmware and keys
are not fetched or copied into this repo or onto the bench.

## Model behaviours that can produce a "valid but failed" sepOS answer

These spots return success at the register level while doing less than the
hardware does. Each can make sepOS fail a request without any QEMU error
appearing in the serial log. All file references are to `hw/arm/sep/`.

| Block | Behaviour | Where |
|---|---|---|
| AESS | Commands outside the modelled set (`0x00`, `0x01`, `0x02/0x12` keywrap, `0x0B`, `0x10`, the CBC variants) complete with a zeroed output buffer and no error interrupt. The source lists `0x205/0x204/0x245/0x244` as known but unmodelled sync commands. | `aes.c` "TODO: other sync commands" |
| AESS | Output buffer is cleared before every command ("not correct behavior"). | `aes.c` top of `apple_sep_aess_handle_cmd` |
| PKA | TMM registers `0x818..0x834` read back whatever was last written. | `pka.c` `pka_tmm_reg_read` |
| SSC (anti-replay store, `sep_ssc` pflash) | Metadata *write* (cmd 3) rejects `copy > 0` with `COMMAND_OR_FIELD_INVALID`, while metadata *read* (cmd 6) and data read/write (cmds 4/5) accept copies 0–3. | `ssc.c` `answer_cmd_0x3_metadata_write` |
| SSC | Only 0x49 key slots are modelled (a comment notes 0x100 may be needed on newer iOS). Slots above that answer `KEYSLOT_INVALID`. | `ssc.c` `KBKDF_KEY_MAX_SLOTS` |
| SSC | Panic/info record (cmd 9) returns 0x24 bytes of `0xCC` and a fake CPSN. | `ssc.c` `answer_cmd_0x9_panic` |

The SSC rows are the first to check. Persistent SEP-side state such as ACM
settings is expected to depend on anti-replay storage, and the earlier xART
traffic in the same boot already shows `USER_XART_LOCKER ... not found`. This
is a hypothesis to test with the capture below, not a finding.

## Capture recipe (needs a boot with real SEP inputs)

The trace points added with these notes are compiled out in default builds,
because the fork sets `trace_backends=nop`.

1. Configure with `-Dtrace_backends=log`. For the Android build, add it to the
   QEMU `meson setup` line in `native/build-android.sh`.
2. Boot with:
   ```
   -trace 'apple_sep_aess_*' -trace 'apple_sep_ssc_*' -trace 'apple_sep_pka_*' \
   -d guest_errors,unimp -D sep-trace.log
   ```
3. In `sep-trace.log`, look at the window around each ACM cmd 25/36 and the
   AKS sel 17 failure in the serial log:
   - `apple_sep_aess_cmd_unknown`: sepOS used an AESS command the model leaves
     zeroed. Implement it.
   - `apple_sep_ssc_resp cmd 0x3 -> flags 0x04` right after an
     `apple_sep_ssc_cmd cmd 0x3 ... [3]=0x01..0x03`: a metadata write to copy
     1–3 is being refused (see the table above).
   - any `apple_sep_ssc_resp` flags other than `0x80`, `apple_sep_aess_cmd_error`,
     or `apple_sep_pka_tmm_unmodelled_read`.
4. For sepOS's own view, uncomment `SEP_ENABLE_TRACE_BUFFER` in
   `hw/arm/sep/private.h`. `debug-trace.c` then decodes sepOS's shared-memory
   trace buffer (function/event IDs) into the QEMU log. It is iOS-version
   specific (`SEP_USE_VERSION_OVERRIDE 14`).

If the failing request turns out to need device-unique material (UID-derived
keys, or anti-replay state provisioned at the factory), it cannot be emulated
faithfully. That case should be recorded here and left alone.

## Fixed alongside these notes

- `ssc.c` cmd 1 (connect SP) used the request's key-slot byte (0–255) without
  checking it against the 0x49-slot tables. That let a guest derive keys into
  memory past the end of the state struct. Out-of-range slots now answer
  `KEYSLOT_INVALID`, like the other commands.
