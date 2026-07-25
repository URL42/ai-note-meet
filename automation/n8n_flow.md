# n8n flow: notemeet → Obsidian

The device POSTs a JSON payload to a webhook whenever a voice note is
transcribed (during Sync) or a meeting summary is finalised. n8n turns that
into a Markdown file in an Obsidian vault.

## Payload

```json
{
  "type": "note",
  "tag": "Work",
  "text": "Pick up milk",
  "timestamp": "2026-07-25T14:30:00Z"
}
```

`type` is `note`, `meeting`, or `test`. Meetings carry `title`, `summary`, and
`transcript` instead of `tag`/`text`. The `test` type comes from the dashboard's
**Send test payload** button and is written to a `test/` subfolder so it never
pollutes real notes.

## What gets written

| Payload | File | Contents |
|---|---|---|
| `note` | `notes/<date> <Tag>.md` | The transcript of the voice note |
| `meeting` | `meetings/<date> Meeting.md` | AI summary, then the full transcript under `## Full transcript` |
| `test` | `test/<date> Test.md` | Fixed test string |

Every file gets YAML frontmatter (`source`, `type`, `created`) so it's
queryable from Dataview.

The meeting summary already contains its own `##` headings, so it sits at top
level and the transcript is one more sibling section — fold it from the gutter
in Obsidian when you only want the summary. Long meetings make large files:
the device sends the complete transcript, so a two-hour meeting is roughly
90 KB of Markdown in one note.

Filenames are derived from the payload timestamp, so a device retry
overwrites the previous file rather than creating a duplicate.

## Setup

1. Import `notemeet_n8n_workflow.json` into n8n.
2. Check the `VAULT` constant at the top of the **Build note** node. It must
   be the path as seen *inside the container* — `/obsidian/notemeet`. It
   writes to `<VAULT>/notes/`, `<VAULT>/meetings/` and `<VAULT>/test/`;
   those directories must already exist, since the write node does not
   create them. On the host that means:
   `mkdir -p /home/anthony/obsidian/notemeet/{notes,meetings,test}`
3. Activate the workflow.
4. In the device dashboard → Settings, set the Webhook URL to the
   **production** URL (`https://<host>/webhook/notemeet`), save, then click
   **Send test payload**.

The button reports the HTTP status the device actually received, so a green
result means the file was written — not merely that n8n accepted the request.

## Why the flow is shaped this way

Three failure modes made the original version fail silently. Each is now
closed, and re-introducing any of them brings the silence back:

**The webhook responds when the workflow finishes** (`responseMode:
"lastNode"`), not immediately. n8n's default replies `200 {"message":"Workflow
was started"}` before running anything, so the device could never distinguish
"note filed" from "workflow exploded on the first node".

**The write uses the native Read/Write File node**, not `require('fs')` in a
Code node. The original called `mkdirSync(dir, {recursive: true})` before
writing, which means a wrong path was *silently created* inside the container
and written into a layer that disappears on restart — a misconfiguration that
looks exactly like success. The native node fails loudly with `ENOENT`
instead. It also removes any dependency on `NODE_FUNCTION_ALLOW_BUILTIN`.

**An unrecognised `type` throws** instead of being dropped. The original Switch
node had `fallbackOutput: "none"`, so a payload that didn't match `note` or
`meeting` exactly — including the version-dependent `$json.body.type` vs
`$json.type` difference — vanished without a trace.

## The delivery chain

```
device  →  n8n container (bossbitch)  →  /obsidian/notemeet          [in container]
                                      =  /home/anthony/obsidian/notemeet  [on host]
                                                  ↓  Syncthing
                                   ~/Documents/obsidian/notemeet  (Mac, Obsidian)
```

**n8n runs in Docker on bossbitch**, with the host's `/home/anthony/obsidian`
bind-mounted to `/obsidian` in the container. Workflow paths must use the
**container** path (`/obsidian/notemeet/...`); a host path writes into the
container's own filesystem, where files are invisible from outside and are
lost when the container is recreated.

n8n only ever writes to local disk on its own host. Getting the file to the
machine running Obsidian is Syncthing's job, and **both** Syncthing instances
have to be running for a note to arrive. Each end needs supervision, and
neither reports anything when it isn't running — the device and n8n both
report success regardless, because from their side nothing failed.

**bossbitch** runs the per-user unit `syncthing.service`
(`/usr/lib/systemd/user/syncthing.service`), which requires lingering, or it
only runs while someone is logged in over SSH:

```bash
loginctl show-user anthony --property=Linger   # must be yes
sudo loginctl enable-linger anthony
```

Do **not** enable the system-wide `syncthing@anthony` unit as well — two
instances fight over the same database and the second dies with
`Error opening database: resource temporarily unavailable`.

**Mac** runs `~/Library/LaunchAgents/com.syncthing.syncthing.plist`
(`RunAtLoad` + `KeepAlive`, headless). Don't also run the menu-bar
Syncthing.app; same database-lock conflict. Web UI stays at
`http://127.0.0.1:8384`.

This combination is what caused a month of silent failure: bossbitch's
Syncthing ran only during SSH sessions, the Mac's only while the app was
open, and notes could only arrive when both were true at once.

## If the device reports success but no note appears

The file was written to the filesystem **n8n is running on**. If your vault
lives on a different machine, that's the whole problem: nothing moves it
across on its own.

```bash
# on the n8n host
ls -la /home/anthony/obsidian/notemeet/notes/
```

Files present here but absent in Obsidian means you need a sync layer between
the two machines (Syncthing, an SMB mount, or a git pull), not a workflow fix.

If n8n runs in Docker, also check that the vault path is a mounted volume —
otherwise the writes land inside the container and disappear when it restarts:

```bash
docker exec <n8n-container> ls /home/anthony/obsidian/notemeet/notes/
```

## Legacy

`imap_note.py` belongs to an earlier version of this flow that filed notes into
Apple Notes over iCloud IMAP. It is not used by the workflow above and is kept
only for reference.
