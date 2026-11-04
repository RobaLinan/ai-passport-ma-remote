import asyncio
import io
import json
import logging
import os
import random
import time
import wave
from array import array
from pathlib import Path

from aiohttp import ClientSession, ClientTimeout, FormData, web

LOG_LEVEL = os.environ.get("LOG_LEVEL", "INFO").upper()
logging.basicConfig(
    level=getattr(logging, LOG_LEVEL, logging.INFO),
    format="%(asctime)s %(levelname)s %(message)s",
)
log = logging.getLogger("bridge")

STT_PROVIDERS = ("ha", "nabucasa", "wyoming", "openai", "azure")
STT_PROVIDER = os.environ.get("STT_PROVIDER", "ha").strip().lower()
STT_LANGUAGE = os.environ.get("STT_LANGUAGE", "").strip() or os.environ.get(
    "HA_LANGUAGE", "zh-CN"
)
HA_URL = os.environ.get("HA_URL", "").rstrip("/")
HA_TOKEN_FILE = os.environ.get("HA_TOKEN_FILE", "secrets/HA_TOKEN")
HA_PIPELINE_ID = os.environ.get("HA_PIPELINE_ID")
HA_STT_ENGINE = os.environ.get("HA_STT_ENGINE", "").strip()
WYOMING_STT_HOST = os.environ.get("WYOMING_STT_HOST", "127.0.0.1").strip()
WYOMING_STT_PORT = int(os.environ.get("WYOMING_STT_PORT", "10300"))
WYOMING_STT_TIMEOUT = int(os.environ.get("WYOMING_STT_TIMEOUT", "60"))
OPENAI_STT_URL = os.environ.get("OPENAI_STT_URL", "").strip()
OPENAI_STT_MODEL = os.environ.get("OPENAI_STT_MODEL", "whisper-1")
OPENAI_STT_KEY_FILE = os.environ.get("OPENAI_STT_KEY_FILE", "secrets/OPENAI_STT_KEY")
AZURE_SPEECH_REGION = os.environ.get("AZURE_SPEECH_REGION", "").strip()
AZURE_SPEECH_ENDPOINT = os.environ.get("AZURE_SPEECH_ENDPOINT", "").strip()
AZURE_SPEECH_KEY_FILE = os.environ.get("AZURE_SPEECH_KEY_FILE", "secrets/AZURE_SPEECH_KEY")
MA_URL = os.environ.get("MA_URL", "http://127.0.0.1:8095").rstrip("/")
MA_TOKEN_FILE = os.environ.get("MA_TOKEN_FILE", "secrets/MA_TOKEN")
DEEPSEEK_URL = os.environ.get(
    "DEEPSEEK_URL", "https://api.deepseek.com/v1/chat/completions"
)
DEEPSEEK_MODEL = os.environ.get("DEEPSEEK_MODEL", "deepseek-chat")
DEEPSEEK_KEY_FILE = os.environ.get("DEEPSEEK_KEY_FILE", "secrets/DEEPSEEK_API_KEY")
DEFAULT_PLAYER_ID = os.environ.get("MA_PLAYER_ID", "").strip()
BRIDGE_TOKEN = os.environ.get("BRIDGE_TOKEN", "")
DEBUG_AUDIO = os.environ.get("DEBUG_AUDIO", "0") in ("1", "true", "yes")
PORT = int(os.environ.get("PORT", "8096"))
MAX_PCM_BYTES = 2 * 1024 * 1024

INTENT_PROMPT = """你是音乐遥控器。把用户的话转换成 JSON，只输出 JSON。
格式: {"intent":"play_media","media_type":"artist","queries":["候选1","候选2"]}
或:   {"intent":"play_media","media_type":"playlist","queries":["候选1"]}
只有用户想播放某位歌手或某个歌单时才用 play_media。
如果用户想“随机播放/随便放首歌/来点音乐/播放一个随机的歌”，输出 {"intent":"play_random"}。
其他意图输出 {"intent":"unsupported"}。
queries 是 1-3 个搜索词，按优先级排列，只包含歌手/歌单名称，去掉“播放、放、来点、的歌、歌单”等词和标点。
规则：
- 用户原话中的名字必须保留在 queries 里。
- 如果用户说的是外国歌手/乐队的中文译名（例如“老鹰乐队”），把常用英文/原名放第一位，并保留中文名在最后，例如 ["Eagles","The Eagles","老鹰乐队"]。
- 中文歌手保持中文名在前；不确定时不要翻译，保留原名。
不要解释，不要输出多余字段。"""


