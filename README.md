# Soundpad TS3 Fix

Automatic TeamSpeak 3 capture-DSP bypass for Soundpad, with built-in diagnostics.

## Current target

- Windows x64
- TeamSpeak 3 **3.6.2**
- TeamSpeak plugin API **26**
- Soundpad

TeamSpeak 3.6 uses plugin API 26, and the official TeamSpeak plugin template currently declares `PLUGIN_API_VERSION 26`.

## What is implemented now

The plugin now has two roles:

1. **Automatic Soundpad DSP bypass** — enabled by default on every TeamSpeak start. It watches Soundpad's local Remote Control named pipe and, while Soundpad reports `PLAYING` or `SEEKING`, snapshots supported TeamSpeak capture-DSP booleans and temporarily disables only those that were enabled. When playback stops, pauses, Soundpad closes, or the plugin unloads, each changed setting is restored to its exact pre-playback value. Current managed/probed identifiers include `vad`, `denoise`, `agc`, `echo_canceling`, plus runtime probes for older/alternate profile identifiers such as `echo_reduction`, `echo_cancellation`, and `typing_attenuation`; unsupported identifiers are skipped.
2. **Diagnostics** — capture post-preprocessor PCM and TeamSpeak SEND/DROP telemetry for A/B testing.

It uses TeamSpeak's `ts3plugin_onEditCapturedVoiceDataEvent` callback. TeamSpeak documents this callback as receiving audio **after capture-device recording and preprocessing**, before the normal encode/transmit path.

For each diagnostic run the plugin records:

- `ts3_processed.wav` — PCM after TeamSpeak preprocessing;
- `ts3_frames.csv` — callback timing, RMS level and SEND/DROP state;
- `ts3_settings.txt` — current preprocessor settings;
- `summary.txt` — duration, SEND/DROP counts and DROP ratio.

The callback is observational only: the plugin does **not** modify the audio samples or TeamSpeak's SEND/DROP decision.

Captures are written under:

```text
%LOCALAPPDATA%\Soundpad-TS3-Fix\captures\<timestamp>\
```

## Install

Release builds are packaged as:

```text
soundpad_ts3_fix.ts3_plugin
```

Double-click the package and let TeamSpeak install it, then restart TeamSpeak 3. The plugin autoloads and the automatic DSP bypass is enabled by default.

The DLL inside the package intentionally keeps the historical filename `soundpad_ts3_diag_win64.dll` so upgrades overwrite older development builds instead of leaving two copies installed.

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

Create an installable TeamSpeak package with:

```powershell
.\scripts\package.ps1
```

Package output:

```text
dist\soundpad_ts3_fix.ts3_plugin
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
/spdiag auto on
/spdiag auto off
/spdiag auto status
/spdiag vad on
/spdiag vad off
/spdiag denoise on
/spdiag denoise off
```

The automatic mode is intentionally **ON by default** and is not something the user needs to remember to enable. It polls Soundpad locally only; it does not install a Windows service, driver, virtual audio device, or network component. Restoration is state-preserving rather than hardcoded: for example, if `denoise` was already `false`, auto mode leaves it alone and never forces it back to `true`.

## What this proves

This first stage observes the **TeamSpeak side**.

If the resulting WAV already has missing or damaged portions, TeamSpeak preprocessing is implicated.

If PCM is present but frames are marked DROP, TeamSpeak's gating/VAD decision is implicated.

## Diagnostic next phase

If deeper audio-quality analysis is needed, add a Windows-side orchestrator that simultaneously records the same capture endpoint **before TeamSpeak preprocessing**.

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
