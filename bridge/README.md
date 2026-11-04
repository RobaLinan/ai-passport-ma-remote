<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# MA Passport voice bridge

Local bridge for the AI Passport Music Assistant remote. It receives 16 kHz mono
16-bit PCM from the device, transcribes it with a configurable STT service
(Home Assistant / Nabu Casa, local Whisper, any OpenAI-compatible endpoint, or
Azure), asks DeepSeek to turn the transcript into a small JSON intent, and
executes the result against Music Assistant.

Supported intents: play an artist, play a playlist, play a random recent track.
Transport and volume are handled by the device buttons, not by voice.

## Requirements

- Python 3.10+
- One speech-to-text service:
  - Home Assistant with an Assist pipeline using Nabu Casa Cloud STT or local Whisper
  - A local Whisper server speaking the Wyoming protocol (e.g. the HA Whisper add-on)
  - Any OpenAI-compatible `/v1/audio/transcriptions` endpoint
  - Azure Speech
- A Music Assistant API token
- A DeepSeek API key (DeepSeek is the only supported LLM for now)

## Choosing STT

`STT_PROVIDER` selects the service; `STT_LANGUAGE` sets the language (default
`zh-CN`; the old `HA_LANGUAGE` is still accepted):

| STT_PROVIDER | Description | Required settings |
| --- | --- | --- |
| `ha` | HA Assist pipeline, any STT engine | `HA_URL`, `HA_TOKEN_FILE`; optional `HA_PIPELINE_ID`, `HA_STT_ENGINE` |
| `nabucasa` | HA Assist pipeline restricted to the Nabu Casa Cloud engine | Same as `ha`, engine defaults to `cloud` |
| `wyoming` | Local Whisper over the Wyoming protocol | `WYOMING_STT_HOST` / `WYOMING_STT_PORT`, default `127.0.0.1:10300` |
| `openai` | OpenAI-compatible transcription (whisper.cpp, faster-whisper-server, OpenAI, Groq, ...) | `OPENAI_STT_URL`; optional `OPENAI_STT_MODEL`, `OPENAI_STT_API_KEY` |
| `azure` | Azure Speech short-audio REST | `AZURE_SPEECH_REGION` or `AZURE_SPEECH_ENDPOINT`, plus `AZURE_SPEECH_KEY` |

Home Assistant is only needed for `ha` / `nabucasa`. All optional settings are
listed in `.env.example`.

## Install

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
cp .env.example .env
mkdir -p secrets
printf '%s' 'YOUR_MA_TOKEN' > secrets/MA_TOKEN
printf '%s' 'YOUR_DEEPSEEK_KEY' > secrets/DEEPSEEK_API_KEY
# add the secret for your STT_PROVIDER:
# printf '%s' 'YOUR_HA_LONG_LIVED_TOKEN' > secrets/HA_TOKEN           # ha / nabucasa
# printf '%s' 'YOUR_AZURE_SPEECH_KEY' > secrets/AZURE_SPEECH_KEY       # azure
# printf '%s' 'YOUR_API_KEY' > secrets/OPENAI_STT_KEY                  # openai, optional
chmod 600 .env secrets/*
.venv/bin/python bridge.py
```

Alternatively set the matching keys directly in `.env` (environment variables
take precedence over files).

## Endpoints

- `GET /health` — STT provider, pipeline and player
- `POST /text` — `{"text":"play Eagles","dry_run":true}` for testing
- `POST /stt` — raw 16 kHz mono s16le PCM body, returns the transcript
- `POST /voice` — raw PCM body; query `player_id`/`queue_id` are optional,
  `dry_run=1` only searches

The device sends 5 seconds of PCM per press-to-talk gesture (padded with silence).
`DEBUG_AUDIO=1` saves the last clip to `/tmp/last_voice.pcm`/`.wav` and logs the
peak/RMS; it is off by default.

## Customizing intents

The DeepSeek system prompt is `intent_prompt.txt` next to `bridge.py`; keep that
file alongside the script when deploying. Edit it and restart the service, or
set `INTENT_PROMPT_FILE` to use a different path.

The prompt must keep returning the JSON contract understood by the bridge:
`play_media` with `media_type` `artist` or `playlist` and 1-3 `queries`,
`play_random`, or `unsupported`. Changing that contract also requires updating
`parse_intent()` and the command handlers.

## systemd (user service)

```bash
mkdir -p ~/.config/systemd/user
cp deploy/ma-bridge.service ~/.config/systemd/user/ma-bridge.service
# edit WorkingDirectory/EnvironmentFile if you did not use ~/ma-bridge
systemctl --user daemon-reload
systemctl --user enable --now ma-bridge
loginctl enable-linger "$USER"   # start without an interactive login
journalctl --user -u ma-bridge -f
```

Allow the device to reach the bridge from the LAN, for example:

```bash
sudo ufw allow from 192.168.1.0/24 to any port 8096 proto tcp
```

Keep the bridge on the LAN. The device does not send a bridge token, so do not
expose the bridge directly to the internet; use a trusted LAN or put it behind a
reverse proxy with its own authentication. The optional `BRIDGE_TOKEN` setting
only works for clients that send the matching `X-Bridge-Token` header.

## Device configuration

The Passport's setup AP collects the WiFi credentials, the MA host/port/token and
the bridge host/port. Use this bridge's LAN address (for example
`192.168.1.20`) and port `8096`.

`MA_PLAYER_ID` is optional. If omitted, the bridge selects the currently
playing player, otherwise a paused player with media, otherwise the first usable
player. The device also re-resolves its own target on boot and screen wake.

## Notes

- The intent prompt (`intent_prompt.txt`) and UI are Chinese; set `STT_LANGUAGE`
  to your STT language (the old `HA_LANGUAGE` still works).
- `ha` picks an Assist pipeline by language; `nabucasa` only accepts the Nabu
  Casa Cloud engine and fails with the available engine list if none matches.
- Artist aliases are handled by asking DeepSeek for translated/original candidate
  names before searching the MA library.
- Random playback uses the recently played tracks, falling back to recently added
  tracks filtered to 1–10 minutes.
- Secrets are read from files or environment variables; never commit them.