def read_secret(value_env: str, file_env: str, default_file: str) -> str:
    value = os.environ.get(value_env, "").strip()
    if value:
        return value
    path = os.environ.get(file_env, default_file)
    return Path(path).read_text(encoding="utf-8").strip()


def read_optional_secret(value_env: str, file_env: str, default_file: str) -> str:
    value = os.environ.get(value_env, "").strip()
    if value:
        return value
    path = os.environ.get(file_env, default_file)
    try:
        return Path(path).read_text(encoding="utf-8").strip()
    except FileNotFoundError:
        return ""


def pcm_to_wav(pcm: bytes) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(16000)
        wav.writeframes(pcm[: len(pcm) // 2 * 2])
    return buf.getvalue()


def wyoming_dump(event: dict, payload: bytes = b"") -> bytes:
    return (json.dumps(event, ensure_ascii=False) + "\n").encode() + payload


async def read_wyoming_event(reader: asyncio.StreamReader):
    line = await reader.readline()
    if not line:
        return None
    event = json.loads(line)
    length = int((event.get("data") or {}).get("payload_length") or 0)
    if length:
        await reader.readexactly(length)
    return event


class Bridge:
    def __init__(self):
        self.ha_token = (
            read_secret("HA_TOKEN", "HA_TOKEN_FILE", HA_TOKEN_FILE)
            if STT_PROVIDER in ("ha", "nabucasa")
            else ""
        )
        self.azure_key = (
            read_secret(
                "AZURE_SPEECH_KEY", "AZURE_SPEECH_KEY_FILE", AZURE_SPEECH_KEY_FILE
            )
            if STT_PROVIDER == "azure"
            else ""
        )
        self.openai_key = (
            read_optional_secret(
                "OPENAI_STT_API_KEY", "OPENAI_STT_KEY_FILE", OPENAI_STT_KEY_FILE
            )
            if STT_PROVIDER == "openai"
            else ""
        )
        self.ma_token = read_secret("MA_TOKEN", "MA_TOKEN_FILE", MA_TOKEN_FILE)
        self.deepseek_key = read_secret(
            "DEEPSEEK_API_KEY", "DEEPSEEK_API_KEY_FILE", DEEPSEEK_KEY_FILE
        )
        self.session: ClientSession | None = None
        self.pipeline_id: str | None = None
        self.pipeline_name = ""

    async def start(self):
        if STT_PROVIDER not in STT_PROVIDERS:
            raise RuntimeError(
                f"unknown STT_PROVIDER '{STT_PROVIDER}' (use one of: {', '.join(STT_PROVIDERS)})"
            )
        if not self.ma_token:
            raise RuntimeError("MA token is required (MA_TOKEN or MA_TOKEN_FILE)")
        if not self.deepseek_key:
            raise RuntimeError(
                "DeepSeek key is required (DEEPSEEK_API_KEY or DEEPSEEK_API_KEY_FILE)"
            )
        if STT_PROVIDER in ("ha", "nabucasa"):
            if not HA_URL:
                raise RuntimeError(
                    "HA_URL is required for STT_PROVIDER=ha/nabucasa "
                    "(for example http://homeassistant.local:8123)"
                )
            if not self.ha_token:
                raise RuntimeError("HA token is required (HA_TOKEN or HA_TOKEN_FILE)")
        elif STT_PROVIDER == "azure":
            if not (AZURE_SPEECH_REGION or AZURE_SPEECH_ENDPOINT):
                raise RuntimeError(
                    "AZURE_SPEECH_REGION or AZURE_SPEECH_ENDPOINT is required for STT_PROVIDER=azure"
                )
            if not self.azure_key:
                raise RuntimeError(
                    "Azure Speech key is required (AZURE_SPEECH_KEY or AZURE_SPEECH_KEY_FILE)"
                )
        elif STT_PROVIDER == "openai":
            if not OPENAI_STT_URL:
                raise RuntimeError(
                    "OPENAI_STT_URL is required for STT_PROVIDER=openai "
                    "(for example http://127.0.0.1:8080/v1/audio/transcriptions)"
                )
        self.session = ClientSession(timeout=ClientTimeout(total=45))
        if STT_PROVIDER in ("ha", "nabucasa"):
            await self.discover_pipeline()
        log.info("STT provider: %s (%s)", STT_PROVIDER, STT_LANGUAGE)

    async def stop(self):
        if self.session:
            await self.session.close()

    @property
    def ha_ws_url(self) -> str:
        scheme = "wss" if HA_URL.startswith("https") else "ws"
        return HA_URL.replace("https", scheme, 1).replace("http", scheme, 1) + "/api/websocket"

    async def ha_connect(self):
        ws = await self.session.ws_connect(self.ha_ws_url, timeout=10)
        msg = await ws.receive_json(timeout=10)
        if msg.get("type") != "auth_required":
            await ws.close()
            raise RuntimeError(f"unexpected HA auth message: {msg.get('type')}")
        await ws.send_json({"type": "auth", "access_token": self.ha_token})
        msg = await ws.receive_json(timeout=10)
        if msg.get("type") != "auth_ok":
            await ws.close()
            raise RuntimeError(f"HA auth failed: {msg.get('type')}")
        return ws

    async def discover_pipeline(self):
        ws = await self.ha_connect()
        try:
            await ws.send_json({"id": 1, "type": "assist_pipeline/pipeline/list"})
            while True:
                msg = await ws.receive_json(timeout=10)
                if msg.get("type") == "result":
                    if not msg.get("success"):
                        raise RuntimeError(f"pipeline list failed: {msg}")
                    pipelines = msg.get("result", {}).get("pipelines", [])
                    break
        finally:
            await ws.close()

        engine_filter = HA_STT_ENGINE.lower()
        if not engine_filter and STT_PROVIDER == "nabucasa":
            engine_filter = "cloud"

        if HA_PIPELINE_ID:
            selected = next(
                (p for p in pipelines if p.get("id") == HA_PIPELINE_ID), None
            )
            if not selected:
                raise RuntimeError(f"HA pipeline {HA_PIPELINE_ID} not found")
        else:
            candidates = pipelines
            if engine_filter:
                candidates = [
                    p
                    for p in candidates
                    if engine_filter in str(p.get("stt_engine", "")).lower()
                ]
            lang_prefix = STT_LANGUAGE.split("-")[0].lower()
            selected = next(
                (
                    p
                    for p in candidates
                    if str(p.get("stt_language", ""))
                    .lower()
                    .startswith(lang_prefix)
                ),
                None,
            )
            if selected is None and candidates:
                selected = candidates[0]
            if not selected:
                engines = sorted({str(p.get("stt_engine")) for p in pipelines})
                raise RuntimeError(
                    "no Assist pipeline with STT engine "
                    f"'{engine_filter}' (available: {', '.join(engines) or 'none'})"
                )
        self.pipeline_id = selected["id"]
        self.pipeline_name = selected.get("name", "")
        log.info(
            "HA pipeline: %s (%s), stt=%s/%s",
            self.pipeline_name,
            self.pipeline_id,
            selected.get("stt_engine"),
            selected.get("stt_language"),
        )

    async def transcribe(self, pcm: bytes) -> str:
        if STT_PROVIDER in ("ha", "nabucasa"):
            return await self.transcribe_ha(pcm)
        if STT_PROVIDER == "wyoming":
            return await self.transcribe_wyoming(pcm)
        if STT_PROVIDER == "openai":
            return await self.transcribe_openai(pcm)
        if STT_PROVIDER == "azure":
            return await self.transcribe_azure(pcm)
        raise RuntimeError(f"unsupported STT provider: {STT_PROVIDER}")

    async def transcribe_ha(self, pcm: bytes) -> str:
        ws = await self.ha_connect()
        try:
            await ws.send_json(
                {
                    "id": 2,
                    "type": "assist_pipeline/run",
                    "start_stage": "stt",
                    "end_stage": "stt",
                    "input": {"sample_rate": 16000},
                    "pipeline": self.pipeline_id,
                }
            )
            handler_id = None
            while True:
                msg = await ws.receive_json(timeout=15)
                if msg.get("type") == "result" and not msg.get("success"):
                    raise RuntimeError(f"pipeline run failed: {msg.get('error')}")
                if msg.get("type") != "event":
                    continue
                event = msg["event"]
                etype = event.get("type")
                if etype == "run-start":
                    handler_id = event["data"]["runner_data"]["stt_binary_handler_id"]
                elif etype == "stt-start":
                    break
                elif etype == "error":
                    raise RuntimeError(f"stt error: {event.get('data')}")
            if handler_id is None:
                raise RuntimeError("stt_binary_handler_id missing")

            prefix = bytes([handler_id])
            for offset in range(0, len(pcm), 4096):
                await ws.send_bytes(prefix + pcm[offset : offset + 4096])
            await ws.send_bytes(prefix)

            deadline = time.monotonic() + 25
            while time.monotonic() < deadline:
                try:
                    msg = await ws.receive_json(timeout=8)
                except asyncio.TimeoutError:
                    break
                if msg.get("type") == "result" and not msg.get("success"):
                    raise RuntimeError(f"pipeline result error: {msg.get('error')}")
                if msg.get("type") != "event":
                    continue
                event = msg["event"]
                etype = event.get("type")
                if etype == "stt-end":
                    output = event.get("data", {}).get("stt_output", {})
                    return (output.get("text") or "").strip()
                if etype == "error":
                    data = event.get("data", {})
                    if data.get("code") == "stt-no-text-recognized":
                        return ""
                    raise RuntimeError(f"stt error: {data}")
            return ""
        finally:
            await ws.close()

    async def transcribe_wyoming(self, pcm: bytes) -> str:
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(WYOMING_STT_HOST, WYOMING_STT_PORT), timeout=10
            )
        except (OSError, asyncio.TimeoutError) as exc:
            raise RuntimeError(
                f"cannot connect to Wyoming STT at {WYOMING_STT_HOST}:{WYOMING_STT_PORT}: {exc}"
            ) from exc
        try:
            audio_format = {"rate": 16000, "width": 2, "channels": 1}
            writer.write(wyoming_dump({"type": "audio-start", "data": audio_format}))
            await writer.drain()
            for offset in range(0, len(pcm), 4096):
                chunk = pcm[offset : offset + 4096]
                writer.write(
                    wyoming_dump(
                        {
                            "type": "audio-chunk",
                            "data": {**audio_format, "payload_length": len(chunk)},
                        },
                        chunk,
                    )
                )
            writer.write(wyoming_dump({"type": "audio-stop"}))
            await writer.drain()

            final = ""
            chunks = []
            while True:
                event = await asyncio.wait_for(
                    read_wyoming_event(reader), timeout=WYOMING_STT_TIMEOUT
                )
                if event is None:
                    break
                etype = event.get("type")
                data = event.get("data") or {}
                if etype == "transcript":
                    final = (data.get("text") or "").strip()
                elif etype == "transcript-chunk":
                    chunks.append(data.get("text") or "")
                elif etype == "transcript-stop":
                    break
                elif etype == "error":
                    raise RuntimeError(f"Wyoming STT error: {data.get('text') or data}")
            return (final or "".join(chunks)).strip()
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except OSError:
                pass

    async def transcribe_openai(self, pcm: bytes) -> str:
        form = FormData()
        form.add_field(
            "file", pcm_to_wav(pcm), filename="audio.wav", content_type="audio/wav"
        )
        form.add_field("model", OPENAI_STT_MODEL)
        form.add_field("response_format", "json")
        language = STT_LANGUAGE.split("-")[0].lower()
        if language:
            form.add_field("language", language)
        headers = {}
        if self.openai_key:
            headers["Authorization"] = f"Bearer {self.openai_key}"
        async with self.session.post(
            OPENAI_STT_URL, data=form, headers=headers, timeout=60
        ) as resp:
            body = await resp.text()
            if resp.status != 200:
                raise RuntimeError(f"STT HTTP {resp.status}: {body[:200]}")
        try:
            data = json.loads(body)
        except json.JSONDecodeError:
            return body.strip()
        if isinstance(data, dict):
            return (data.get("text") or "").strip()
        return ""

    async def transcribe_azure(self, pcm: bytes) -> str:
        url = AZURE_SPEECH_ENDPOINT or (
            f"https://{AZURE_SPEECH_REGION}.stt.speech.microsoft.com"
            "/speech/recognition/conversation/cognitiveservices/v1"
        )
        headers = {
            "Ocp-Apim-Subscription-Key": self.azure_key,
            "Content-Type": "audio/wav; codecs=audio/pcm; samplerate=16000",
            "Accept": "application/json",
        }
        async with self.session.post(
            url,
            params={"language": STT_LANGUAGE, "format": "simple"},
            data=pcm_to_wav(pcm),
            headers=headers,
            timeout=30,
        ) as resp:
            body = await resp.text()
            if resp.status != 200:
                raise RuntimeError(f"Azure STT HTTP {resp.status}: {body[:200]}")
        data = json.loads(body)
        status = data.get("RecognitionStatus")
        if status == "Success":
            return (data.get("DisplayText") or "").strip()
        if status in ("NoMatch", "InitialSilenceTimeout", "BabbleTimeout"):
            return ""
        raise RuntimeError(f"Azure STT {status}: {body[:200]}")

    async def ma_call(self, command: str, args: dict):
        payload = {
            "message_id": str(int(time.time() * 1000)),
            "command": command,
            "args": args,
        }
        headers = {
            "Authorization": f"Bearer {self.ma_token}",
            "Content-Type": "application/json",
        }
        async with self.session.post(
            MA_URL + "/api", json=payload, headers=headers, timeout=15
        ) as resp:
            text = await resp.text()
            if resp.status != 200:
                raise RuntimeError(f"MA {command} HTTP {resp.status}: {text[:200]}")
            try:
                return json.loads(text)
            except json.JSONDecodeError as exc:
                raise RuntimeError(f"MA {command} bad JSON: {exc}") from exc

    async def parse_intent(self, text: str):
        payload = {
            "model": DEEPSEEK_MODEL,
            "messages": [
                {"role": "system", "content": INTENT_PROMPT},
                {"role": "user", "content": text},
            ],
            "temperature": 0,
            "max_tokens": 120,
            "response_format": {"type": "json_object"},
        }
        headers = {
            "Authorization": f"Bearer {self.deepseek_key}",
            "Content-Type": "application/json",
        }
        async with self.session.post(
            DEEPSEEK_URL, json=payload, headers=headers, timeout=30
        ) as resp:
            body = await resp.text()
            if resp.status != 200:
                raise RuntimeError(f"DeepSeek HTTP {resp.status}: {body[:200]}")
            data = json.loads(body)
        content = data["choices"][0]["message"]["content"]
        intent = json.loads(content)
        if intent.get("intent") not in ("play_media", "play_random", "unsupported"):
            raise RuntimeError(f"unsupported intent payload: {intent}")
        if intent.get("intent") == "play_media":
            if intent.get("media_type") not in ("artist", "playlist"):
                raise RuntimeError(f"bad media_type: {intent.get('media_type')}")
            raw_queries = intent.get("queries")
            if not raw_queries:
                raw_queries = [intent.get("query", "")]
            queries = []
            for item in raw_queries:
                value = str(item).strip()
                if value and value not in queries:
                    queries.append(value)
            if not queries:
                raise RuntimeError("empty query")
            intent["queries"] = queries[:3]
        return intent

    async def resolve_target(self, player_id: str | None, queue_id: str | None):
        pid = (player_id or DEFAULT_PLAYER_ID).strip()
        if not pid:
            players = await self.ma_call(
                "players/all", {"return_unavailable": False}
            )
            usable = [
                item
                for item in (players or [])
                if item.get("available")
                and item.get("enabled", True)
                and not item.get("private")
                and not item.get("hide_in_ui")
            ]
            best = next(
                (item for item in usable if item.get("playback_state") == "playing"),
                None,
            )
            if best is None:
                best = next(
                    (
                        item
                        for item in usable
                        if item.get("playback_state") == "paused"
                        and item.get("current_media")
                    ),
                    None,
                )
            if best is None and usable:
                best = usable[0]
            if best is None:
                raise RuntimeError("no usable MA player found")
            pid = best["player_id"]
            log.info("auto-selected player %s (%s)", best.get("name"), pid)
        if queue_id:
            return pid, queue_id.strip()
        try:
            active = await self.ma_call(
                "player_queues/get_active_queue", {"player_id": pid}
            )
            if isinstance(active, dict) and active.get("queue_id"):
                return pid, active["queue_id"]
        except Exception as exc:  # noqa: BLE001
            log.warning("active queue lookup failed for %s: %s", pid, exc)
        return pid, pid

    async def run_command(
        self,
        text: str,
        player_id: str | None = None,
        queue_id: str | None = None,
        dry_run: bool = False,
    ) -> dict:
        text = (text or "").strip()
        if not text:
            return {"ok": False, "error": "empty text"}

        intent = await self.parse_intent(text)
        if intent.get("intent") == "unsupported":
            return {
                "ok": False,
                "transcript": text,
                "intent": "unsupported",
                "error": "V1 only supports play artist/playlist",
            }

        if intent.get("intent") == "play_random":
            pid, qid = await self.resolve_target(player_id, queue_id)
            played = await self.ma_call(
                "music/recently_played_items", {"limit": 100}
            )
            valid = [
                item
                for item in (played or [])
                if item.get("media_type") == "track" and item.get("uri")
            ]
            if not valid:
                tracks = await self.ma_call(
                    "music/recently_added_tracks", {"limit": 300}
                )
                valid = [
                    item
                    for item in (tracks or [])
                    if item.get("uri")
                    and 60 <= int(item.get("duration") or 0) <= 600
                ]
            if not valid:
                return {
                    "ok": False,
                    "transcript": text,
                    "intent": intent,
                    "error": "no tracks in library",
                }
            selected = random.choice(valid)
            response = {
                "ok": True,
                "transcript": text,
                "intent": intent,
                "player_id": pid,
                "queue_id": qid,
                "media_type": "track",
                "queries": [],
                "query": "",
                "dry_run": dry_run,
                "candidates": [],
                "selected": {
                    "name": selected.get("name"),
                    "uri": selected.get("uri"),
                },
            }
            if dry_run:
                response["played"] = False
                return response
            await self.ma_call(
                "player_queues/play_media",
                {
                    "queue_id": qid,
                    "media": selected["uri"],
                    "option": "play",
                },
            )
            response["played"] = True
            response["display"] = f"正在播放 {selected.get('name')}"
            return response

        media_type = intent["media_type"]
        queries = intent["queries"]
        pid, qid = await self.resolve_target(player_id, queue_id)
        key = "artists" if media_type == "artist" else "playlists"

        def norm(value: str) -> str:
            return "".join(value.casefold().split())

        searches = []
        all_items = []
        seen_uris = set()
        for query in queries:
            result = await self.ma_call(
                "music/search",
                {
                    "search_query": query,
                    "media_types": [media_type],
                    "providers": ["library"],
                    "limit": 8,
                },
            )
            items = [item for item in (result.get(key) or []) if item.get("uri")]
            searches.append((query, items))
            for item in items:
                uri = item.get("uri")
                if uri and uri not in seen_uris:
                    seen_uris.add(uri)
                    all_items.append(item)

        selected = None
        for query, items in searches:
            selected = next(
                (i for i in items if norm(i.get("name", "")) == norm(query)), None
            )
            if selected:
                break
        if selected is None:
            for query, items in searches:
                selected = next(
                    (
                        i
                        for i in items
                        if norm(query) in norm(i.get("name", ""))
                        or norm(i.get("name", "")) in norm(query)
                    ),
                    None,
                )
                if selected:
                    break
        if selected is None and searches and searches[0][1]:
            selected = searches[0][1][0]

        response = {
            "ok": True,
            "transcript": text,
            "intent": intent,
            "player_id": pid,
            "queue_id": qid,
            "media_type": media_type,
            "queries": queries,
            "query": queries[0],
            "dry_run": dry_run,
            "candidates": [
                {"name": i.get("name"), "uri": i.get("uri")} for i in all_items[:5]
            ],
            "selected": (
                {"name": selected.get("name"), "uri": selected.get("uri")}
                if selected
                else None
            ),
        }
        if selected is None:
            response["ok"] = False
            response["error"] = f"no {media_type} matched {queries}"
            return response
        if dry_run:
            response["played"] = False
            return response

        await self.ma_call(
            "player_queues/play_media",
            {
                "queue_id": qid,
                "media": selected["uri"],
                "option": "play",
            },
        )
        response["played"] = True
        response["display"] = f"正在播放 {selected.get('name')}"
        return response


bridge: Bridge | None = None


def require_token(request: web.Request):
    if BRIDGE_TOKEN and request.headers.get("X-Bridge-Token") != BRIDGE_TOKEN:
        raise web.HTTPUnauthorized(text="bad bridge token")


async def health(request: web.Request):
    pipeline = (
        {"id": bridge.pipeline_id, "name": bridge.pipeline_name}
        if bridge.pipeline_id
        else None
    )
    return web.json_response(
        {
            "ok": True,
            "ha": HA_URL or None,
            "stt": {"provider": STT_PROVIDER, "language": STT_LANGUAGE},
            "pipeline": pipeline,
            "ma": MA_URL,
            "player": DEFAULT_PLAYER_ID or "auto",
        }
    )


async def text_handler(request: web.Request):
    require_token(request)
    try:
        payload = await request.json()
    except json.JSONDecodeError:
        raise web.HTTPBadRequest(text="invalid JSON")
    text = payload.get("text", "")
    dry_run = bool(payload.get("dry_run", False))
    try:
        result = await bridge.run_command(
            text,
            payload.get("player_id"),
            payload.get("queue_id"),
            dry_run,
        )
        return web.json_response(result)
    except Exception as exc:  # noqa: BLE001
        log.exception("text command failed")
        return web.json_response({"ok": False, "error": str(exc)}, status=500)


async def stt_handler(request: web.Request):
    require_token(request)
    pcm = await request.read()
    if not pcm or len(pcm) > MAX_PCM_BYTES:
        raise web.HTTPBadRequest(text="invalid PCM length")
    try:
        text = await bridge.transcribe(pcm)
        return web.json_response({"ok": True, "text": text})
    except Exception as exc:  # noqa: BLE001
        log.exception("stt failed")
        return web.json_response({"ok": False, "error": str(exc)}, status=500)


async def voice_handler(request: web.Request):
    require_token(request)
    pcm = await request.read()
    if not pcm or len(pcm) > MAX_PCM_BYTES:
        raise web.HTTPBadRequest(text="invalid PCM length")
    player_id = request.query.get("player_id")
    queue_id = request.query.get("queue_id")
    dry_run = request.query.get("dry_run", "0") in ("1", "true", "yes")
    try:
        if DEBUG_AUDIO:
            samples = array("h")
            samples.frombytes(pcm[: len(pcm) // 2 * 2])
            peak = max((abs(s) for s in samples), default=0)
            rms = (
                int((sum(s * s for s in samples) / len(samples)) ** 0.5)
                if samples
                else 0
            )
            log.info("voice pcm %d bytes peak=%d rms=%d", len(pcm), peak, rms)
            try:
                Path("/tmp/last_voice.pcm").write_bytes(pcm)
                with wave.open("/tmp/last_voice.wav", "wb") as wav:
                    wav.setnchannels(1)
                    wav.setsampwidth(2)
                    wav.setframerate(16000)
                    wav.writeframes(pcm)
            except OSError as exc:
                log.warning("save voice debug audio failed: %s", exc)
        transcript = await bridge.transcribe(pcm)
        log.debug("voice transcript=%r", transcript)
        if not transcript:
            return web.json_response(
                {"ok": False, "error": "no speech recognized", "transcript": ""}
            )
        result = await bridge.run_command(transcript, player_id, queue_id, dry_run)
        log.debug("voice result=%s", json.dumps(result, ensure_ascii=False)[:300])
        return web.json_response(result)
    except Exception as exc:  # noqa: BLE001
        log.exception("voice command failed")
        return web.json_response({"ok": False, "error": str(exc)}, status=500)


async def on_startup(app: web.Application):
    global bridge
    bridge = Bridge()
    await bridge.start()


async def on_cleanup(app: web.Application):
    await bridge.stop()


def main():
    app = web.Application(client_max_size=MAX_PCM_BYTES + 1024)
    app.router.add_get("/health", health)
    app.router.add_post("/text", text_handler)
    app.router.add_post("/stt", stt_handler)
    app.router.add_post("/voice", voice_handler)
    app.on_startup.append(on_startup)
    app.on_cleanup.append(on_cleanup)
    log.info("bridge listening on 0.0.0.0:%d", PORT)
    web.run_app(app, host="0.0.0.0", port=PORT, print=None)


if __name__ == "__main__":
    main()
