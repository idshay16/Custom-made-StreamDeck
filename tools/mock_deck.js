// mock_deck.js — fake deck for developing the web UI without hardware.
//
//   node tools/mock_deck.js [port]      then open http://localhost:8080
//
// Serves webui/ and emulates the firmware's REST API in memory. The initial
// config is the firmware's real DEFAULT_CONFIG (parsed out of
// firmware/streamdeck/default_config.h), so this also checks it's valid JSON.
// No WebSocket: the UI falls back to polling /api/status, which is fine here.
'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..');
const WEBUI = path.join(ROOT, 'webui');
const PORT = parseInt(process.argv[2] || '8080', 10);

const header = fs.readFileSync(path.join(ROOT, 'firmware/streamdeck/default_config.h'), 'utf8');
const m = header.match(/R"json\(([\s\S]*?)\)json"/);
if (!m) throw new Error('DEFAULT_CONFIG not found in default_config.h');
let config = JSON.parse(m[1]);

const icons = new Map();   // name -> Buffer
const log = [];
const settings = { wifi_ssid: 'HomeNet', wifi_pass: 'x', hostname: 'streamdeck', web_pass: '', obs_host: '', obs_port: 4455,
  obs_pass: '', tz: 'IST-2IDT,M3.4.4/26,M10.5.0', pot_enabled: true, pot_invert: false, fixed_brightness: 160 };
const st = { page: 0, pressed: 0, brightness: 128, states: {}, strings: { 'obs.scene': 'Scene' } };
const t0 = Date.now();

const L = (s) => { const line = `[${((Date.now() - t0) / 1000).toFixed(3).padStart(7)}] ${s}`; log.push(line); console.log(line); if (log.length > 200) log.shift(); };

