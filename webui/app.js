// StreamDeck web UI. Plain JS, no build step. Served gzipped from the firmware.
'use strict';

const $ = (sel, root = document) => root.querySelector(sel);
const el = (tag, props = {}, ...kids) => {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (k === 'class') e.className = v;
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (k === 'value' || k === 'checked' || k === 'disabled') e[k] = v;
    else if (k in e && typeof v !== 'string') e[k] = v;
    else e.setAttribute(k, v);
  }
  for (const kid of kids.flat()) if (kid != null) e.append(kid.nodeType ? kid : document.createTextNode(kid));
  return e;
};

const NUM_KEYS = 12;
let cfg = null;          // working copy being edited
let savedJson = '';      // last config saved to / loaded from the deck
let status = {};         // live device status
let editPage = 0;
let selKey = -1;
let icons = [];
let govee = [];
let settings = {};
let clipboard = null;

// ------------------------------------------------------------------ api
async function api(method, path, body) {
  const opt = { method, headers: {} };
  if (body !== undefined) {
    opt.body = typeof body === 'string' ? body : JSON.stringify(body);
    opt.headers['Content-Type'] = 'application/json';
  }
  const r = await fetch(path, opt);
  const text = await r.text();
  let data = text;
  try { data = JSON.parse(text); } catch (_) { /* plain text */ }
  if (!r.ok) throw new Error((data && data.error) || text || r.statusText);
  return data;
}

