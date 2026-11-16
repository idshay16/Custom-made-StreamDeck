# Test plan: firmware v0.1.0 on real hardware

What has been checked without hardware: everything compiles, and the web UI
passes an end-to-end browser test against a mock of the deck's API
(`node tools/ui_test.js`). Everything below needs the board.

Each section is independent. Do them in order the first time; tick them off.
Commands run from the repo root in PowerShell. `COMx` = the board's port
(`.\scripts\build.ps1 -ListPorts`).

## 1. Flash and boot (board only, cable in the "USB" port)

```powershell
.\scripts\build.ps1 firmware\streamdeck -Port COMx -Usb
& "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" monitor -p COMx
```
If the upload can't find the board: hold BOOT, tap RST, and upload again without `-Usb`.

- [ ] Console prints `DIY StreamDeck 0.1.0`, `config: 2 pages`, `render: no display detected ... headless`, `web: server on port 80`.
- [ ] `status` prints JSON with `"usb": true`.
- [ ] Windows shows a keyboard + consumer control "DIY StreamDeck".

## 2. Keys without wiring (console simulates presses)

- [ ] `press 1` → media play/pause toggles (open YouTube first).
- [ ] `press 6` → volume up. `press 5` → volume down.
- [ ] `press 12` → `page` changes to OBS (`status` shows `"page": 1`); `press 12` again → back to Main.
- [ ] `long 7` → Discord mute state flips without sending a key (`status` → `states`).

## 3. Keyboard layout (the important one)

The "Notepad" key (K11) launches via Win+R and types `notepad` with Alt codes.
- [ ] English layout active: `press 11` → Notepad opens.
- [ ] Switch to Hebrew (Alt+Shift): `press 11` → Notepad still opens.
- [ ] If Hebrew fails: tell me what appeared in the Run box.

## 4. Wi-Fi + web UI

Either on the console:
```
wifi "YourNetwork" "YourPassword"
```
or: the deck's setup network **StreamDeck-Setup** (password `streamdeck`) appears ~20 s after boot with no credentials. Join it from a phone; the settings page opens.

- [ ] Console: `net: connected to ...` and `http://streamdeck.local`.
- [ ] Open http://streamdeck.local (or the IP from the log). Grid shows Main page.
- [ ] Settings → set a **web password** (user `admin`). Reload: the browser asks for it.
- [ ] Edit K4: change label, Save to deck. Console shows `config: applied`.
- [ ] Icon: upload any PNG/JPG on a key → it appears on the tile.
- [ ] Click **Test** on a key → the action runs on the PC.
- [ ] `press 1` on the console → the K1 tile flashes in the browser (live updates).
- [ ] Clock: once on Wi-Fi, `status` → time is right (status bar shows it once the display is wired).

## 5. OBS

OBS → Tools → WebSocket Server Settings → enable, note port + password.
- [ ] Web UI Settings → OBS: PC IP, port 4455, password → Save. Console: `obs: identified`.
- [ ] Chip "OBS" turns green.
- [ ] Main page K9 (Record): `press 9` → OBS starts recording; the key turns red ("REC"). Stop recording inside OBS → key goes back.
- [ ] OBS page: scene keys follow the active scene, also when switched inside OBS. Scene names must match exactly (defaults: "Scene", "Scene 2").
- [ ] Wrong password → console says `closed during handshake: wrong OBS password?`.

## 6. Discord

Discord → User Settings → Keybinds → add "Toggle Mute" = press K7 on the deck while recording the bind (sends Ctrl+Shift+F13). "Toggle Deafen" = K8 (Ctrl+Shift+F14).
- [ ] K7 mutes/unmutes Discord; key color follows.
- [ ] Out of step? Long-press K7 flips the deck's state without touching Discord.

## 7. Govee H6056

Govee Home app → the light → settings → **LAN Control** on.
- [ ] Web UI Settings → Govee → Scan → the light appears with its IP.
- [ ] Set a key to Govee / Toggle with that IP → it switches the light.

## 8. OTA + rollback

- [ ] Build a .bin: `.\scripts\build.ps1 firmware\streamdeck` → the .bin is in the arduino-cli build folder (path printed with `--verbose`; or Arduino IDE → Sketch → Export compiled binary).
- [ ] Settings → Firmware update → upload → deck reboots; `status` still works.

## 9. Wiring steps (when parts/wires are ready)

- [ ] `03_keys_leds`: see the sketch header. Chain order vs `LED_INDEX`, no chatter, pot sweep.
- [ ] `04_display`: check interface jumpers first (sketch header). Color bars, border visible, fps.
- [ ] Main firmware with display: tiles, status bar clock + Wi-Fi/OBS, pressed highlight, knob dims the backlight.
