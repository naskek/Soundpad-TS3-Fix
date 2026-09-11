# Soundpad-TS3-Diag

Diagnostic tooling for tracing Soundpad audio through Windows and TeamSpeak 3.

## Current target

- Windows x64
- TeamSpeak 3 **3.6.2**
- TeamSpeak plugin API **26**
- Soundpad

TeamSpeak 3.6 uses plugin API 26, and the official TeamSpeak plugin template currently declares `PLUGIN_API_VERSION 26`.

## What is implemented now

The first TS3-side diagnostic plugin is in the repository.

It uses TeamSpeak's `ts3plugin_onEditCapturedVoiceDataEvent` callback. TeamSpeak documents this callback as receiving audio **after capture-device recording and preprocessing**, before the normal encode/transmit path.

For each diagnostic run the plugin records:

- `ts3_processed.wav` — PCM after TeamSpeak preprocessing;
- `ts3_frames.csv` — callback timing, RMS level and SEND/DROP state;
- `ts3_settings.txt` — current preprocessor settings;
- `summary.txt` — duration, SEND/DROP counts and DROP ratio.

The callback is observational only: the plugin does **not** modify the audio samples or TeamSpeak's SEND/DROP decision.

Captures are written under:

```text
%LOCALAPPDATA%\Soundpad-TS3-Diag\captures\<timestamp>\
```

## Build locally

From PowerShell in the repository root:

```powershell
.\scripts\build.ps1
```

The script downloads the official TeamSpeak 3 Client Plugin SDK into `vendor/` if necessary, then builds the x64 Release DLL.

Expected output:

```text
build\Release\soundpad_ts3_diag_win64.dll
```

Install it with:

```powershell
.\scripts\install.ps1
```

Then restart TeamSpeak 3.

## First diagnostic run

After the plugin is loaded in TeamSpeak:

```text
/spdiag settings
/spdiag start
```

Play one known-problematic Soundpad sound through the microphone path, then run:

```text
/spdiag stop
```

The resulting capture folder contains the files listed above.

Useful commands:

```text
/spdiag start
/spdiag stop
/spdiag status
/spdiag settings
```

## What this proves

This first stage observes the **TeamSpeak side**.

If the resulting WAV already has missing or damaged portions, TeamSpeak preprocessing is implicated.

If PCM is present but frames are marked DROP, TeamSpeak's gating/VAD decision is implicated.

## Next phase

Add a Windows-side orchestrator that simultaneously records the same capture endpoint **before TeamSpeak preprocessing** and later controls Soundpad's Remote Control interface.

The final comparison will be:

```text
Soundpad
   |
   v
Windows capture endpoint  -> raw reference
   |
   v
TeamSpeak preprocessing   -> TS3 processed reference + SEND/DROP
   |
   v
encoder / server
```

See `docs/architecture.md` for the design.
