'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ui = require('../web/app.js');
const base = { wifi_ssid: 'rehab-test', socket_server: '192.168.0.100', heart_rate_address: 'aa:bb:cc:dd:ee:ff', change_password: false, password_mode: 'secured', wifi_password: '' };

test('saving unrelated settings preserves the stored password by omitting it', () => {
  const payload = ui.buildConfigPayload(base, 4);
  assert.equal(Object.hasOwn(payload, 'wifi_password'), false);
  assert.equal(payload.revision, 4);
  assert.equal(payload.heart_rate_address, 'AA:BB:CC:DD:EE:FF');
});
test('only explicit open-network selection clears a stored password', () => {
  assert.throws(() => ui.buildConfigPayload({ ...base, change_password: true }, 1), /無密碼網路/);
  assert.equal(ui.buildConfigPayload({ ...base, change_password: true, password_mode: 'open' }, 1).wifi_password, '');
  assert.equal(Object.hasOwn(ui.buildConfigPayload({ ...base, password_mode: 'open' }, 1), 'wifi_password'), false);
  assert.equal(ui.buildConfigPayload({ ...base, change_password: true, wifi_password: '12345678' }, 1).wifi_password, '12345678');
});
test('Wi-Fi credentials enforce encoded limits without trimming meaningful whitespace', () => {
  assert.equal(ui.buildConfigPayload({ ...base, wifi_ssid: ' office ' }, 1).wifi_ssid, ' office ');
  assert.throws(() => ui.buildConfigPayload({ ...base, wifi_ssid: '復'.repeat(11) }, 1), /32/);
  assert.throws(() => ui.buildConfigPayload({ ...base, change_password: true, wifi_password: '密'.repeat(22) }, 1), /63/);
  assert.equal(ui.buildConfigPayload({ ...base, change_password: true, wifi_password: 'a'.repeat(64) }, 1).wifi_password.length, 64);
  assert.throws(() => ui.buildConfigPayload({ ...base, change_password: true, wifi_password: 'z'.repeat(64) }, 1));
});
test('host and MAC validation rejects accidental URLs/ports but accepts manual hostnames and empty BLE', () => {
  for (const socket_server of ['http://host', 'host:9999', '999.1.2.3', '0000.1.2.3', 'host/path', 'a..b', '-host', 'a.-b']) {
    assert.throws(() => ui.buildConfigPayload({ ...base, socket_server }, 1), { field: 'socketServer' });
  }
  assert.equal(ui.buildConfigPayload({ ...base, socket_server: ' rehab-server.local ', heart_rate_address: '' }, 1).socket_server, 'rehab-server.local');
  assert.throws(() => ui.buildConfigPayload({ ...base, heart_rate_address: 'aabbccddeeff' }, 1), { field: 'heartAddress' });
  assert.throws(() => ui.buildConfigPayload(base, null), /版本/);
  assert.throws(() => ui.buildConfigPayload(base, 0), /版本/);
  assert.throws(() => ui.buildConfigPayload(base, 0x100000000), /版本/);
  assert.throws(() => ui.buildConfigPayload({ ...base, wifi_ssid: 'test\0ssid' }, 1), { field: 'wifiSsid' });
  assert.throws(() => ui.buildConfigPayload({ ...base, change_password: true, wifi_password: 'test\0password' }, 1), { field: 'wifiPassword' });
});
test('unknown values differ from zero, and stale values cannot appear current', () => {
  assert.equal(ui.display(0), '0'); assert.equal(ui.display(null), '—'); assert.equal(ui.display(false), '否');
  assert.equal(ui.sampleState(0, true, 0), 'fresh');
  assert.equal(ui.sampleState(75, true, 9999), 'fresh');
  assert.equal(ui.sampleState(75, true, 10000), 'stale');
  assert.equal(ui.sampleState(75, true, 10001), 'stale');
  assert.equal(ui.sampleState(75, false, 1), 'stale');
  assert.equal(ui.sampleState(75, true, null), 'stale');
  assert.equal(ui.sampleState(null, true, 0), 'missing');
  assert.equal(ui.display(300), '300'); // A 16-bit heart rate stays visible without truncation.
});
test('BLE discovery exposes unnamed devices and prioritizes heart-rate broadcasts', () => {
  const list = [{ name: '', address: '01:02:03:04:05:06', rssi: -40, heart_rate: false }, { name: 'Chest strap', address: 'AA:BB:CC:DD:EE:FF', rssi: -80, heart_rate: true }];
  assert.match(ui.bleLabel(list[0]), /未提供名稱.*01:02:03:04:05:06.*-40/);
  assert.equal(ui.sortScanResults('ble', list)[0].name, 'Chest strap');
  assert.equal(list[0].name, ''); // Does not modify the source snapshot.
  assert.match(ui.wifiLabel({ ssid: '', secure: false, rssi: -55 }), /隱藏網路.*無密碼/);
});
test('timestamps show absence accurately and format uptime independently from sample age', () => {
  assert.equal(ui.ageText(null), '—'); assert.equal(ui.ageText(0), '剛剛'); assert.equal(ui.ageText(12000), '12 秒前');
  assert.equal(ui.uptimeText(90061000), '1 天 01:01:01');
});
test('device entry URLs accept IPv4 addresses only and cannot inject an arbitrary link', () => {
  assert.equal(ui.entryUrl('192.168.4.1'), 'http://192.168.4.1/');
  assert.equal(ui.entryUrl('192.0.2.20'), 'http://192.0.2.20/');
  assert.equal(ui.entryUrl('010.001.002.003'), 'http://10.1.2.3/');
  for (const value of [null, '', '0.0.0.0', '255.255.255.255', '300.1.2.3', 'javascript:alert(1)', '192.0.2.20/example', 'http://192.0.2.20/', '192.0.2.20:80']) assert.equal(ui.entryUrl(value), null);
});
test('slow polling is single-flight; stopping clears future requests and resume works', async () => {
  let resolveRequest, requests = 0, timerId = 0;
  const timers = new Map();
  const poller = ui.createPoller(() => { requests++; return new Promise(resolve => { resolveRequest = resolve; }); }, {
    setTimeout(fn) { const id = ++timerId; timers.set(id, fn); return id; }, clearTimeout(id) { timers.delete(id); }
  });
  poller.start(); poller.start();
  assert.equal(requests, 1); assert.equal(poller.inFlight, true); assert.equal(timers.size, 0);
  poller.stop(); poller.start(); // Visibility changes during a slow request must not duplicate it.
  assert.equal(requests, 1);
  resolveRequest(); await new Promise(resolve => setImmediate(resolve));
  assert.equal(timers.size, 1);
  const callback = [...timers.values()][0]; timers.clear(); callback();
  assert.equal(requests, 2);
  poller.stop(); resolveRequest(); await new Promise(resolve => setImmediate(resolve));
  assert.equal(timers.size, 0); assert.equal(poller.active, false);
  poller.start(); assert.equal(requests, 3);
  poller.stop(); resolveRequest();
});
test('failed polling schedules recovery instead of leaving the page permanently stale', async () => {
  let errors = 0; const scheduled = [];
  const poller = ui.createPoller(async () => { throw new Error('offline'); }, { onError() { errors++; }, setTimeout(fn) { scheduled.push(fn); return 1; }, clearTimeout() {} });
  poller.start(); await new Promise(resolve => setImmediate(resolve));
  assert.equal(errors, 1); assert.equal(scheduled.length, 1); assert.equal(poller.inFlight, false); poller.stop();
});
test('offline assets and safe rendering contain no external resource dependency or HTML injection sinks', () => {
  const html = fs.readFileSync(path.join(__dirname, '../web/index.html'), 'utf8');
  const js = fs.readFileSync(path.join(__dirname, '../web/app.js'), 'utf8');
  const css = fs.readFileSync(path.join(__dirname, '../web/style.css'), 'utf8');
  assert.doesNotMatch(html, /(?:src|href)=["']https?:\/\/(?!192\.168\.4\.1)/);
  assert.doesNotMatch(js, /\.innerHTML\s*=|insertAdjacentHTML|document\.write\(/);
  assert.doesNotMatch(css, /@import|url\(\s*["']?https?:/);
  assert.match(html, /lang="zh-Hant"/); assert.match(html, /name="viewport"/);
});

// A small DOM transport harness exercises the actual form handlers and polling code.
// No firmware or device-specific implementation is duplicated here.
function pageHarness() {
  const elements = new Map(), scheduled = new Map(), requests = [];
  let timerId = 0, saveReply = { status: 200, data: { ok: true, revision: 8, pending: false } };
  class Element {
    constructor(tag = 'div') {
      this.tagName = tag; this.children = []; this.events = {}; this.value = ''; this.textContent = ''; this.hidden = false;
      this.checked = false; this.disabled = false; this.type = 'text'; this.attributes = {};
      this.classList = { add() {}, toggle() {} };
    }
    append(...children) { this.children.push(...children); }
    replaceChildren(...children) { this.children = children; }
    addEventListener(event, callback) { this.events[event] = callback; }
    setAttribute(key, value) { this.attributes[key] = value; }
    removeAttribute(key) { delete this.attributes[key]; }
    focus() { this.focused = true; }
    get options() { return this.children; }
  }
  const document = {
    hidden: false, events: {}, createElement(tag) { return new Element(tag); },
    getElementById(id) { if (!elements.has(id)) elements.set(id, new Element()); return elements.get(id); },
    addEventListener(event, callback) { this.events[event] = callback; }
  };
  const snapshot = {
    firmware: 'test', pending: false, ble: { bpm: 82, connected: true, ready: true, age_ms: 1 },
    machine: { rpm: 50, reported_level: 7, target_level: 9, sent_level: 8, machine_bpm: 77, uart_online: true, age_ms: 1 },
    ap: { ip: '192.168.4.1' }, tcp: { connected: true, server: 'test.invalid', port: 9999 }, wifi: { ssid: 'active-network', state: 'connected', ip: '192.0.2.20' }
  };
  const window = {
    document, addEventListener() {}, confirm() { return true; },
    async fetch(url, options) {
      requests.push({ url, options });
      let status = 200, data;
      if (url === '/api/status') data = snapshot;
      else if (url === '/api/config' && options.method === 'POST') ({ status, data } = saveReply);
      else if (url === '/api/config') data = { revision: 7, pending: false, settings: { wifi_ssid: 'saved-network', has_password: true, socket_server: '192.168.0.100', heart_rate_address: '' } };
      else data = { state: 'done', results: [] };
      return { ok: status >= 200 && status < 300, status, async json() { return data; } };
    }
  };
  vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../web/app.js'), 'utf8'), {
    window, TextEncoder, AbortController, setTimeout(callback, delay) { const id = ++timerId; scheduled.set(id, { callback, delay }); return id; }, clearTimeout(id) { scheduled.delete(id); }
  });
  return {
    elements, scheduled, requests, snapshot, document,
    setSaveReply(status, data) { saveReply = { status, data }; },
    async settle() { await new Promise(resolve => setImmediate(resolve)); },
    async poll() {
      const entry = [...scheduled.entries()].find(([, timer]) => timer.delay === 1000);
      assert.ok(entry, 'status poll must be scheduled'); scheduled.delete(entry[0]); await entry[1].callback();
    },
    edit(id, value) { elements.get(id).value = value; elements.get('configForm').events.input(); },
    async save() { await elements.get('configForm').events.submit({ preventDefault() {} }); }
  };
}

test('actual status rendering keeps BLE/machine values distinct and never overwrites form edits', async () => {
  const page = pageHarness(); await page.settle();
  assert.equal(page.elements.get('wifiSsid').value, 'saved-network');
  assert.equal(page.elements.get('metricBpm').textContent, '82');
  assert.equal(page.elements.get('metricRpm').textContent, '50');
  assert.equal(page.elements.get('metricLevel').textContent, '7');
  page.edit('wifiSsid', 'unsaved-network'); page.edit('socketServer', 'new-server.local');
  page.snapshot.ble.bpm = 300; page.snapshot.machine.reported_level = 6;
  await page.poll();
  assert.equal(page.elements.get('metricBpm').textContent, '300');
  assert.equal(page.elements.get('metricLevel').textContent, '6');
  assert.equal(page.elements.get('wifiSsid').value, 'unsaved-network');
  assert.equal(page.elements.get('socketServer').value, 'new-server.local');
  assert.equal(page.elements.get('unsavedBadge').hidden, false);
});
test('409 conflicts retain edited credentials and the original revision without blind overwrite', async () => {
  const page = pageHarness(); await page.settle();
  page.edit('wifiSsid', 'my-new-network'); page.edit('wifiPassword', 'new-password');
  page.elements.get('changePassword').checked = true;
  page.setSaveReply(409, { error: 'revision conflict', revision: 8 });
  await page.save();
  const post = page.requests.find(request => request.options.method === 'POST');
  assert.equal(JSON.parse(post.options.body).revision, 7);
  assert.equal(page.elements.get('wifiSsid').value, 'my-new-network');
  assert.equal(page.elements.get('wifiPassword').value, 'new-password');
  assert.match(page.elements.get('configMessage').textContent, /輸入仍保留/);
  assert.equal(page.elements.get('unsavedBadge').hidden, false);
  await page.save();
  const posts = page.requests.filter(request => request.options.method === 'POST');
  assert.equal(JSON.parse(posts[1].options.body).revision, 7);
});
test('UART heartbeats cannot refresh stale 0x28 values or hide the separate 0x3F age', async () => {
  const page = pageHarness(); await page.settle();
  page.snapshot.machine.age_ms = 0;
  page.snapshot.machine.age28_ms = 5000;
  page.snapshot.machine.age3f_ms = 100;
  await page.poll();
  assert.match(page.elements.get('rpmBadge').textContent, /過期/);
  assert.match(page.elements.get('levelBadge').textContent, /有效/);
  page.snapshot.machine.age3f_ms = 8000;
  await page.poll();
  assert.match(page.elements.get('levelBadge').textContent, /過期/);
});
test('successful save omits unchanged password and clears only the submitted edits', async () => {
  const page = pageHarness(); await page.settle();
  page.edit('socketServer', 'new-server.local'); await page.save();
  const post = page.requests.find(request => request.options.method === 'POST');
  assert.equal(Object.hasOwn(JSON.parse(post.options.body), 'wifi_password'), false);
  assert.equal(page.elements.get('unsavedBadge').hidden, true);
  assert.equal(page.elements.get('socketServer').value, 'new-server.local');
  assert.match(page.elements.get('configMessage').textContent, /已保存/);
});
test('queue and write diagnostics expose each transport stage independently', async () => {
  const page = pageHarness(); await page.settle();
  Object.assign(page.snapshot.tcp, { writing: true, awaiting_ack: false, queued_events: 3, queued_replies: 2, telemetry_pending: true });
  Object.assign(page.snapshot.machine, { state: 'Start', state_age_ms: 7000, age_ms: 0, control_state: 'End', level_queued: true, level_ack_pending: false, queued_controls: 1, control_ack_queued: true, queued_uart_replies: 4 });
  await page.poll();
  const rows = new Map();
  for (const group of page.elements.get('diagnosticGroups').children) {
    for (const row of group.children[1].children) rows.set(row.children[0].textContent, row.children[1].textContent);
  }
  assert.equal(rows.get('正在寫入 TCP'), '是');
  assert.equal(rows.get('等待事件回覆（非 0x23）'), '否');
  assert.equal(rows.get('待送事件數'), '3');
  assert.equal(rows.get('待送優先回覆數'), '2');
  assert.equal(rows.get('有尚未送出的最新讀值'), '是');
  assert.equal(rows.get('阻力目標等待送出'), '是');
  assert.equal(rows.get('阻力指令等待 ACK'), '否');
  assert.equal(rows.get('待執行的啟停命令數'), '1');
  assert.equal(rows.get('啟停回覆等待交給 TCP'), '是');
  assert.equal(rows.get('待送 UART 回覆數'), '4');
  assert.equal(rows.get('機台回報狀態'), '運動中（過期）');
  assert.equal(rows.get('本機控制判斷'), '已停止');
  assert.match(page.elements.get('machineState').textContent, /過期/);
  page.snapshot.machine.state = null; page.snapshot.machine.state_age_ms = null;
  await page.poll();
  assert.equal(page.elements.get('machineState').textContent, '機台尚未回報狀態');
});
test('hidden pages stop status polling and refresh after becoming visible', async () => {
  const page = pageHarness(); await page.settle();
  page.document.hidden = true; page.document.events.visibilitychange();
  assert.equal([...page.scheduled.values()].filter(timer => timer.delay === 1000).length, 0);
  const count = page.requests.filter(request => request.url === '/api/status').length;
  page.document.hidden = false; page.document.events.visibilitychange(); await page.settle();
  assert.equal(page.requests.filter(request => request.url === '/api/status').length, count + 1);
});
test('AP and LAN entries follow live connectivity without overwriting unsaved settings', async () => {
  const page = pageHarness(); await page.settle();
  assert.equal(page.elements.get('apEntry').attributes.href, 'http://192.168.4.1/');
  assert.equal(page.elements.get('lanEntry').attributes.href, 'http://192.0.2.20/');
  page.edit('wifiSsid', 'unsaved-network');
  page.snapshot.wifi.state = 'retrying';
  await page.poll();
  assert.equal(page.elements.get('lanEntry').textContent, '—');
  assert.equal(Object.hasOwn(page.elements.get('lanEntry').attributes, 'href'), false);
  assert.equal(page.elements.get('apEntry').attributes.href, 'http://192.168.4.1/');
  page.snapshot.wifi.state = 'connected'; page.snapshot.wifi.ip = '';
  await page.poll();
  assert.equal(page.elements.get('lanEntry').textContent, '—');
  page.snapshot.wifi.ip = '192.0.2.21';
  await page.poll();
  assert.equal(page.elements.get('lanEntry').attributes.href, 'http://192.0.2.21/');
  assert.equal(page.elements.get('wifiSsid').value, 'unsaved-network');
});
