# Soundpad-TS3-Diag

Diagnostic tooling for tracing audio from Soundpad through the Windows capture endpoint into TeamSpeak 3.

## Goal

Determine where Soundpad audio is being altered or dropped:

```text
Soundpad
  -> Windows capture endpoint
  -> TeamSpeak 3 capture/pre-processing
  -> TeamSpeak transmit path
```

The first useful version will record the same test run at two points:

- raw Windows capture endpoint;
- TeamSpeak 3 captured PCM via a TS3 client plugin.

It will then produce WAV files plus a machine-readable report so the two signals can be compared.

## Planned components

```text
src/
  orchestrator/   Windows diagnostic runner
  ts3-plugin/     TeamSpeak 3 client plugin
docs/
  architecture.md
artifacts/        local diagnostic output (gitignored)
```

The orchestrator will later also control Soundpad through its Remote Control interface so the same sound can be replayed reproducibly.

## Status

Repository initialized. Implementation is being prepared against the exact TeamSpeak 3 client/plugin API version installed on the test machine.