// Same rules as config::validate in config.cpp (subset that matters for the UI).
const TYPES = ['none', 'hotkey', 'media', 'text', 'launch', 'url', 'macro', 'page', 'obs', 'govee', 'multi'];
function validate(c) {
  if (c.version !== 1) return 'config: "version" must be 1';
  if (!Array.isArray(c.pages) || !c.pages.length) return 'config: needs at least one page';
  const names = new Set();
  for (const [i, p] of c.pages.entries()) {
    if (!p.name) return `pages[${i}]: needs a "name"`;
    if (names.has(p.name)) return `pages[${i}]: duplicate page name`;
    names.add(p.name);
    if (!Array.isArray(p.keys)) return `pages[${i}]: needs "keys" array`;
    if (p.keys.length > 12) return `pages[${i}]: more than 12 keys`;
    for (const [k, key] of p.keys.entries()) {
      if (!key) continue;
      for (const which of ['press', 'long']) {
        const a = key[which];
        if (a && !TYPES.includes(a.type)) return `pages[${i}].keys[${k}].${which}: unknown action type "${a.type}"`;
        if (a && a.type === 'hotkey' && !a.keys) return `pages[${i}].keys[${k}].${which}: needs "keys"`;
      }
      for (const c2 of ['color', 'on_color']) if (key[c2] && !/^#[0-9a-fA-F]{6}$/.test(key[c2])) return `pages[${i}].keys[${k}]: "${c2}" must be #rrggbb`;
    }
  }
  return null;
}

function runAction(a) {
  if (!a) return;
  if (a.type === 'page') {
    const n = config.pages.length;
    if (a.to === 'next') st.page = (st.page + 1) % n;
    else if (a.to === 'prev') st.page = (st.page + n - 1) % n;
    else { const i = config.pages.findIndex((p) => p.name === a.to); if (i >= 0) st.page = i; }
  } else if (a.type === 'obs') {
    if (a.op === 'record') st.states['obs.record'] = !st.states['obs.record'];
    if (a.op === 'stream') st.states['obs.stream'] = !st.states['obs.stream'];
    if (a.op === 'mute') st.states['obs.mute:' + a.input] = !st.states['obs.mute:' + a.input];
    if (a.op === 'scene') st.strings['obs.scene'] = a.scene;
  } else if (a.type === 'multi') (a.actions || []).forEach(runAction);
  if (a.flip) st.states[a.flip] = !st.states[a.flip];
  L(`action -> ${a.type}${a.keys ? ' ' + a.keys : ''}`);
}

function status() {
  return { ...st, page_name: config.pages[st.page].name, pages: config.pages.length,
    wifi: { connected: true, ap: false, ip: '127.0.0.1', ssid: settings.wifi_ssid, rssi: -48 },
    obs: !!settings.obs_host, usb: true, display: false, uptime: Math.floor((Date.now() - t0) / 1000),
    heap: 180000, psram: 1700000, fw: '0.1.0-mock' };
}

function readBody(req) {
  return new Promise((res) => { const ch = []; req.on('data', (c) => ch.push(c)); req.on('end', () => res(Buffer.concat(ch))); });
}

// Minimal multipart parser: first part's filename + bytes.
function parseMultipart(buf, ctype) {
  const b = '--' + ctype.split('boundary=')[1];
  const start = buf.indexOf(b) + b.length + 2;
  const headEnd = buf.indexOf('\r\n\r\n', start);
  const head = buf.slice(start, headEnd).toString();
  const end = buf.indexOf('\r\n' + b, headEnd);
  const fn = (head.match(/filename="([^"]*)"/) || [])[1] || '';
  return { filename: fn, data: buf.slice(headEnd + 4, end) };
}

const json = (res, code, obj) => { res.writeHead(code, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(obj)); };
const MIME = { '.html': 'text/html', '.js': 'application/javascript', '.css': 'text/css', '.png': 'image/png' };

http.createServer(async (req, res) => {
  const u = new URL(req.url, 'http://x');
  const p = u.pathname;
  try {
    if (req.method === 'GET' && (p === '/' || /^\/[\w.-]+\.(html|js|css)$/.test(p))) {
      const f = path.join(WEBUI, p === '/' ? 'index.html' : p.slice(1));
      if (!fs.existsSync(f)) return json(res, 404, { error: 'not found' });
      res.writeHead(200, { 'Content-Type': MIME[path.extname(f)] });
      return res.end(fs.readFileSync(f));
    }
    if (p === '/api/status') return json(res, 200, status());
    if (p === '/api/config' && req.method === 'GET') return json(res, 200, config);
    if (p === '/api/config' && req.method === 'PUT') {
      const c = JSON.parse((await readBody(req)).toString());
      const err = validate(c);
      if (err) return json(res, 400, { error: err });
      config = c;
      if (st.page >= config.pages.length) st.page = 0;
      L(`config: applied (${config.pages.length} pages)`);
      return json(res, 200, { ok: true });
    }
    if (p === '/api/icons' && req.method === 'GET') return json(res, 200, [...icons].map(([name, d]) => ({ name, size: d.length })));
    if (p === '/api/icons' && req.method === 'POST') {
      const { filename, data } = parseMultipart(await readBody(req), req.headers['content-type']);
      if (!/^[\w.-]{1,27}\.png$/.test(filename)) return json(res, 400, { error: 'bad icon name' });
      if (data.length > 65536) return json(res, 400, { error: 'icon larger than 64 KB' });
      icons.set(filename, data);
      L(`icon: ${filename} ${data.length} bytes`);
      return json(res, 200, { ok: true });
    }
    if (p === '/api/icons' && req.method === 'DELETE') { icons.delete(u.searchParams.get('name')); return json(res, 200, { ok: true }); }
    if (p.startsWith('/icons/')) {
      const d = icons.get(decodeURIComponent(p.slice(7)));
      if (!d) return json(res, 404, { error: 'no icon' });
      res.writeHead(200, { 'Content-Type': 'image/png' });
      return res.end(d);
    }
    if (p === '/api/press') {
      const k = parseInt(u.searchParams.get('key'), 10);
      if (!(k >= 0 && k < 12)) return json(res, 400, { error: 'key must be 0..11' });
      const key = config.pages[st.page].keys[k];
      st.pressed |= 1 << k;
      setTimeout(() => (st.pressed &= ~(1 << k)), 300);
      runAction(key && (u.searchParams.get('long') === '1' && key.long ? key.long : key.press));
      return json(res, 200, { ok: true });
    }
    if (p === '/api/run') { runAction(JSON.parse((await readBody(req)).toString())); return json(res, 200, { ok: true }); }
    if (p === '/api/settings' && req.method === 'GET') {
      const { wifi_pass, web_pass, obs_pass, ...rest } = settings;
      return json(res, 200, { ...rest, has_wifi_pass: !!wifi_pass, has_web_pass: !!web_pass, has_obs_pass: !!obs_pass });
    }
    if (p === '/api/settings' && req.method === 'POST') {
      const b = JSON.parse((await readBody(req)).toString());
      const wifi = (b.wifi_ssid !== undefined && b.wifi_ssid !== settings.wifi_ssid) || !!b.wifi_pass;
      for (const [k, v] of Object.entries(b)) if (v !== '' || !k.endsWith('_pass')) settings[k] = v;
      L('settings: saved');
      return json(res, 200, { ok: true, reboot: wifi });
    }
    if (p === '/api/govee') return json(res, 200, [{ ip: '192.168.1.77', sku: 'H6056', id: 'AA:BB' }]);
    if (p === '/api/govee/scan') return json(res, 200, { ok: true });
    if (p === '/api/log') { res.writeHead(200, { 'Content-Type': 'text/plain' }); return res.end(log.join('\n') + '\n'); }
    if (p === '/api/ota') { await readBody(req); L('ota: received (mock)'); return json(res, 200, { ok: true }); }
    if (p === '/api/reboot') { L('reboot (mock)'); return json(res, 200, { ok: true }); }
    json(res, 404, { error: 'not found' });
  } catch (e) {
    json(res, 500, { error: String(e.message || e) });
  }
}).listen(PORT, () => L(`mock deck on http://localhost:${PORT}`));
