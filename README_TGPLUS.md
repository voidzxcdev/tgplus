# TGPlus

TGPlus is a personal fork of [Telegram Desktop](https://github.com/telegramdesktop/tdesktop)
with a black and white look and extras the official client doesn't have.
Everything new is under **Settings → TGPlus**, except where noted.

TGPlus is unofficial. It keeps its data in `%APPDATA%\TGPlus`, apart from
an official install, so you sign in once after installing it.

## Deleted and edited messages

- **Keep deleted messages.** When someone deletes a message, it stays in the
  chat with a "deleted" mark, including its text and media. Kept messages
  are saved encrypted with your local data, so they survive restarts and
  show up again when you scroll back to them.
- **Save media from deleted messages.** Photos and files from deleted
  messages that were already loaded are copied to
  `%APPDATA%\TGPlus\TGPlus deleted media`, so they stay even after Telegram
  removes them from its servers.
- **Deleted messages log.** **Settings → TGPlus → Deleted messages** lists
  every kept message from all chats, newest first, with a search field.
  Click one to jump to it. A chat's own log is in its **⋮** menu.
- **Edit history.** Right-click an edited message and choose **Edit
  history** to see every earlier version and when it was replaced.
- Right-click a kept message and choose **Remove from archive** to delete it
  for good. Messages you delete yourself are never kept.

The app can only keep messages it received before they were deleted. For
messages deleted while TGPlus is closed, set up the cloud recorder below.

## Cloud recorder

The recorder is a small service in [`recorder/`](recorder) that stays signed
in to your account on a server (the free Render plan is enough). It
remembers recent messages, and when one is deleted or edited it posts:

- a readable copy to a private **TGPlus Vault** channel, which you can read
  on any device;
- a data copy to a private **TGPlus Sync** channel. TGPlus imports it on
  start and every 10 minutes, and puts the message back in its chat marked
  "Captured by the recorder".

Both channels are created automatically, archived and muted.

The recorder only sees messages that arrive while it runs, plus the last 40
messages of your 80 most recent chats when it starts. View-once media is
never saved. Media up to 20 MB from private chats is downloaded in advance,
because deleted media can't be fetched later.

### Setting it up

1. Get your own `api_id` and `api_hash` at [my.telegram.org](https://my.telegram.org)
   under **API development tools**.
2. On your computer, with Python 3.10 or newer, create a session:

   ```
   cd recorder
   pip install -r requirements.txt
   python login.py
   ```

   It asks for the api pair, your phone number, the login code and your 2FA
   password, then prints a long **session string**. The session string is a
   full login to your account: never share it, commit it or paste it in a
   chat. You can end it any time in Telegram under **Settings → Devices**.
3. In [Render](https://dashboard.render.com), choose **New → Blueprint**,
   connect this repository, and pick the `tgplus/build` branch. Render reads
   [`render.yaml`](render.yaml) and asks for three values: `API_ID`,
   `API_HASH` and `SESSION`. Paste them into the dashboard.
4. After the deploy, open the service URL followed by `/health`. It shows
   `"connected": true` and how many messages are cached. No message content
   is ever shown there or written to the logs.
5. In TGPlus, **Settings → TGPlus → Import from the recorder** is on by
   default. **Sync now** imports right away.

### Notes

- Free Render services sleep after 15 minutes without visitors. The recorder
  visits its own `/health` page every 10 minutes to stay awake. One service
  running all month uses about 744 of the 750 free instance hours, so don't
  run other free services in the same workspace.
- Render's disk is wiped on every restart. Nothing important is kept there:
  records go to your channels right away, and the message cache refills.
- Optional settings, as environment variables on the service:

  | Variable | Default | Meaning |
  | --- | --- | --- |
  | `MEDIA_SCOPE` | `private` | Download media from `private` chats, `groups` too, `all` chats, or `none` |
  | `MEDIA_MAX_MB` | `20` | Largest file downloaded in advance |
  | `MEDIA_CACHE_MB` | `350` | Disk space for media downloaded in advance |
  | `RECORD_CHANNELS` | `0` | `1` also records posts in broadcast channels |
  | `RECORD_OUTGOING` | `0` | `1` also records your own messages |
  | `CACHE_MAX` | `40000` | Messages kept in memory |
  | `WARMUP_DIALOGS` | `80` | Chats read on start |
  | `WARMUP_MESSAGES` | `40` | Messages read per chat on start |

## Ghost mode

**Settings → TGPlus → Enable ghost mode**, then choose what to hide:

- **Don't send read receipts.** Chats are marked read only for you. Others
  keep seeing your messages as unread until you reply. Voice messages,
  video messages and animated emoji you open also stay unread for them.
- **Don't show me online.** TGPlus always reports you as offline.
- **Don't send typing status.** No "typing…", "recording…" or "choosing a
  sticker" for others.
- **View stories anonymously.** Stories you watch don't count as viewed.

Sending a message still marks the chat as read for the other side. That's
how Telegram works and no client can avoid it.

## Privacy

- **Streamer mode** hides your phone number in settings and profiles, and
  hides names and text in notifications.
- **Hide windows from screen capture** makes TGPlus windows black in
  screenshots, recordings and screen sharing.

## Messages

- **Show seconds in message time** shows the time of every message to the
  second, including edits.
- **Keyword alerts.** Add keywords under **Alert keywords**, one per line.
  A message that contains one notifies you even in a muted chat.
- **Copy message ID** in a message's context menu.
- **Remind me** in a message's context menu forwards the message to your
  Saved Messages at a time you choose, as a scheduled message.

## Most important person

Right-click a private chat in the chat list and choose **Make most important
person**. That chat gets its own highlighted row at the top of the chat list,
in every folder and in the archive, and its messages notify you even when
the chat is muted. Only one person per account; choose **Remove from most
important** to undo it.

## Customization

**Settings → Customization** gathers the cosmetic options: theme and accent
color, interface scale, hiding the stories bar, a mute icon next to muted
chats, smaller bubble corners, full-width messages, hiding the AI button,
and showing user and chat IDs and channel join dates in profiles.

New TGPlus text is in English only. Other languages show the English text.

## Building

Every push to `tgplus/build` runs the **TGPlus Windows installer** workflow
in GitHub Actions. Download **TGPlus installer x64** or **TGPlus portable
x64** from the run's artifacts. Auto-update and crash reports are off, so
the official updater never replaces TGPlus.

The first run builds all libraries and can take several hours. They are
cached, and later runs take about two hours. The workflow compiles the
changed files first, so a compile error fails within minutes.

Add your own api pair as repository secrets named `TGPLUS_API_ID` and
`TGPLUS_API_HASH` (**Settings → Secrets and variables → Actions**). Without
them, the build uses the public test pair from the official CI. Never commit
an `api_id`, `api_hash` or session string to the repository.

To build locally, follow the official [build instructions](docs/) and pass
`-D TDESKTOP_API_ID=... -D TDESKTOP_API_HASH=...` to `configure.bat`.

The black and white icons are generated by `Telegram/build/tgplus/make_icons.py`.
