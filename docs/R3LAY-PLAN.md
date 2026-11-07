# R3lay for LiteKern Go: plan

A native C client for R3lay, the owner's messaging app. It talks to the same Supabase project as the web app, so it shares the same accounts, chats and messages. No calls in this version.

Worked out from the web app inside `r3lay-1.4.0.apk` (a Capacitor wrapper around one 485 KB page) and four screenshots. Column lists below are only what that page uses, so they need checking against the real schema.

## Scope

In the first version:

- Sign in with email and password, stay signed in, sign out.
- Chats: one-to-one and groups, newest first, with last message and date; find a person by username.
- Chat: bubbles, date separators, sending text, read ticks, call history shown as plain lines.
- Profile: picture, username, bio.
- Settings: Appearance, Privacy, Sign out.
- A notification when a message arrives while the app is closed but the phone is on.

Not in the first version: calls, voice messages, sending files, reactions, moderation tools, orbs, quests, shop, app updates.

## What the backend looks like

| Purpose | Name | Fields seen |
| --- | --- | --- |
| Direct messages | `messages` | `id`, `sender_id`, `recipient_id`, `text_content`, `created_at`, `read_at`, `client_ref`, `reply_to_id`, `media_url`, `deleted_at` |
| Group messages | `group_messages` | as above, with `group_id` |
| Groups | `groups`, `group_members` | `name`, `created_by`; `group_id`, `user_id`, `role` |
| Read state | `group_reads` | `group_id`, `last_read_at` |
| People | `profiles` | `id`, `username`, `bio`, `avatar_url`, `last_seen_at` |
| Blocking | `blocked_users` | `blocker_id`, `blocked_id` |
| Call signalling | `call_signals` | `caller_id`, `receiver_id`, `signal_type`, `call_type`, `payload` |
| Files | avatar and chat media buckets | exact names to confirm |
| On login | RPC `touch_login` | |

Call history is stored as a marker in `text_content` (`callMarkerText`, `parseCallMarker`), so the client must turn those markers into readable lines.

Security stays where it is: the client uses the same public key and the database's row rules, exactly like the web app.

## How it fits our OS

- **App:** `apps/r3lay/`, a normal KERN86 app using the shell's widgets and the flat Adwaita dark look.
- **Network:** apps have no way to reach the internet yet, only Wi-Fi status calls. Add a small HTTPS system call (`SYS_HTTP`) in `platform/`, on top of the libcurl and OpenSSL already in the image. Non-blocking, so a slow network can't freeze the shell.
- **New messages:** poll every few seconds while open, slower in the background. Supabase Realtime over WebSocket comes later. Polling only asks for rows newer than the last one seen.
- **JSON:** a small parser in the app.
- **Sign-in tokens:** Supabase's refresh token, saved in the app's private storage, so the password is never stored.
- **Project URL and public key:** read from a file kept out of git, not written in the source.
- **Pictures:** a small JPEG decoder (stb_image), with avatars shrunk once and cached.

## Known gaps to close

- **Text:** I found no Unicode handling in the text code, so accented letters probably need new font work (to confirm). French messages depend on this. Colour emoji can wait; they'd show as a plain box or be left out at first.
- **Keyboard:** the on-screen keyboard needs checking for accents and long messages.
- **Blocked by Wi-Fi on the phone:** everything works in the VM first, which has internet.

## Build order

Each step is tested in the VM, then on the phone once Wi-Fi works.

1. `SYS_HTTP` and a test that fetches a page.
2. Sign in, stay signed in, sign out.
3. Chat list, from one-to-one messages.
4. Open a chat, read messages, polling for new ones.
5. Send a message, with `client_ref` so a retry can't send it twice.
6. Groups.
7. Accented text.
8. Profile pictures.
9. Settings and profile editing.
10. Notifications through the shell.
11. Realtime instead of polling.

Calls come after sound and WebRTC or WebKit, which is a separate decision.

## Needed from the owner

- Confirm the tables above, ideally with the schema or the source.
- A test account, typed in on the device, never in chat or git.
- The calls block (see below) live on the backend before his brother uses the app.

## Calls until they're supported

Nobody can ring an account marked `calls_disabled`. The block sits in the web app and the database, so the C client simply never sees a ring. Remove the flag when calls arrive.
