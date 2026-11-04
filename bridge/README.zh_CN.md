<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# MA Passport 语音 bridge

AI Passport 遥控器的局域网语音 bridge：接收设备上传的 16 kHz 单声道 16-bit PCM，用可配置的 STT 服务转成文字（Home Assistant / Nabu Casa、本地 Whisper、OpenAI 兼容接口或 Azure），让 DeepSeek 输出受限制的 JSON 意图，然后调用 Music Assistant 执行。

支持的意图：播放歌手、播放歌单、随机播放最近听过的歌曲。播放/暂停、切歌和音量由设备按键控制，不走语音。

## 依赖

- Python 3.10+
- 一个语音识别服务，任选其一：
  - Home Assistant，Assist pipeline 配好 Nabu Casa Cloud STT 或本地 Whisper
  - 本地 Whisper，走 Wyoming 协议（例如 HA Whisper 加载项）
  - OpenAI 兼容的 `/v1/audio/transcriptions` 接口
  - Azure 语音服务
- Music Assistant API token
- DeepSeek API key（AI 服务目前只支持 DeepSeek）

## 选择 STT

`STT_PROVIDER` 决定用哪个识别服务，语言由 `STT_LANGUAGE` 指定（默认 `zh-CN`，兼容旧的 `HA_LANGUAGE`）：

| STT_PROVIDER | 说明 | 需要的配置 |
| --- | --- | --- |
| `ha` | HA Assist pipeline，任意 STT 引擎 | `HA_URL`、`HA_TOKEN_FILE`；可选 `HA_PIPELINE_ID`、`HA_STT_ENGINE` |
| `nabucasa` | HA Assist pipeline，只用 Nabu Casa Cloud 引擎 | 同上，引擎默认 `cloud` |
| `wyoming` | 本地 Whisper（Wyoming 协议） | `WYOMING_STT_HOST` / `WYOMING_STT_PORT`，默认 `127.0.0.1:10300` |
| `openai` | OpenAI 兼容转写接口（whisper.cpp、faster-whisper-server、OpenAI、Groq 等） | `OPENAI_STT_URL`；可选 `OPENAI_STT_MODEL`、`OPENAI_STT_API_KEY` |
| `azure` | Azure 语音短音频转写 REST | `AZURE_SPEECH_REGION` 或 `AZURE_SPEECH_ENDPOINT`，加 `AZURE_SPEECH_KEY` |

除 `ha` / `nabucasa` 外不需要 Home Assistant。各服务的可选变量见 `.env.example`。

## 安装

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
cp .env.example .env
mkdir -p secrets
printf '%s' '你的 MA token' > secrets/MA_TOKEN
printf '%s' '你的 DeepSeek key' > secrets/DEEPSEEK_API_KEY
# 按所选 STT_PROVIDER 添加：
# printf '%s' '你的 HA 长期令牌' > secrets/HA_TOKEN             # ha / nabucasa
# printf '%s' '你的 Azure 语音密钥' > secrets/AZURE_SPEECH_KEY   # azure
# printf '%s' '你的 API key' > secrets/OPENAI_STT_KEY            # openai，本地服务可省略
chmod 600 .env secrets/*
.venv/bin/python bridge.py
```

也可以把对应的 key 直接写进 `.env`；环境变量优先于文件。

## 接口

- `GET /health`：STT provider、pipeline、播放器
- `POST /text`：`{"text":"播放老鹰乐队的歌","dry_run":true}`
- `POST /stt`：16 kHz mono s16le PCM，返回文字
- `POST /voice`：PCM 上传；可选 `player_id`/`queue_id`、`dry_run=1`

设备每次按住说话上传 5 秒 PCM（不足补静音）。`DEBUG_AUDIO=1` 会把最后一段存到 `/tmp/last_voice.pcm/.wav` 并打印峰值；默认关闭。

## 自定义意图

DeepSeek 的系统提示词在 `bridge.py` 同目录的 `intent_prompt.txt`；部署时请把这个文件
和脚本放在一起。修改后重启服务，也可以用 `INTENT_PROMPT_FILE` 指向其他路径。

提示词必须保持 bridge 能解析的 JSON 契约：`play_media` 配 `media_type` `artist` 或
`playlist` 和 1-3 个 `queries`，或 `play_random`、`unsupported`。如果修改契约，
还需要同步修改 `parse_intent()` 和命令执行逻辑。

## systemd（user 服务）

```bash
mkdir -p ~/.config/systemd/user
cp deploy/ma-bridge.service ~/.config/systemd/user/ma-bridge.service
# 如果不是 ~/ma-bridge，修改 WorkingDirectory/EnvironmentFile
systemctl --user daemon-reload
systemctl --user enable --now ma-bridge
loginctl enable-linger "$USER"
journalctl --user -u ma-bridge -f
```

设备需要能从局域网访问 8096，例如：

```bash
sudo ufw allow from 192.168.1.0/24 to any port 8096 proto tcp
```

bridge 只应放在可信局域网；设备目前不发送 bridge token，不要直接暴露到公网。可选 `BRIDGE_TOKEN` 只对会发送 `X-Bridge-Token` 的客户端生效。

## 设备配置

Passport 首次开机进入 `Passport-XXXX` 配网热点，在 `192.168.4.1` 填写 WiFi、MA 地址/端口/token 和 bridge 地址/端口。bridge 地址填这台服务器的局域网 IP，端口 8096。

`MA_PLAYER_ID` 可省略；省略时 bridge 自动选择正在播放的播放器，其次是有媒体的暂停播放器，最后是第一个可用播放器。设备自己也会在开机和亮屏唤醒时重新解析目标。

## 说明

- 意图提示词（`intent_prompt.txt`）和界面是中文；用 `STT_LANGUAGE` 匹配你的 STT 语言（旧配置里的 `HA_LANGUAGE` 仍兼容）。
- `ha` 模式下按语言自动挑选 Assist pipeline；`nabucasa` 只接受 Nabu Casa Cloud 引擎，找不到会直接报错并列出可用引擎。
- 歌手别名由 DeepSeek 生成译名/原名候选，再在 MA 库里搜索。
- 随机播放优先用最近播放的 track，不足时回退到最近添加并过滤 1–10 分钟。
- 密钥只从文件或环境变量读取，不要提交到仓库。
