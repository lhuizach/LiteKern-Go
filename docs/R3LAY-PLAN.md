# R3lay for LiteKern Go: plan

A native C client for R3lay, the owner's messaging app. It talks to the same Supabase project as the web app, so it shares the same accounts, chats and messages. No calls in this version.

Worked out from the web app inside `r3lay-1.4.0.apk` (a Capacitor wrapper around one 485 KB page) and four screenshots. Tables and row rules were then checked against the real project.

## Where it stands

Built in `apps/r3lay/`: sign in, stay signed in, sign out, chats and groups, reading, polling for new messages, sending, read ticks, finding a person, notifications while it runs. Tested against a fake server (`tests/r3lay/test-r3lay.sh`), and confirmed on the phone over Wi-Fi with a real account on 7 Nov 2026 (`go14`): sign-in, real chats, sending and replies.

Not done yet: accented text (accents come off, emoji are left out), profile pictures, settings and profile editing, Realtime.

Setup: copy `apps/r3lay/r3lay.conf.example` to `apps/r3lay/r3lay.conf` (git-ignored) and fill it in. `linux/build.sh` puts it next to the program.

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
- **Network:** apps are Linux programs, so no new system call was needed. The app loads libcurl itself and uses its non-blocking interface, so a slow network can't freeze it, and tokens never pass through the shell.
- **New messages:** poll every few seconds while open, slower in the background. Supabase Realtime over WebSocket comes later. Polling only asks for rows newer than the last one seen.
- **JSON:** a small parser in the app.
- **Sign-in tokens:** Supabase's refresh token, saved as `r3lay.session` in the user's home folder, so the password is never stored. That folder isn't private yet: the Files app can see it.
- **Project URL and public key:** read from `r3lay.conf`, kept out of git.
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
5. Send a message, with `client_ref`. Nothing in the database makes `client_ref` unique, so the app only retries after a poll has shown the message didn't arrive.
6. Groups.
7. Accented text.
8. Profile pictures.
9. Settings and profile editing.
10. Notifications through the shell.
11. Realtime instead of polling.

Calls come after sound and WebRTC or WebKit, which is a separate decision.

## Needed from the owner

- A test account, typed in on the device, never in chat or git.
- The calls block (see below): the column is live; his account still needs setting.

## Calls until they're supported

Nobody can ring an account marked `calls_disabled`. The block sits in the web app and the database, so the C client simply never sees a ring. Remove the flag when calls arrive.
