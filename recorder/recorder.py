"""TGPlus recorder.

Runs next to your account on a small server (Render free plan works) and
keeps deleted messages, including the ones deleted while TGPlus is closed.

It caches recent messages and, when one is deleted or edited, writes:
  * a readable copy to the private "TGPlus Vault" channel, for you;
  * a machine copy to the private "TGPlus Sync" channel, which TGPlus
    imports and shows in the original chat.

Configuration is read from the environment, see README_TGPLUS.md.
"""

import asyncio
import collections
import json
import logging
import os
import secrets
import signal
import time
from datetime import datetime, timezone
from pathlib import Path

from aiohttp import ClientSession, ClientTimeout, web
from telethon import TelegramClient, events, functions, types, utils
from telethon.errors import FloodWaitError, RPCError
from telethon.sessions import StringSession

log = logging.getLogger("tgplus")

VAULT_TITLE = "TGPlus Vault"
SYNC_TITLE = "TGPlus Sync"
CHUNK_PREFIX = "TGPLUS1"
MEDIA_PREFIX = "TGPLUS1M"
# Telegram limits a message to 4096 UTF-16 units, so 1800 code points
# leave room for the header even when every character needs two units.
CHUNK_SIZE = 1800
CAPTION_LIMIT = 1000
TEXT_LIMIT = 3900
MB = 1024 * 1024


def env_int(name, fallback):
    return int(os.environ.get(name, fallback))


def env_float(name, fallback):
    return float(os.environ.get(name, fallback))


def env_flag(name, fallback):
    return os.environ.get(name, "1" if fallback else "0").strip() == "1"


API_ID = env_int("API_ID", "0")
API_HASH = os.environ.get("API_HASH", "")
SESSION = os.environ.get("SESSION", "")
PORT = env_int("PORT", "10000")
EXTERNAL_URL = os.environ.get("RENDER_EXTERNAL_URL", "").rstrip("/")
STARTUP_DELAY = env_float("STARTUP_DELAY", "40")
PING_PERIOD = env_float("PING_PERIOD", "600")
CACHE_MAX = env_int("CACHE_MAX", "40000")
WARMUP_DIALOGS = env_int("WARMUP_DIALOGS", "80")
WARMUP_MESSAGES = env_int("WARMUP_MESSAGES", "40")
MEDIA_SCOPE = os.environ.get("MEDIA_SCOPE", "private").strip().lower()
MEDIA_MAX_MB = env_float("MEDIA_MAX_MB", "20")
MEDIA_CACHE_MB = env_float("MEDIA_CACHE_MB", "350")
MEDIA_DIR = Path(os.environ.get("MEDIA_DIR", "/tmp/tgplus-media"))
RECORD_CHANNELS = env_flag("RECORD_CHANNELS", False)
RECORD_OUTGOING = env_flag("RECORD_OUTGOING", False)


def peer_ref(peer):
    if isinstance(peer, types.PeerUser):
        return ["u", peer.user_id]
    if isinstance(peer, types.PeerChat):
        return ["c", peer.chat_id]
    if isinstance(peer, types.PeerChannel):
        return ["ch", peer.channel_id]
    return None


def cache_key(peer, msg_id):
    # Private chats and basic groups share one message id sequence.
    if peer[0] == "ch":
        return ("ch", peer[1], msg_id)
    return ("n", msg_id)


def entity_json(entity):
    name = type(entity).__name__
    if not name.startswith("MessageEntity"):
        return None
    result = {
        "_": name[len("MessageEntity"):].lower(),
        "o": entity.offset,
        "l": entity.length,
    }
    if isinstance(entity, types.MessageEntityPre):
        result["lang"] = entity.language or ""
    elif isinstance(entity, types.MessageEntityTextUrl):
        result["url"] = entity.url
    elif isinstance(entity, types.MessageEntityMentionName):
        result["uid"] = str(entity.user_id)
    elif isinstance(entity, types.MessageEntityCustomEmoji):
        result["doc"] = str(entity.document_id)
    elif isinstance(entity, types.MessageEntityBlockquote):
        result["c"] = bool(getattr(entity, "collapsed", False))
    return result


def is_view_once(message):
    return bool(getattr(message.media, "ttl_seconds", None))


