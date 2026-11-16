// ui_test.js — end-to-end test of the web UI in headless Edge/Chrome against
// tools/mock_deck.js. No npm packages: drives the browser over the DevTools
// protocol with Node's built-in WebSocket (Node 22+).
//
//   node tools/ui_test.js          (starts the mock on port 8098 itself)
'use strict';
const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const PORT = 8098;
const DEVTOOLS = 9300 + Math.floor(Math.random() * 500);   // fresh port: never attach to a stale browser
// Killing the launcher leaves Edge's child processes alive; take down the tree.
const killTree = (p) => { try { spawn('taskkill', ['/PID', String(p.pid), '/T', '/F'], { stdio: 'ignore' }); } catch (_) { p.kill(); } };
const BROWSERS = [
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
  'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let failures = 0;
const check = (name, ok, extra = '') => { console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${extra ? '  ' + extra : ''}`); if (!ok) failures++; };

async function main() {
  const mock = spawn(process.execPath, [path.join(__dirname, 'mock_deck.js'), String(PORT)], { stdio: 'ignore' });
  const exe = BROWSERS.find((b) => fs.existsSync(b));
  if (!exe) throw new Error('no Edge/Chrome found');
  const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'deck-ui-'));
  const br = spawn(exe, ['--headless=new', '--disable-gpu', '--no-first-run', `--user-data-dir=${prof}`,
    `--remote-debugging-port=${DEVTOOLS}`, 'about:blank'], { stdio: 'ignore' });
  try {
    await sleep(1500);
    const targets = await (await fetch(`http://127.0.0.1:${DEVTOOLS}/json`)).json();
    const ws = new WebSocket(targets.find((t) => t.type === 'page').webSocketDebuggerUrl);
    await new Promise((r) => (ws.onopen = r));
    let id = 0;
    const pending = new Map();
    const errors = [];
    ws.onmessage = (e) => {
      const m = JSON.parse(e.data);
      if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
      if (m.method === 'Runtime.exceptionThrown') errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
      if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'error') errors.push(m.params.args.map((a) => a.value).join(' '));
    };
    const send = (method, params = {}) => new Promise((r) => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
    const evalJs = async (expr) => {
      const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true });
      if (r.result.exceptionDetails) throw new Error(r.result.exceptionDetails.exception?.description || 'eval failed');
      return r.result.result.value;
    };
    const api = (p, opt) => evalJs(`fetch(${JSON.stringify(p)}${opt ? ', ' + JSON.stringify(opt) : ''}).then(r => r.json())`);

    await send('Runtime.enable');
    await send('Page.enable');
    await send('Page.navigate', { url: `http://localhost:${PORT}/#keys` });
    await sleep(2000);
    // confirm() is answered by the dialog handler below; auto-accept.
    ws.addEventListener('message', (e) => { const m = JSON.parse(e.data); if (m.method === 'Page.javascriptDialogOpening') send('Page.handleJavaScriptDialog', { accept: true, promptText: 'Gaming' }); });

    check('12 tiles rendered', (await evalJs(`document.querySelectorAll('#grid .tile').length`)) === 12);
    check('2 pages listed', (await evalJs(`document.querySelectorAll('#pages button').length`)) === 2);
    check('K1 shows "Play"', (await evalJs(`document.querySelectorAll('#grid .tile')[0].textContent`)).includes('Play'));
    check('save disabled when clean', await evalJs(`document.querySelector('#btn-save').disabled`));

    // Edit K1: label + switch to hotkey via the type select.
    await evalJs(`document.querySelectorAll('#grid .tile')[0].click()`);
    check('editor opened for K1', (await evalJs(`document.querySelector('#editor h2').textContent`)) === 'Key K1');
    await evalJs(`(() => { const i = document.querySelector('#editor input'); i.value = 'Copy'; i.dispatchEvent(new Event('input')); })()`);
    await evalJs(`(() => { const s = document.querySelector('#editor fieldset select'); s.value = 'hotkey'; s.dispatchEvent(new Event('change')); })()`);
    await evalJs(`(() => { const i = document.querySelector('#editor fieldset input[placeholder^="ctrl"]'); i.value = 'ctrl+c'; i.dispatchEvent(new Event('input')); })()`);
    check('grid tile follows label edit', (await evalJs(`document.querySelectorAll('#grid .tile')[0].textContent`)).includes('Copy'));
    check('save enabled when dirty', !(await evalJs(`document.querySelector('#btn-save').disabled`)));

    // Record a hotkey by "pressing" keys.
    await evalJs(`[...document.querySelectorAll('#editor button')].find(b => b.textContent === 'Record').click()`);
    await evalJs(`window.dispatchEvent(new KeyboardEvent('keydown', { code: 'KeyV', key: 'v', ctrlKey: true, shiftKey: true }))`);
    check('hotkey recorder captured ctrl+shift+v', (await evalJs(`document.querySelector('#editor fieldset input[placeholder^="ctrl"]').value`)) === 'ctrl+shift+v');

    // Discord preset on K2.
    await evalJs(`document.querySelectorAll('#grid .tile')[1].click()`);
    await evalJs(`(() => { const s = document.querySelector('#editor .row select'); s.value = 'discord_mute'; s.dispatchEvent(new Event('change')); })()`);
    check('preset applied to K2', (await evalJs(`document.querySelectorAll('#grid .tile')[1].textContent`)).includes('Discord'));

    // Add a page (prompt answered with "Gaming").
    await evalJs(`document.querySelector('#page-add').click()`);
    await sleep(300);
    check('page added', (await evalJs(`[...document.querySelectorAll('#pages button')].map(b => b.textContent).join(',')`)).includes('Gaming'));

    // Save and check what the mock deck stored.
    await evalJs(`document.querySelector('#btn-save').click()`);
    await sleep(500);
    const cfg = await api('/api/config');
    check('saved: K1 is a ctrl+shift+v hotkey', cfg.pages[0].keys[0].press.type === 'hotkey' && cfg.pages[0].keys[0].press.keys === 'ctrl+shift+v');
    check('saved: K2 Discord flips discord.mute', cfg.pages[0].keys[1].press.flip === 'discord.mute');
    check('saved: 3 pages, empty trailing keys trimmed', cfg.pages.length === 3 && cfg.pages[2].keys.length === 0, `keys=${cfg.pages[2] && cfg.pages[2].keys.length}`);
    check('save button clean after save', await evalJs(`document.querySelector('#btn-save').disabled`));

    // Live state: press K2 on the "deck" -> discord.mute flips -> tile shows on_label after a poll.
    await evalJs(`document.querySelectorAll('#pages button')[0].click()`);   // back to Main (add-page switched to Gaming)
    await evalJs(`(async () => { await fetch('/api/press?key=1', { method: 'POST' }); })()`);
    await sleep(2600);
    check('live state turns K2 "on"', (await evalJs(`document.querySelectorAll('#grid .tile')[1].textContent`)).includes('Mic OFF'));

    // Validation error from the deck is shown, not swallowed.
    await evalJs(`(() => { cfg.pages[0].keys[0].color = 'red'; markDirty(); document.querySelector('#btn-save').click(); })()`);
    await sleep(500);
    check('deck validation error surfaced', (await evalJs(`document.querySelector('#toast').textContent`)).includes('#rrggbb'));

    // Settings tab loads.
    await evalJs(`document.querySelector('nav button[data-tab=settings]').click()`);
    check('settings tab shows SSID', (await evalJs(`document.querySelector('#s-ssid').value`)) === 'HomeNet');

    check('no JS errors', errors.length === 0, errors.join(' | '));
    ws.close();
  } finally {
    killTree(br);
    killTree(mock);
  }
  console.log(failures ? `\n${failures} FAILED` : '\nall passed');
  process.exit(failures ? 1 : 0);
}
main().catch((e) => { console.error(e); process.exit(2); });
