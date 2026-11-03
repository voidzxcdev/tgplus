# TGPlus

TGPlus is a personal fork of [Telegram Desktop](https://github.com/telegramdesktop/tdesktop).
It adds three features on top of the official client. Each one lives on its
own branch and pull request:

| Feature | Branch |
| --- | --- |
| Keep deleted messages | `tgplus/keep-deleted-messages` |
| Most important person | `tgplus/vip-contact` |
| Customization settings | `tgplus/customization` |

## Keep deleted messages

When someone deletes a message, Telegram tells every client to remove it.
TGPlus keeps that message on screen instead. It stays in the chat dimmed, with
a "deleted" mark next to the time, and its original text and media are kept.

- Turn it on or off in **Settings → Chat Settings → Keep messages deleted by
  others**. It is on by default.
- It only covers messages deleted by the server: by the other person, by
  admins, or from your other devices. Messages you delete in this app are
  removed as usual.
- Kept messages live in memory only. Once the app restarts or the chat
  history is reloaded from the server, they are gone.
- **A message your client never received can't be recovered.** If a message
  was sent and deleted while TGPlus was closed, or before the chat was opened
  and loaded, the server never gives it to you, and no client can show it.

## Most important person

Pick one person per account as your most important person:

- Right-click a private chat in the chat list and choose **Make most important
  person**. Choose **Remove from most important** to undo it, or right-click
  the highlighted row.
- That chat gets its own highlighted row at the very top of the chat list,
  above pinned chats. The row has an accent background, a star and an unread
  counter. It shows in every folder and in the archive, and it hides while you
  search.
- Their messages show a notification even if you muted that chat in the app.
  Your system's Do Not Disturb mode and the app's global notification switches
  still apply.
- Only one person can be chosen at a time, and only private chats with real
  users (not bots or Saved Messages). The choice is stored locally for that
  account and is not synced to the server.

## Customization settings

**Settings → Customization** gathers the cosmetic options in one place:

- **Theme and accent color** opens the existing theme and accent color picker.
- **Interface scale** is the same scale control as in the main settings.
- **Hide stories bar** hides the stories row above the chat list. It applies
  right away.
- **Show a mute icon next to muted chat names**
- **Smaller bubble corners**
- **Use the full width for messages**
- **Hide the AI button in the message field**
- **Show user and chat IDs in profiles** and **Show when you joined a
  channel**

Most of these switch on hidden options that already exist in Telegram Desktop.
The ones that only apply on startup ask you to restart.

Chat list density is not included. The row height is a compile-time style
value used for layout and hit-testing all over the chat list, so it can't be
changed at runtime without a large rewrite.

New TGPlus text is in English only. The official translations don't include
these strings, so other languages show the English text.

## Building your own copy

Follow the official [build instructions](docs/) for your platform.

A custom build needs its **own** `api_id` and `api_hash`. Get them at
[my.telegram.org](https://my.telegram.org) under *API development tools*, and
pass them to CMake:

```
-D TDESKTOP_API_ID=YOUR_API_ID -D TDESKTOP_API_HASH=YOUR_API_HASH
```

Never commit your `api_id` or `api_hash` to this repository or anywhere
public. Keep them in your local build configuration only.