def media_label(message):
    media = message.media
    if media is None or isinstance(media, types.MessageMediaWebPage):
        return ""
    if is_view_once(message):
        return "View-once media (not saved)"
    if message.photo:
        return "Photo"
    if message.sticker:
        return "Sticker"
    if message.voice:
        return "Voice message"
    if message.video_note:
        return "Video message"
    if message.gif:
        return "GIF"
    if message.video:
        return "Video"
    if message.audio:
        return "Audio"
    if message.document:
        name = message.file.name if message.file else None
        return f"File: {name}" if name else "File"
    if isinstance(media, (types.MessageMediaGeo, types.MessageMediaGeoLive,
                          types.MessageMediaVenue)):
        return "Location"
    if isinstance(media, types.MessageMediaContact):
        return "Contact"
    if isinstance(media, types.MessageMediaPoll):
        question = media.poll.question
        return "Poll: " + getattr(question, "text", question or "")
    if isinstance(media, types.MessageMediaDice):
        return "Dice"
    return "Media"


def display_name(entity):
    return utils.get_display_name(entity) if entity else ""


def format_time(stamp):
    moment = datetime.fromtimestamp(stamp, tz=timezone.utc)
    return moment.strftime("%Y-%m-%d %H:%M UTC")


def split_payload(payload):
    parts = []
    rest = payload
    while rest:
        cut = min(len(rest), CHUNK_SIZE)
        # Telegram trims whitespace at the end of a message.
        while 0 < cut < len(rest) and (rest[cut - 1].isspace()
                                       or rest[cut].isspace()):
            cut -= 1
        if cut == 0:
            cut = min(len(rest), CHUNK_SIZE)
        parts.append(rest[:cut])
        rest = rest[cut:]
    return parts


def remove_file(path):
    try:
        os.remove(path)
        os.rmdir(os.path.dirname(path))
    except OSError:
        pass


def send_options(message):
    document = message.document
    return {
        "attributes": list(document.attributes) if document else None,
        "voice_note": bool(message.voice),
        "video_note": bool(message.video_note),
        "supports_streaming": bool(message.video),
    }


class MediaCache:
    """Files downloaded ahead of time, because deleted media can't be fetched."""

    def __init__(self, limit_bytes):
        self.limit = limit_bytes
        self.files = collections.OrderedDict()
        self.size = 0
        MEDIA_DIR.mkdir(parents=True, exist_ok=True)

    def add(self, key, path, options):
        size = os.path.getsize(path)
        self.drop(key)
        self.files[key] = (path, size, options)
        self.size += size
        while self.size > self.limit and self.files:
            oldest = next(iter(self.files))
            self.drop(oldest)

    def take(self, key):
        entry = self.files.pop(key, None)
        if entry:
            self.size -= entry[1]
            return entry[0], entry[2]
        return None

    def drop(self, key):
        entry = self.take(key)
        if entry:
            remove_file(entry[0])


class MessageCache:
    def __init__(self, limit, media):
        self.limit = limit
        self.media = media
        self.records = collections.OrderedDict()

    def __len__(self):
        return len(self.records)

    def put(self, key, record):
        self.records[key] = record
        self.records.move_to_end(key)
        while len(self.records) > self.limit:
            oldest, _ = self.records.popitem(last=False)
            self.media.drop(oldest)

    def get(self, key):
        return self.records.get(key)

    def pop(self, key):
        return self.records.pop(key, None)


