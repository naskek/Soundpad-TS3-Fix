# Architecture

## Target environment

Initial target:

- Windows x64
- TeamSpeak 3 client 3.6.2
- TeamSpeak plugin API 26
- Soundpad
- TeamSpeak capture path at 48 kHz / signed 16-bit PCM

The official TeamSpeak 3 plugin template currently declares plugin API version 26.

## Diagnostic points

The complete intended pipeline is:

```text
Soundpad
   |
   v
Windows capture endpoint
   |        Point A (planned orchestrator)
   v
TeamSpeak 3 capture + preprocessing
   |        Point B (implemented by TS3 plugin)
   v
VAD / SEND decision
   |
   v
encoder -> server
```

### Point B: implemented

TeamSpeak's `ts3plugin_onEditCapturedVoiceDataEvent` callback is invoked after audio is recorded from the capture device and preprocessed.

The plugin records:

- post-preprocessor PCM to `ts3_processed.wav`;
- each callback frame's SEND/DROP state;
- RMS level of each callback frame;
- current preprocessor configuration;
- a summary with the observed DROP ratio.

The plugin never changes the audio buffer or SEND/DROP flag.

### Point A: next phase

A Windows-side orchestrator will open the same Windows capture endpoint in shared mode and record a raw reference at the same time.

That gives us two comparable signals:

1. what reached the Windows capture endpoint;
2. what remained after TeamSpeak preprocessing.

The orchestrator will later control Soundpad so repeated test runs can use the exact same sound automatically.

## Why both points matter

If Point A is intact but Point B is damaged, the fault is inside TeamSpeak preprocessing.

If Point A is already damaged, the fault occurs before TeamSpeak, such as in the Soundpad/Windows capture path.

If Point B contains audio while TeamSpeak marks frames DROP, VAD/gating is implicated.
