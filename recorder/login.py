"""Creates the SESSION value for the TGPlus recorder.

Run it on your own computer:

    pip install -r requirements.txt
    python login.py

It asks for your api_id and api_hash (https://my.telegram.org), your phone
number, the login code and your 2FA password if you have one, then prints a
session string. Paste that string into the SESSION variable of the Render
service and nowhere else: anyone holding it can use your account.
"""

import asyncio
import getpass

from telethon import TelegramClient
from telethon.sessions import StringSession


async def main():
    api_id = int(input("api_id: ").strip())
    api_hash = input("api_hash: ").strip()
    client = TelegramClient(
        StringSession(),
        api_id,
        api_hash,
        device_model="TGPlus Recorder",
        system_version="Server",
        app_version="1.0",
    )
    await client.start(
        phone=lambda: input("Phone number (international format): "),
        password=lambda: getpass.getpass("2FA password: "),
    )
    me = await client.get_me()
    session = client.session.save()
    await client.disconnect()
    print()
    print(f"Logged in as {me.first_name} (id {me.id}).")
    print("SESSION value, keep it secret:")
    print()
    print(session)
    print()
    print("Paste it into the Render dashboard, then close this window.")
    print("You can revoke it any time in Telegram: Settings > Devices.")


if __name__ == "__main__":
    asyncio.run(main())