class Recorder:
    def __init__(self):
        self.client = TelegramClient(
            StringSession(SESSION),
            API_ID,
            API_HASH,
            device_model="TGPlus Recorder",
            system_version="Server",
            app_version="1.0",
        )
        self.media = MediaCache(int(MEDIA_CACHE_MB * MB))
        self.cache = MessageCache(CACHE_MAX, self.media)
        self.queue = asyncio.Queue()
        self.downloads = asyncio.Semaphore(2)
        self.tasks = set()
        self.me = None
        self.vault = None
        self.sync = None
        self.own_channels = set()
        self.started = time.time()
        self.stats = {"deleted": 0, "edited": 0, "media": 0}

    # Telegram side.

    async def run(self, stop):
        await self.client.connect()
        if not await self.client.is_user_authorized():
            raise SystemExit("SESSION is not authorized, run login.py again.")
        self.me = await self.client.get_me()
        await self.prepare_dialogs()
        self.client.add_event_handler(self.on_new, events.NewMessage())
        self.client.add_event_handler(self.on_edit, events.MessageEdited())
        self.client.add_event_handler(self.on_delete, events.MessageDeleted())
        worker = asyncio.create_task(self.work())
        await self.client.catch_up()
        log.info("Recording, %d messages cached.", len(self.cache))
        disconnected = asyncio.create_task(self.client.run_until_disconnected())
        stopped = asyncio.create_task(stop.wait())
        await asyncio.wait(
            {disconnected, stopped},
            return_when=asyncio.FIRST_COMPLETED)
        worker.cancel()
        await self.client.disconnect()

    async def prepare_dialogs(self):
        warmed = 0
        async for dialog in self.client.iter_dialogs():
            entity = dialog.entity
            if (isinstance(entity, types.Channel)
                    and entity.broadcast
                    and entity.creator
                    and dialog.name in (VAULT_TITLE, SYNC_TITLE)):
                if dialog.name == VAULT_TITLE and not self.vault:
                    self.vault = entity
                elif dialog.name == SYNC_TITLE and not self.sync:
                    self.sync = entity
                continue
            if warmed >= WARMUP_DIALOGS:
                continue
            if isinstance(entity, types.Channel) and entity.broadcast \
                    and not RECORD_CHANNELS:
                continue
            warmed += 1
            try:
                async for message in self.client.iter_messages(
                        entity, limit=WARMUP_MESSAGES):
                    self.remember(message, dialog.name)
            except RPCError as error:
                log.warning("Warm-up skipped a chat: %s", type(error).__name__)
            await asyncio.sleep(0.3)
        if not self.vault:
            self.vault = await self.create_channel(
                VAULT_TITLE,
                "Messages deleted or edited in your chats, kept by TGPlus.")
        if not self.sync:
            self.sync = await self.create_channel(
                SYNC_TITLE,
                "Data for the TGPlus app. Don't post here.")
        self.own_channels = {self.vault.id, self.sync.id}

    async def create_channel(self, title, about):
        result = await self.client(functions.channels.CreateChannelRequest(
            title=title,
            about=about,
            broadcast=True))
        channel = result.chats[0]
        await self.client.edit_folder(channel, 1)
        await self.client(functions.account.UpdateNotifySettingsRequest(
            peer=types.InputNotifyPeer(utils.get_input_peer(channel)),
            settings=types.InputPeerNotifySettings(mute_until=2**31 - 1)))
        log.info("Created the %s channel.", title)
        return channel

    # Cache.

    def wanted(self, message):
        if not isinstance(message, types.Message):
            return False
        peer = message.peer_id
        if isinstance(peer, types.PeerChannel) \
                and peer.channel_id in self.own_channels:
            return False
        if message.post and not RECORD_CHANNELS:
            return False
        if message.out and not RECORD_OUTGOING:
            return False
        return True

    def make_record(self, message, chat_title):
        peer = peer_ref(message.peer_id)
        if message.from_id is not None:
            sender = peer_ref(message.from_id)
        elif message.out:
            sender = ["u", self.me.id]
        else:
            sender = peer
        reply_to = 0
        topic_root = 0
        header = message.reply_to
        if isinstance(header, types.MessageReplyHeader):
            reply_to = header.reply_to_msg_id or 0
            if header.forum_topic:
                topic_root = header.reply_to_top_id or reply_to
        sender_name = display_name(message.sender)
        if not sender_name and message.out:
            sender_name = display_name(self.me)
        return {
            "p": peer,
            "id": message.id,
            "date": int(message.date.timestamp()),
            "from": sender,
            "fn": sender_name,
            "pa": message.post_author or "",
            "rt": reply_to,
            "tr": topic_root,
            "text": message.message or "",
            "ent": [e for e in map(entity_json, message.entities or []) if e],
            "ml": media_label(message),
            "out": bool(message.out),
            "post": bool(message.post),
            "chat": chat_title or display_name(message.chat),
        }

    def remember(self, message, chat_title=""):
        if not self.wanted(message):
            return None
        record = self.make_record(message, chat_title)
        if not record["p"]:
            return None
        key = cache_key(record["p"], message.id)
        self.cache.put(key, record)
        return key

    def wants_media(self, message):
        if not message.media or is_view_once(message):
            return False
        if not (message.photo or message.document):
            return False
        if MEDIA_SCOPE == "none":
            return False
        if MEDIA_SCOPE != "all":
            if not (message.is_private
                    or (MEDIA_SCOPE == "groups" and message.is_group)):
                return False
        size = message.file.size if message.file else None
        return bool(size) and size <= MEDIA_MAX_MB * MB

    async def prefetch(self, message, key):
        async with self.downloads:
            if not self.cache.get(key):
                return
            # A folder per message keeps the original file name.
            folder = MEDIA_DIR / "_".join(str(part) for part in key)
            folder.mkdir(parents=True, exist_ok=True)
            try:
                path = await self.client.download_media(
                    message,
                    file=str(folder))
            except (RPCError, OSError) as error:
                log.warning("Media prefetch failed: %s", type(error).__name__)
                path = None
            if path and self.cache.get(key):
                self.media.add(key, path, send_options(message))
            elif path:
                remove_file(path)
            else:
                try:
                    folder.rmdir()
                except OSError:
                    pass

    def spawn(self, coroutine):
        task = asyncio.create_task(coroutine)
        self.tasks.add(task)
        task.add_done_callback(self.tasks.discard)

    async def resolve(self, message):
        try:
            await message.get_sender()
            await message.get_chat()
        except (RPCError, ValueError):
            pass

    # Events.

    async def on_new(self, event):
        message = event.message
        if not self.wanted(message):
            return
        await self.resolve(message)
        key = self.remember(message)
        if key and self.wants_media(message):
            self.spawn(self.prefetch(message, key))

    async def on_edit(self, event):
        message = event.message
        if not self.wanted(message):
            return
        peer = peer_ref(message.peer_id)
        if not peer:
            return
        key = cache_key(peer, message.id)
        old = self.cache.get(key)
        if old and old["text"] != (message.message or ""):
            edited = message.edit_date or datetime.now(timezone.utc)
            self.queue.put_nowait(lambda: self.report_edit(
                dict(old),
                int(edited.timestamp())))
        await self.resolve(message)
        record = self.make_record(message, old["chat"] if old else "")
        if old and not record["fn"]:
            record["fn"] = old["fn"]
        self.cache.put(key, record)

    async def on_delete(self, event):
        update = event.original_update
        if isinstance(update, types.UpdateDeleteChannelMessages):
            if update.channel_id in self.own_channels:
                return
            keys = [("ch", update.channel_id, i) for i in update.messages]
        else:
            keys = [("n", i) for i in event.deleted_ids]
        now = int(time.time())
        for key in keys:
            record = self.cache.pop(key)
            if record:
                media = self.media.take(key)
                self.queue.put_nowait(
                    lambda r=record, m=media: self.report_deleted(r, m, now))

    # Output.

    async def work(self):
        while True:
            job = await self.queue.get()
            try:
                await job()
            except Exception:
                log.exception("Could not write a record.")
            await asyncio.sleep(0.5)

    async def call(self, make):
        for _ in range(5):
            try:
                return await make()
            except FloodWaitError as error:
                await asyncio.sleep(error.seconds + 1)
        return None

    async def send_text(self, channel, text):
        return await self.call(lambda: self.client.send_message(
            channel,
            text,
            parse_mode=None,
            link_preview=False))

    async def send_payload(self, payload):
        key = secrets.token_hex(6)
        data = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
        parts = split_payload(data)
        for index, part in enumerate(parts, 1):
            header = f"{CHUNK_PREFIX} {key} {index}/{len(parts)}\n"
            await self.send_text(self.sync, header + part)

    def vault_text(self, record, title):
        lines = [title]
        if record["chat"]:
            lines.append(f"Chat: {record['chat']}")
        if record["fn"]:
            lines.append(f"From: {record['fn']}")
        lines.append(f"Sent: {format_time(record['date'])}")
        if record["ml"]:
            lines.append(record["ml"])
        return "\n".join(lines)

    async def store_media(self, path, options, caption):
        try:
            return await self.call(lambda: self.client.send_file(
                self.vault,
                path,
                caption=caption,
                parse_mode=None,
                **options))
        except (RPCError, ValueError, OSError) as error:
            log.warning("Media upload failed: %s", type(error).__name__)
            return None

    async def report_deleted(self, record, media, deleted_at):
        head = self.vault_text(record, "Deleted message")
        text = record["text"]
        full = head + ("\n\n" + text if text else "")
        media_key = None
        stored = None
        try:
            if media and os.path.exists(media[0]):
                caption = full if len(full) <= CAPTION_LIMIT else head
                stored = await self.store_media(media[0], media[1], caption)
                if stored and caption == head and text:
                    await self.send_text(self.vault, text[:TEXT_LIMIT])
        finally:
            if media:
                remove_file(media[0])
        if not stored:
            await self.send_text(self.vault, full[:TEXT_LIMIT])
        elif stored.media:
            media_key = secrets.token_hex(6)
            # WHY: the desktop matches records to media by this key, so the
            # media has to be in the sync channel before the record is.
            sent = await self.call(lambda: self.client.send_file(
                self.sync,
                stored.media,
                caption=f"{MEDIA_PREFIX} {media_key}",
                parse_mode=None))
            if sent:
                self.stats["media"] += 1
            else:
                media_key = None
        payload = {key: value for key, value in record.items()
                   if key != "chat"}
        payload["t"] = "del"
        payload["at"] = deleted_at
        if media_key:
            payload["mkey"] = media_key
        await self.send_payload(payload)
        self.stats["deleted"] += 1

    async def report_edit(self, old, edited_at):
        head = self.vault_text(old, "Edited message, earlier version")
        body = head + ("\n\n" + old["text"] if old["text"] else "")
        await self.send_text(self.vault, body[:TEXT_LIMIT])
        await self.send_payload({
            "t": "edit",
            "p": old["p"],
            "id": old["id"],
            "text": old["text"],
            "ent": old["ent"],
            "at": edited_at,
        })
        self.stats["edited"] += 1

    # Health.

    def health(self):
        return {
            "ok": True,
            "connected": self.client.is_connected(),
            "cached": len(self.cache),
            "media_mb": round(self.media.size / MB, 1),
            "deleted": self.stats["deleted"],
            "edited": self.stats["edited"],
            "media": self.stats["media"],
            "uptime": int(time.time() - self.started),
        }


