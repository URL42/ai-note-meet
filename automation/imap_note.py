#!/usr/bin/env python3
"""
imap_note.py — Create an Apple Note by appending to iCloud IMAP.

Usage (called by n8n Execute Command node):
  python3 imap_note.py \
    --folder "notemeet.notes" \
    --title  "Work · Jun 22 14:30" \
    --body   "<html><body><b>Tag: Work</b><br><br>Pick up milk...</body></html>"

Credentials are read from environment variables — set these on the n8n server
(e.g. in /etc/environment or the n8n systemd unit's Environment= block):
  ICLOUD_USER   your Apple ID email (e.g. you@icloud.com)
  ICLOUD_PASS   app-specific password from appleid.apple.com

Apple Notes IMAP:
  host: imap.mail.me.com  port: 993  SSL: yes
  Notes appear as messages in the specified IMAP folder.
  Each message needs the X-Uniform-Type-Identifier header to be recognised
  as a note by the Notes app on iOS/macOS.
"""

from __future__ import annotations

import argparse
import imaplib
import os
import sys
import textwrap
from email.mime.multipart import MIMEMultipart
from email.mime.text import MIMEText
from email.utils import formatdate

IMAP_HOST = "imap.mail.me.com"
IMAP_PORT = 993


def build_message(title: str, body_html: str) -> bytes:
    msg = MIMEMultipart("alternative")
    msg["Subject"] = title
    msg["From"] = os.environ["ICLOUD_USER"]
    msg["Date"] = formatdate(localtime=True)
    # These two headers tell Apple Notes to treat this as a note, not an email.
    msg["X-Uniform-Type-Identifier"] = "com.apple.mail-note"
    msg["X-Mail-Created-Date"] = msg["Date"]

    # Plain-text fallback (Notes shows the HTML body; this is just in case).
    plain = MIMEText("", "plain", "utf-8")
    msg.attach(plain)

    html_part = MIMEText(body_html, "html", "utf-8")
    msg.attach(html_part)

    return msg.as_bytes()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--folder", required=True)
    parser.add_argument("--title",  required=True)
    parser.add_argument("--body",   required=True)
    args = parser.parse_args()

    user = os.environ.get("ICLOUD_USER")
    passwd = os.environ.get("ICLOUD_PASS")
    if not user or not passwd:
        print("ERROR: ICLOUD_USER and ICLOUD_PASS must be set", file=sys.stderr)
        sys.exit(1)

    msg_bytes = build_message(args.title, args.body)

    with imaplib.IMAP4_SSL(IMAP_HOST, IMAP_PORT) as imap:
        imap.login(user, passwd)

        # IMAP folder names with dots need to be quoted.
        folder = f'"{args.folder}"'
        status, _ = imap.select(folder)
        if status != "OK":
            # Folder doesn't exist — create it then select.
            imap.create(folder)
            imap.subscribe(folder)
            imap.select(folder)

        # APPEND adds the message directly to the folder as a new note.
        imap.append(folder, r"\Seen", None, msg_bytes)

    print(f"[imap_note] Appended to {args.folder}: {args.title!r}")


if __name__ == "__main__":
    main()