let toastTimer;
function toast(msg, err = false) {
  const t = $('#toast');
  t.textContent = msg;
  t.className = 'toast show' + (err ? ' err' : '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (t.className = 'toast'), err ? 6000 : 2500);
}

// ------------------------------------------------------------------ helpers
const isEmptyKey = (k) => !k || Object.keys(k).length === 0;
const page = () => cfg.pages[editPage];
const keyAt = (i) => page().keys[i];

function normalize(c) {
  for (const p of c.pages) {
    p.keys = p.keys || [];
    while (p.keys.length < NUM_KEYS) p.keys.push(null);
    p.keys = p.keys.slice(0, NUM_KEYS).map((k) => (isEmptyKey(k) ? null : k));
  }
  c.typing = c.typing || 'altcode';
  return c;
}

function hexToRgb(h) {
  const v = parseInt((h || '#5a5a5a').slice(1), 16);
  return [(v >> 16) & 255, (v >> 8) & 255, v & 255];
}
const luma = ([r, g, b]) => (r * 299 + g * 587 + b * 114) / 1000;
const rgbCss = ([r, g, b], f = 1) => `rgb(${Math.round(r * f)},${Math.round(g * f)},${Math.round(b * f)})`;

function stateOn(name) {
  if (!name) return false;
  if (name.startsWith('obs.scene:')) return (status.strings || {})['obs.scene'] === name.slice(10);
  return !!(status.states || {})[name];
}

function isDirty() { return JSON.stringify(cfg) !== savedJson; }

function markDirty() {
  const d = isDirty();
  $('#btn-save').disabled = !d;
  $('#save-msg').textContent = d ? 'Unsaved changes' : 'Saved';
  $('#save-msg').className = 'msg' + (d ? ' dirty' : '');
  renderGrid();
}

// ------------------------------------------------------------------ pages + grid
function renderPages() {
  const box = $('#pages');
  box.replaceChildren(...cfg.pages.map((p, i) => el('button', {
    class: (i === editPage ? 'sel ' : '') + (i === status.page ? 'live' : ''),
    title: i === status.page ? 'Showing on the deck now' : '',
    onclick: () => { editPage = i; selKey = -1; renderAll(); },
  }, p.name)));
}

function renderGrid() {
  const grid = $('#grid');
  const tiles = [];
  for (let i = 0; i < NUM_KEYS; i++) {
    const k = keyAt(i);
    const pressed = status.page === editPage && (status.pressed & (1 << i));
    const t = el('div', { class: 'tile', onclick: () => { selKey = i; renderGrid(); renderEditor(); } },
      el('span', { class: 'k' }, `K${i + 1}`));
    if (isEmptyKey(k)) {
      t.classList.add('empty');
      t.append('empty');
    } else {
      const on = stateOn(k.state);
      const rgb = hexToRgb(on && k.on_color ? k.on_color : k.color);
      t.style.background = rgbCss(rgb, on ? 1 : 0.6);
      if (luma(rgb.map((c) => c * (on ? 1 : 0.6))) > 150) t.classList.add('dark-text');
      if (k.icon) t.append(el('img', { src: '/icons/' + encodeURIComponent(k.icon), alt: '' }));
      t.append(el('span', {}, (on && k.on_label) || k.label || ''));
    }
    if (i === selKey) t.classList.add('sel');
    if (pressed) t.classList.add('pressed');
    tiles.push(t);
  }
  grid.replaceChildren(...tiles);
}

function renderAll() {
  renderPages();
  renderGrid();
  renderEditor();
  $('#typing').value = cfg.typing || 'altcode';
  updateStateNames();
}

// ------------------------------------------------------------------ action editor
const ACTION_TYPES = {
  none: 'Nothing',
  hotkey: 'Hotkey / shortcut',
  media: 'Media key',
  text: 'Type text',
  launch: 'Launch app (Win+R)',
  url: 'Open URL',
  macro: 'Macro (steps)',
  page: 'Go to page',
  obs: 'OBS',
  govee: 'Govee light',
  multi: 'Several actions (JSON)',
};
const MEDIA = { play_pause: 'Play / Pause', next: 'Next track', prev: 'Previous track', stop: 'Stop', mute: 'Mute', vol_up: 'Volume +', vol_down: 'Volume −' };
const OBS_OPS = { scene: 'Switch scene', record: 'Toggle recording', stream: 'Toggle streaming', mute: 'Toggle input mute', raw: 'Raw request' };
const GOVEE_OPS = { toggle: 'Toggle on/off', on: 'On', off: 'Off', brightness: 'Set brightness', color: 'Set color' };

function select(options, value, onchange) {
  const s = el('select', { onchange: (e) => onchange(e.target.value) },
    Object.entries(options).map(([v, t]) => el('option', { value: v }, t)));
  s.value = value;
  return s;
}

function field(labelText, input, hint) {
  return el('div', {}, el('label', {}, labelText), input, hint ? el('div', { class: 'hint' }, hint) : null);
}

function textInput(obj, prop, onChange, props = {}) {
  return el('input', Object.assign({ value: obj[prop] ?? '', oninput: (e) => { obj[prop] = e.target.value; onChange(); } }, props));
}

// Browser keydown -> our key names ("ctrl+shift+m").
function comboFromEvent(e) {
  const parts = [];
  if (e.ctrlKey) parts.push('ctrl');
  if (e.shiftKey) parts.push('shift');
  if (e.altKey) parts.push('alt');
  if (e.metaKey) parts.push('win');
  const c = e.code;
  const map = { Enter: 'enter', Escape: 'esc', Space: 'space', Backspace: 'backspace', Tab: 'tab', Delete: 'delete',
    Insert: 'insert', Home: 'home', End: 'end', PageUp: 'pageup', PageDown: 'pagedown', ArrowUp: 'up', ArrowDown: 'down',
    ArrowLeft: 'left', ArrowRight: 'right', Minus: '-', Equal: '=', BracketLeft: '[', BracketRight: ']', Backslash: '\\',
    Semicolon: ';', Quote: "'", Backquote: '`', Comma: ',', Period: '.', Slash: '/', PrintScreen: 'printscreen',
    CapsLock: 'capslock', ContextMenu: 'menu', Pause: 'pause', ScrollLock: 'scrolllock' };
  let k = null;
  if (/^Key[A-Z]$/.test(c)) k = c.slice(3).toLowerCase();
  else if (/^Digit\d$/.test(c)) k = c.slice(5);
  else if (/^Numpad\d$/.test(c)) k = c.slice(6);
  else if (/^F\d{1,2}$/.test(c)) k = c.toLowerCase();
  else if (map[c]) k = map[c];
  if (!k) return null;   // modifier alone: keep waiting
  parts.push(k);
  return parts.join('+');
}

function actionEditor(holder, a, onChange) {
  // `holder` is { get(), set(action) }; a is the current action (may be null).
  const box = el('div');
  const rebuild = () => box.replaceChildren(...build());
  const changed = () => onChange();

  function build() {
    const act = holder.get();
    const type = act ? act.type : 'none';
    const out = [select(ACTION_TYPES, type, (t) => {
      holder.set(t === 'none' ? null : defaultAction(t));
      rebuild();
      changed();
    })];
    if (!act) return out;

    if (type === 'hotkey') {
      const inp = textInput(act, 'keys', changed, { placeholder: 'ctrl+shift+m, alt+f4, f13, win+d' });
      const rec = el('button', { class: 'shrink', onclick: () => {
        rec.textContent = 'Press keys…';
        const h = (e) => {
          e.preventDefault();
          const combo = comboFromEvent(e);
          if (!combo) return;
          act.keys = combo;
          inp.value = combo;
          window.removeEventListener('keydown', h, true);
          rec.textContent = 'Record';
          changed();
        };
        window.addEventListener('keydown', h, true);
      } }, 'Record');
      out.push(field('Keys', el('div', { class: 'row' }, inp, rec),
        'Names: ctrl shift alt win, a-z 0-9, f1-f24, enter esc tab space up down left right home end pageup pagedown delete insert printscreen. F13-F24 are free keys for Discord/OBS binds.'));
      out.push(el('label', {}, el('input', { type: 'checkbox', checked: !!act.hold, onchange: (e) => { act.hold = e.target.checked || undefined; changed(); } }), ' Hold (key stays down while pressed, e.g. push-to-talk)'));
    } else if (type === 'media') {
      out.push(field('Key', select(MEDIA, act.key, (v) => { act.key = v; changed(); })));
    } else if (type === 'text') {
      out.push(field('Text', el('textarea', { rows: 3, value: act.text || '', oninput: (e) => { act.text = e.target.value; changed(); } })));
      out.push(el('label', {}, el('input', { type: 'checkbox', checked: !!act.enter, onchange: (e) => { act.enter = e.target.checked || undefined; changed(); } }), ' Press Enter after'));
    } else if (type === 'launch') {
      out.push(field('Command', textInput(act, 'cmd', changed, { placeholder: 'notepad, calc, "C:\\Program Files\\App\\app.exe", spotify:' }),
        'Anything the Windows Run dialog (Win+R) accepts.'));
    } else if (type === 'url') {
      out.push(field('URL', textInput(act, 'url', changed, { placeholder: 'https://…' })));
    } else if (type === 'page') {
      const opts = { next: 'Next page', prev: 'Previous page' };
      cfg.pages.forEach((p) => (opts[p.name] = 'Page: ' + p.name));
      out.push(field('Go to', select(opts, act.to, (v) => { act.to = v; changed(); })));
    } else if (type === 'macro') {
      out.push(macroEditor(act, changed));
    } else if (type === 'obs') {
      out.push(field('Operation', select(OBS_OPS, act.op, (v) => { act.op = v; rebuild(); changed(); })));
      if (act.op === 'scene') out.push(field('Scene name (exact, as in OBS)', textInput(act, 'scene', changed)));
      if (act.op === 'mute') out.push(field('Input name (exact, e.g. "Mic/Aux")', textInput(act, 'input', changed)));
      if (act.op === 'raw') {
        out.push(field('Request type', textInput(act, 'request', changed, { placeholder: 'e.g. TriggerHotkeyByName' })));
        out.push(field('Request data (JSON)', jsonInput(act, 'data', changed)));
      }
    } else if (type === 'govee') {
      const dev = textInput(act, 'device', changed, { placeholder: 'device IP', list: 'govee-ips' });
      out.push(field('Device', dev, 'IP address. Use Settings → Govee → Scan to discover.'));
      out.push(field('Operation', select(GOVEE_OPS, act.op, (v) => { act.op = v; rebuild(); changed(); })));
      if (act.op === 'brightness') out.push(field('Brightness (1–100)', textInput(act, 'value', () => { act.value = parseInt(act.value, 10) || 100; changed(); }, { type: 'number', min: 1, max: 100 })));
      if (act.op === 'color') out.push(field('Color', el('input', { type: 'color', value: act.color || '#ffffff', oninput: (e) => { act.color = e.target.value; changed(); } })));
    } else if (type === 'multi') {
      out.push(field('Actions (JSON array of actions)', jsonInput(act, 'actions', changed)));
    }
    out.push(field('Toggle a state after (optional)', textInput(act, 'flip', () => { if (!act.flip) delete act.flip; changed(); }, { list: 'state-names', placeholder: 'e.g. discord.mute' }),
      'For things the deck can\'t read back (Discord): the key flips this state so its color follows.'));
    return out;
  }
  rebuild();
  return box;
}

function jsonInput(obj, prop, onChange) {
  const ta = el('textarea', { rows: 4, value: obj[prop] ? JSON.stringify(obj[prop], null, 1) : '' });
  ta.addEventListener('change', () => {
    try {
      obj[prop] = ta.value.trim() ? JSON.parse(ta.value) : undefined;
      ta.style.borderColor = '';
      onChange();
    } catch (_) {
      ta.style.borderColor = 'var(--bad)';
      toast('Invalid JSON', true);
    }
  });
  return ta;
}

function defaultAction(t) {
  switch (t) {
    case 'hotkey': return { type: t, keys: '' };
    case 'media': return { type: t, key: 'play_pause' };
    case 'text': return { type: t, text: '' };
    case 'launch': return { type: t, cmd: '' };
    case 'url': return { type: t, url: 'https://' };
    case 'macro': return { type: t, steps: [] };
    case 'page': return { type: t, to: 'next' };
    case 'obs': return { type: t, op: 'record' };
    case 'govee': return { type: t, device: govee[0] ? govee[0].ip : '', op: 'toggle' };
    case 'multi': return { type: t, actions: [] };
    default: return { type: t };
  }
}

function macroEditor(act, changed) {
  const box = el('div', { class: 'steps' });
  const KINDS = { keys: 'Keys', text: 'Text', delay: 'Wait (ms)', media: 'Media key' };
  const render = () => {
    box.replaceChildren(
      ...act.steps.map((s, i) => {
        const kind = Object.keys(KINDS).find((k) => k in s) || 'keys';
        const val = kind === 'media'
          ? select(MEDIA, s.media, (v) => { s.media = v; changed(); })
          : el('input', { value: s[kind] ?? '', type: kind === 'delay' ? 'number' : 'text',
              oninput: (e) => { s[kind] = kind === 'delay' ? parseInt(e.target.value, 10) || 0 : e.target.value; changed(); } });
        return el('div', { class: 'row' },
          select(KINDS, kind, (k) => { act.steps[i] = { [k]: k === 'delay' ? 100 : k === 'media' ? 'play_pause' : '' }; render(); changed(); }),
          val,
          el('button', { class: 'shrink', onclick: () => { act.steps.splice(i, 1); render(); changed(); } }, '✕'));
      }),
      el('button', { style: 'margin-top:6px', onclick: () => { act.steps.push({ keys: '' }); render(); changed(); } }, '+ Step'));
  };
  render();
  return field('Steps (run in order)', box);
}

// ------------------------------------------------------------------ key editor
const PRESETS = {
  '': 'Presets…',
  discord_mute: 'Discord: toggle mute (Ctrl+Shift+F13)',
  discord_deafen: 'Discord: toggle deafen (Ctrl+Shift+F14)',
  obs_record: 'OBS: record',
  obs_stream: 'OBS: stream',
  obs_mic: 'OBS: mic mute',
  play: 'Media: play / pause',
  next_page: 'Next page',
};

function applyPreset(k, p) {
  const red = '#ff2020';
  const presets = {
    discord_mute: { label: 'Discord Mic', color: '#5865f2', press: { type: 'hotkey', keys: 'ctrl+shift+f13', flip: 'discord.mute' },
      long: { type: 'none', flip: 'discord.mute' }, state: 'discord.mute', on_color: red, on_label: 'Mic OFF' },
    discord_deafen: { label: 'Deafen', color: '#5865f2', press: { type: 'hotkey', keys: 'ctrl+shift+f14', flip: 'discord.deafen' },
      long: { type: 'none', flip: 'discord.deafen' }, state: 'discord.deafen', on_color: red, on_label: 'Deaf' },
    obs_record: { label: 'Record', color: '#606060', press: { type: 'obs', op: 'record' }, state: 'obs.record', on_color: red, on_label: 'REC' },
    obs_stream: { label: 'Stream', color: '#606060', press: { type: 'obs', op: 'stream' }, state: 'obs.stream', on_color: red, on_label: 'LIVE' },
    obs_mic: { label: 'Mic', color: '#20c060', press: { type: 'obs', op: 'mute', input: 'Mic/Aux' }, state: 'obs.mute:Mic/Aux', on_color: red, on_label: 'Mic OFF' },
    play: { label: 'Play', color: '#3080ff', press: { type: 'media', key: 'play_pause' } },
    next_page: { label: 'Next >', color: '#a0a0a0', press: { type: 'page', to: 'next' } },
  };
  return Object.assign({}, presets[p], k && k.icon ? { icon: k.icon } : {});
}

function renderEditor() {
  const ed = $('#editor');
  if (selKey < 0) {
    ed.replaceChildren(el('p', { class: 'hint' }, 'Click a key to edit it.'));
    return;
  }
  const i = selKey;
  let k = keyAt(i);
  const ensure = () => {
    if (!keyAt(i)) page().keys[i] = { label: `K${i + 1}`, color: '#3080ff' };
    k = keyAt(i);
    return k;
  };
  const changed = () => markDirty();

  const head = el('div', { class: 'row' },
    el('h2', { class: 'shrink', style: 'margin:0' }, `Key K${i + 1}`),
    el('span'),
    select(PRESETS, '', (p) => { if (!p) return; page().keys[i] = applyPreset(k, p); renderEditor(); changed(); }),
    el('button', { class: 'shrink', title: 'Run this key\'s press action on the PC now', onclick: testKey }, 'Test'),
    el('button', { class: 'shrink', onclick: () => { clipboard = JSON.stringify(keyAt(i)); toast('Key copied'); } }, 'Copy'),
    el('button', { class: 'shrink', disabled: !clipboard, onclick: () => { page().keys[i] = JSON.parse(clipboard); renderEditor(); changed(); } }, 'Paste'),
    el('button', { class: 'shrink danger', onclick: () => { page().keys[i] = null; renderEditor(); changed(); } }, 'Clear'));

  if (isEmptyKey(k)) {
    ed.replaceChildren(head, el('p', { class: 'hint' }, 'Empty key.'),
      el('button', { class: 'primary', onclick: () => { ensure(); renderEditor(); changed(); } }, 'Set up this key'));
    return;
  }

  const iconRow = el('div', { class: 'icons' },
    el('button', { class: 'shrink', onclick: () => { delete k.icon; renderEditor(); changed(); } }, 'No icon'),
    ...icons.map((ic) => el('img', { src: '/icons/' + encodeURIComponent(ic.name), title: ic.name, class: k.icon === ic.name ? 'sel' : '',
      onclick: () => { k.icon = ic.name; renderEditor(); changed(); } })));
  const upload = el('input', { type: 'file', accept: 'image/*', onchange: (e) => uploadIcon(e.target.files[0], (name) => { k.icon = name; renderEditor(); changed(); }) });

  const longOn = !!k.long;
  ed.replaceChildren(
    head,
    el('div', { class: 'row' },
      field('Label', textInput(k, 'label', changed)),
      el('div', { class: 'shrink' }, el('label', {}, 'Color'), el('input', { type: 'color', value: k.color || '#3080ff', oninput: (e) => { k.color = e.target.value; changed(); } }))),
    field('Icon (shown above the label; resized to 48×48)', el('div', {}, iconRow, el('div', { style: 'margin-top:6px' }, upload))),
    el('fieldset', {}, el('legend', {}, 'On press'),
      actionEditor({ get: () => k.press, set: (a) => { if (a) k.press = a; else delete k.press; } }, k.press, changed)),
    el('fieldset', {}, el('legend', {}, el('label', { style: 'display:inline;margin:0' },
      el('input', { type: 'checkbox', checked: longOn, onchange: (e) => { if (e.target.checked) k.long = { type: 'none' }; else delete k.long; renderEditor(); changed(); } }),
      ' Long press (hold ½ s)')),
      longOn ? actionEditor({ get: () => k.long, set: (a) => { k.long = a || { type: 'none' }; } }, k.long, changed)
             : el('div', { class: 'hint' }, 'With a long-press action, the normal action runs on release instead of on press.')),
    el('fieldset', {}, el('legend', {}, 'Live state (key turns "on")'),
      field('State', textInput(k, 'state', () => { if (!k.state) delete k.state; changed(); }, { list: 'state-names', placeholder: 'e.g. obs.record, obs.mute:Mic/Aux, obs.scene:Game, discord.mute' })),
      el('div', { class: 'row' },
        field('Label while on', textInput(k, 'on_label', () => { if (!k.on_label) delete k.on_label; changed(); })),
        el('div', { class: 'shrink' }, el('label', {}, 'Color while on'),
          el('input', { type: 'color', value: k.on_color || '#ff2020', oninput: (e) => { k.on_color = e.target.value; changed(); } })))),
  );
}

async function testKey() {
  const k = keyAt(selKey);
  if (!k || !k.press) return toast('This key has no press action', true);
  try {
    await api('POST', '/api/run', k.press);
    toast('Sent to the deck');
  } catch (e) { toast(e.message, true); }
}

// Resize to 48x48 (contain) in the browser, upload as PNG.
function uploadIcon(file, done) {
  if (!file) return;
  const img = new Image();
  img.onload = () => {
    const c = document.createElement('canvas');
    c.width = c.height = 48;
    const s = Math.min(48 / img.width, 48 / img.height);
    const w = img.width * s, h = img.height * s;
    c.getContext('2d').drawImage(img, (48 - w) / 2, (48 - h) / 2, w, h);
    c.toBlob(async (blob) => {
      let name = file.name.replace(/\.[^.]+$/, '').replace(/[^A-Za-z0-9_-]/g, '_').slice(0, 26) || 'icon';
      name += '.png';
      const fd = new FormData();
      fd.append('file', blob, name);
      try {
        const r = await fetch('/api/icons', { method: 'POST', body: fd });
        if (!r.ok) throw new Error((await r.text()) || r.statusText);
        icons = await api('GET', '/api/icons');
        done(name);
        toast('Icon uploaded');
      } catch (e) { toast('Upload failed: ' + e.message, true); }
    }, 'image/png');
    URL.revokeObjectURL(img.src);
  };
  img.onerror = () => toast('Not an image', true);
  img.src = URL.createObjectURL(file);
}

function updateStateNames() {
  const names = new Set(['discord.mute', 'discord.deafen', 'obs.record', 'obs.stream']);
  const walk = (a) => {
    if (!a) return;
    if (a.flip) names.add(a.flip);
    if (a.type === 'obs' && a.op === 'scene' && a.scene) names.add('obs.scene:' + a.scene);
    if (a.type === 'obs' && a.op === 'mute' && a.input) names.add('obs.mute:' + a.input);
    if (a.type === 'govee' && a.device) names.add('govee.on:' + a.device);
    (a.actions || []).forEach(walk);
  };
  for (const p of cfg.pages) for (const k of p.keys) if (k) { walk(k.press); walk(k.long); if (k.state) names.add(k.state); }
  govee.forEach((d) => names.add('govee.on:' + d.ip));
  $('#state-names').replaceChildren(...[...names].sort().map((n) => el('option', { value: n })));
  let gl = $('#govee-ips');
  if (!gl) document.body.append(gl = el('datalist', { id: 'govee-ips' }));
  gl.replaceChildren(...govee.map((d) => el('option', { value: d.ip }, `${d.sku} ${d.ip}`)));
}

// ------------------------------------------------------------------ page buttons
function wirePageButtons() {
  $('#page-add').onclick = () => {
    const name = prompt('Page name');
    if (!name) return;
    if (cfg.pages.some((p) => p.name === name)) return toast('A page with that name exists', true);
    cfg.pages.push({ name, keys: Array(NUM_KEYS).fill(null) });
    editPage = cfg.pages.length - 1;
    selKey = -1;
    renderAll();
    markDirty();
  };
  $('#page-rename').onclick = () => {
    const old = page().name;
    const name = prompt('Rename page', old);
    if (!name || name === old) return;
    if (cfg.pages.some((p) => p.name === name)) return toast('A page with that name exists', true);
    page().name = name;
    // keep "go to page" actions pointing at it
    const fix = (a) => { if (!a) return; if (a.type === 'page' && a.to === old) a.to = name; (a.actions || []).forEach(fix); };
    for (const p of cfg.pages) for (const k of p.keys) if (k) { fix(k.press); fix(k.long); }
    renderAll();
    markDirty();
  };
  const move = (d) => {
    const j = editPage + d;
    if (j < 0 || j >= cfg.pages.length) return;
    [cfg.pages[editPage], cfg.pages[j]] = [cfg.pages[j], cfg.pages[editPage]];
    editPage = j;
    renderAll();
    markDirty();
  };
  $('#page-left').onclick = () => move(-1);
  $('#page-right').onclick = () => move(1);
  $('#page-del').onclick = () => {
    if (cfg.pages.length === 1) return toast('The deck needs at least one page', true);
    if (!confirm(`Delete page "${page().name}"?`)) return;
    cfg.pages.splice(editPage, 1);
    editPage = Math.max(0, editPage - 1);
    selKey = -1;
    renderAll();
    markDirty();
  };
  $('#typing').onchange = (e) => { cfg.typing = e.target.value; markDirty(); };
  $('#btn-save').onclick = saveConfig;
  $('#btn-revert').onclick = () => {
    cfg = normalize(JSON.parse(savedJson));
    editPage = Math.min(editPage, cfg.pages.length - 1);
    renderAll();
    markDirty();
  };
  $('#btn-json').onclick = () => {
    $('#json-text').value = JSON.stringify(cfg, null, 2);
    $('#json-msg').textContent = '';
    $('#json-dlg').showModal();
  };
  $('#json-cancel').onclick = () => $('#json-dlg').close();
  $('#json-apply').onclick = () => {
    try {
      const c = normalize(JSON.parse($('#json-text').value));
      if (!Array.isArray(c.pages) || !c.pages.length) throw new Error('needs a "pages" array');
      cfg = c;
      editPage = 0;
      selKey = -1;
      $('#json-dlg').close();
      renderAll();
      markDirty();
    } catch (e) { $('#json-msg').textContent = e.message; }
  };
}

async function saveConfig() {
  const out = JSON.parse(JSON.stringify(cfg));
  for (const p of out.pages) {   // drop trailing empty keys
    while (p.keys.length && !p.keys[p.keys.length - 1]) p.keys.pop();
  }
  try {
    await api('PUT', '/api/config', out);
    savedJson = JSON.stringify(cfg);
    markDirty();
    toast('Saved to the deck');
  } catch (e) { toast('Not saved: ' + e.message, true); }
}

// ------------------------------------------------------------------ live status
function applyStatus(s) {
  status = s;
  const chip = (id, on, text) => { const c = $(id); c.classList.toggle('on', !!on); if (text) c.textContent = text; };
  chip('#chip-usb', s.usb, 'USB');
  chip('#chip-wifi', s.wifi && s.wifi.connected, s.wifi && s.wifi.ap && !s.wifi.connected ? 'Wi-Fi setup' : 'Wi-Fi');
  chip('#chip-obs', s.obs, 'OBS');
  chip('#chip-display', s.display, 'Display');
  $('#dev-info').innerHTML = '';
  $('#dev-info').append(
    `Firmware ${s.fw} · up ${fmtUptime(s.uptime)} · heap ${Math.round(s.heap / 1024)} KB · PSRAM ${Math.round(s.psram / 1024)} KB free`,
    el('br'),
    s.wifi ? `Wi-Fi: ${s.wifi.connected ? `${s.wifi.ssid} (${s.wifi.ip}, ${s.wifi.rssi} dBm)` : s.wifi.ap ? 'setup access point' : 'not connected'}` : '');
  if (cfg) { renderPages(); renderGrid(); }
  renderBanners();
}

function fmtUptime(t) {
  const h = Math.floor(t / 3600), m = Math.floor((t % 3600) / 60);
  return h ? `${h}h ${m}m` : `${m}m ${t % 60}s`;
}

function renderBanners() {
  const b = [];
  if (settings && settings.has_web_pass === false)
    b.push(el('div', { class: 'banner' }, 'No web password set: anyone on your network can change the deck. Set one in Settings.'));
  if (status.wifi && status.wifi.ap && !status.wifi.connected)
    b.push(el('div', { class: 'banner' }, 'The deck is in Wi-Fi setup mode. Enter your network in Settings.'));
  $('#banners').replaceChildren(...b);
}

let ws, pollTimer;
function connectWs() {
  try { ws = new WebSocket(`ws://${location.host}/ws`); } catch (_) { return startPolling(); }
  ws.onopen = () => { $('#chip-live').textContent = 'live'; $('#chip-live').classList.add('on'); stopPolling(); };
  ws.onmessage = (e) => { try { applyStatus(JSON.parse(e.data)); } catch (_) { /* ignore */ } };
  ws.onclose = () => {
    $('#chip-live').textContent = 'offline';
    $('#chip-live').classList.remove('on');
    startPolling();
    setTimeout(connectWs, 3000);
  };
}
function startPolling() {
  if (pollTimer) return;
  pollTimer = setInterval(async () => { try { applyStatus(await api('GET', '/api/status')); } catch (_) { /* offline */ } }, 2000);
}
function stopPolling() { clearInterval(pollTimer); pollTimer = null; }

// ------------------------------------------------------------------ settings tab
async function loadSettings() {
  settings = await api('GET', '/api/settings');
  $('#s-ssid').value = settings.wifi_ssid || '';
  $('#s-ssid-has').textContent = settings.has_wifi_pass ? '(saved)' : '';
  $('#s-host').value = settings.hostname || '';
  $('#s-web-has').textContent = settings.has_web_pass ? '(set)' : '(NOT SET)';
  $('#s-obshost').value = settings.obs_host || '';
  $('#s-obsport').value = settings.obs_port || 4455;
  $('#s-obs-has').textContent = settings.has_obs_pass ? '(saved)' : '';
  $('#s-tz').value = settings.tz || '';
  $('#s-pot').checked = !!settings.pot_enabled;
  $('#s-potinv').checked = !!settings.pot_invert;
  $('#s-bright').value = settings.fixed_brightness ?? 160;
  renderBanners();
}

function wireSettings() {
  $('#s-save').onclick = async () => {
    const body = {
      wifi_ssid: $('#s-ssid').value.trim(),
      hostname: $('#s-host').value.trim(),
      obs_host: $('#s-obshost').value.trim(),
      obs_port: parseInt($('#s-obsport').value, 10) || 4455,
      tz: $('#s-tz').value.trim(),
      pot_enabled: $('#s-pot').checked,
      pot_invert: $('#s-potinv').checked,
      fixed_brightness: parseInt($('#s-bright').value, 10) || 0,
    };
    for (const [id, key] of [['#s-wifipass', 'wifi_pass'], ['#s-webpass', 'web_pass'], ['#s-obspass', 'obs_pass']])
      if ($(id).value) body[key] = $(id).value;
    try {
      const r = await api('POST', '/api/settings', body);
      $('#s-msg').textContent = r.reboot ? 'Saved. The deck is rebooting to join the new Wi-Fi…' : 'Saved';
      ['#s-wifipass', '#s-webpass', '#s-obspass'].forEach((id) => ($(id).value = ''));
      if (!r.reboot) await loadSettings();
    } catch (e) { $('#s-msg').textContent = 'Error: ' + e.message; }
  };
  $('#govee-scan').onclick = async () => {
    try {
      await api('POST', '/api/govee/scan');
      $('#govee-list').textContent = 'Scanning…';
      setTimeout(loadGovee, 2500);
    } catch (e) { toast(e.message, true); }
  };
  $('#reboot').onclick = async () => {
    if (!confirm('Reboot the deck?')) return;
    try { await api('POST', '/api/reboot'); toast('Rebooting…'); } catch (e) { toast(e.message, true); }
  };
  $('#ota-go').onclick = () => {
    const f = $('#ota-file').files[0];
    if (!f) return toast('Choose a .bin file first', true);
    const fd = new FormData();
    fd.append('firmware', f, f.name);
    const xhr = new XMLHttpRequest();
    const prog = $('#ota-prog');
    prog.hidden = false;
    xhr.upload.onprogress = (e) => { if (e.lengthComputable) prog.value = (e.loaded / e.total) * 100; };
    xhr.onload = () => {
      $('#ota-msg').textContent = xhr.status === 200 ? 'Done. The deck is rebooting into the new firmware.' : 'Failed: ' + xhr.responseText;
    };
    xhr.onerror = () => ($('#ota-msg').textContent = 'Upload failed (connection lost)');
    xhr.open('POST', '/api/ota');
    xhr.send(fd);
    $('#ota-msg').textContent = 'Uploading…';
  };
  $('#log-refresh').onclick = loadLog;
}

async function loadGovee() {
  try {
    govee = await api('GET', '/api/govee');
    $('#govee-list').replaceChildren(...(govee.length
      ? govee.map((d) => el('div', {}, `${d.sku || 'Govee'} · ${d.ip}`))
      : ['No devices found. Is LAN Control on?']));
    if (cfg) updateStateNames();
  } catch (_) { /* ignore */ }
}

async function loadLog() {
  try { $('#log').textContent = await api('GET', '/api/log'); } catch (e) { $('#log').textContent = e.message; }
  const l = $('#log');
  l.scrollTop = l.scrollHeight;
}

// ------------------------------------------------------------------ tabs + boot
function showTab(name) {
  for (const b of document.querySelectorAll('nav button')) b.classList.toggle('active', b.dataset.tab === name);
  for (const s of document.querySelectorAll('main > section')) s.hidden = s.id !== 'tab-' + name;
  if (name === 'log') loadLog();
  if (location.hash !== '#' + name) history.replaceState(null, '', '#' + name);
}

window.addEventListener('beforeunload', (e) => { if (cfg && isDirty()) { e.preventDefault(); e.returnValue = ''; } });

async function boot() {
  for (const b of document.querySelectorAll('nav button')) b.onclick = () => showTab(b.dataset.tab);
  wirePageButtons();
  wireSettings();
  showTab((location.hash || '#keys').slice(1) || 'keys');
  try {
    const [c, ic, st] = await Promise.all([api('GET', '/api/config'), api('GET', '/api/icons'), api('GET', '/api/status')]);
    cfg = normalize(c);
    savedJson = JSON.stringify(cfg);
    icons = ic;
    status = st;
    editPage = Math.min(st.page || 0, cfg.pages.length - 1);
    renderAll();
    markDirty();
    applyStatus(st);
  } catch (e) {
    $('#banners').replaceChildren(el('div', { class: 'banner err' }, 'Could not load from the deck: ' + e.message));
  }
  loadSettings().catch(() => {});
  loadGovee();
  connectWs();
}
boot();