async def keep_awake():
    # WHY: free Render services sleep after 15 minutes without inbound
    # traffic, and a sleeping recorder misses deletions.
    if not EXTERNAL_URL:
        return
    async with ClientSession(timeout=ClientTimeout(total=30)) as session:
        while True:
            await asyncio.sleep(PING_PERIOD)
            try:
                async with session.get(EXTERNAL_URL + "/health") as response:
                    await response.read()
            except Exception as error:
                log.warning("Self-ping failed: %s", type(error).__name__)


async def main():
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s")
    logging.getLogger("telethon").setLevel(logging.WARNING)
    if not (API_ID and API_HASH and SESSION):
        raise SystemExit("Set API_ID, API_HASH and SESSION.")

    recorder = Recorder()
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGTERM, signal.SIGINT):
        try:
            loop.add_signal_handler(sig, stop.set)
        except NotImplementedError:
            pass

    async def health(_request):
        return web.json_response(recorder.health())

    app = web.Application()
    app.router.add_get("/", health)
    app.router.add_get("/health", health)
    runner = web.AppRunner(app)
    await runner.setup()
    await web.TCPSite(runner, "0.0.0.0", PORT).start()
    pinger = asyncio.create_task(keep_awake())

    # WHY: a deploy starts the new instance before stopping the old one.
    # Two connections with one session make Telegram revoke it, so wait
    # until the old instance had time to shut down.
    try:
        await asyncio.wait_for(stop.wait(), timeout=STARTUP_DELAY)
    except asyncio.TimeoutError:
        await recorder.run(stop)
    pinger.cancel()
    await runner.cleanup()


if __name__ == "__main__":
    asyncio.run(main())
