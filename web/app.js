/* Offline control panel. Device-provided text is always assigned via textContent. */
(function (root) {
  'use strict';

  const isKnown = value => value !== null && value !== undefined && value !== '';
  const display = value => !isKnown(value) ? '—' : typeof value === 'boolean' ? (value ? '是' : '否') : typeof value === 'object' ? JSON.stringify(value) : String(value);
  const get = (object, path) => path.split('.').reduce((value, key) => value == null ? undefined : value[key], object);
  function entryUrl(ip) {
    if (typeof ip !== 'string' || !/^\d{1,3}(?:\.\d{1,3}){3}$/.test(ip)) return null;
    const parts = ip.split('.').map(Number);
    if (parts.some(part => part > 255) || parts.every(part => part === 0) || parts.every(part => part === 255)) return null;
    return `http://${parts.join('.')}/`;
  }
  const byteLength = value => new TextEncoder().encode(value).length;
  const words = {
    idle: '待機', start: '運動中', running: '運動中', pause: '暫停', paused: '暫停', end: '已停止', stopped: '已停止',
    connecting: '連線中', connected: '已連線', disconnected: '未連線', disabled: '未啟用', ready: '已就緒',
    scanning: '搜尋中', queued: '已排入搜尋', subscribing: '訂閱中', waiting: '等待中', error: '發生錯誤',
    absent: '尚未連接', unavailable: '無法使用', initializing: '初始化中', offline: '離線', online: '已連線',
    done: '搜尋完成', public: 'Public', random: 'Random', public_identity: 'Public identity', random_identity: 'Random identity', none: '無', high: 'HIGH', low: 'LOW',
    unconfigured: '尚未設定', retrying: '等待重試', starting: '啟動中', disconnecting: '正在中斷連線', discovering: '探索服務中',
    reader_error: '讀卡機錯誤', unsupported_reader: '不支援的讀卡機', checking_reader: '檢查讀卡機中',
    reader_disconnected: '讀卡機已拔除', recovering: '恢復連線中', waiting_reader: '等待連接讀卡機',
    ready_no_card: '已就緒，等待卡片', card_read: '已讀取卡片', stop_settling: '停止脈衝後等待',
    sent: '已送出', acknowledged: '已收到回覆', timeout: '逾時', no_card: '未偵測到卡片', card_present: '已偵測到卡片'
  };
  const stateWord = value => !isKnown(value) ? '—' : (words[String(value).toLowerCase()] || display(value));
  function ageText(value) {
    if (value === null || value === undefined || !Number.isFinite(Number(value)) || Number(value) < 0) return '—';
    const seconds = Math.floor(Number(value) / 1000);
    if (seconds < 1) return '剛剛';
    if (seconds < 60) return `${seconds} 秒前`;
    if (seconds < 3600) return `${Math.floor(seconds / 60)} 分 ${seconds % 60} 秒前`;
    return `${Math.floor(seconds / 3600)} 小時 ${Math.floor(seconds % 3600 / 60)} 分前`;
  }
  function uptimeText(value) {
    if (!isKnown(value) || !Number.isFinite(Number(value))) return '—';
    let seconds = Math.floor(Number(value) / 1000);
    const days = Math.floor(seconds / 86400);
    seconds %= 86400;
    const hh = String(Math.floor(seconds / 3600)).padStart(2, '0');
    const mm = String(Math.floor(seconds % 3600 / 60)).padStart(2, '0');
    const ss = String(seconds % 60).padStart(2, '0');
    return `${days ? days + ' 天 ' : ''}${hh}:${mm}:${ss}`;
  }
  function sampleState(value, online, ageMs, maxAge = 10000) {
    if (!isKnown(value)) return 'missing';
    if (!online || ageMs === null || ageMs === undefined || !Number.isFinite(Number(ageMs)) || Number(ageMs) < 0 || Number(ageMs) >= maxAge) return 'stale';
    return 'fresh';
  }
  function validationError(field, message) {
    const error = new Error(message);
    error.field = field;
    return error;
  }
  function buildConfigPayload(values, revision) {
    const wifiSsid = String(values.wifi_ssid || ''); // SSID whitespace is significant.
    const host = String(values.socket_server || '').trim();
    const address = String(values.heart_rate_address || '').trim().toUpperCase();
    if (wifiSsid.includes('\0') || byteLength(wifiSsid) > 32) throw validationError('wifiSsid', 'Wi-Fi 名稱不可包含空字元或超過 32 個位元組；中文字通常佔 3 個位元組。');
    if (!host || host.length > 253 || !/^[a-zA-Z0-9](?:[a-zA-Z0-9.-]*[a-zA-Z0-9])?$/.test(host) || host.split('.').some(label => !label || label.length > 63 || label.startsWith('-') || label.endsWith('-'))) {
      throw validationError('socketServer', '請填入有效的 IPv4 或主機名稱，不含通訊協定、路徑或連接埠。');
    }
    if (/^[0-9.]+$/.test(host) && (!/^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$/.test(host) || host.split('.').some(part => Number(part) > 255))) throw validationError('socketServer', 'IPv4 必須是四組 0–255 的數字。');
    if (address && !/^(?:[0-9A-F]{2}:){5}[0-9A-F]{2}$/.test(address)) throw validationError('heartAddress', '請使用 AA:BB:CC:DD:EE:FF 格式輸入藍牙 MAC，或留白停用。');
    if (!Number.isInteger(revision) || revision < 1 || revision > 0xFFFFFFFF) throw validationError('reloadConfig', '尚未取得有效設定版本，請重新讀取設定。');
    const payload = { revision, wifi_ssid: wifiSsid, socket_server: host, heart_rate_address: address };
    if (values.change_password) {
      if (values.password_mode === 'open') payload.wifi_password = '';
      else {
        const password = String(values.wifi_password || '');
        const bytes = byteLength(password);
        if (password.includes('\0') || !((bytes >= 8 && bytes <= 63) || /^[0-9a-fA-F]{64}$/.test(password))) throw validationError('wifiPassword', '請輸入 8–63 個位元組的密碼，或 64 位十六進位金鑰，且不可包含空字元。無密碼網路請明確選擇「無密碼網路」。');
        payload.wifi_password = password;
      }
    }
    return payload;
  }
  function wifiLabel(item) {
    return `${item.ssid || '（隱藏網路，請手動輸入）'} · ${display(item.rssi)} dBm · ${item.secure ? '加密' : '無密碼'}`;
  }
  function bleLabel(item) {
    return `${item.name || '未提供名稱'} · ${display(item.address)} · ${display(item.rssi)} dBm${item.heart_rate ? ' · 心率服務' : ''}`;
  }
  function sortScanResults(kind, items) {
    const list = Array.isArray(items) ? items.slice(0, 64) : [];
    const bySignal = (a, b) => (Number.isFinite(b.rssi) ? b.rssi : -999) - (Number.isFinite(a.rssi) ? a.rssi : -999);
    return list.sort((a, b) => kind === 'ble' ? Number(Boolean(b.heart_rate)) - Number(Boolean(a.heart_rate)) || bySignal(a, b) : bySignal(a, b));
  }
  // Scheduling happens after the request settles. A slow request cannot overlap another one.
  function createPoller(task, options = {}) {
    const schedule = options.setTimeout || setTimeout;
    const cancelTimer = options.clearTimeout || clearTimeout;
    const interval = options.interval || 1000;
    let active = false, inFlight = false, timer = null;
    async function run() {
      if (!active || inFlight) return;
      timer = null;
      inFlight = true;
      try { await task(); } catch (error) { if (options.onError) options.onError(error); }
      finally {
        inFlight = false;
        if (active) timer = schedule(run, interval);
      }
    }
    return {
      start() { if (active) return; active = true; if (!inFlight) void run(); },
      stop() { active = false; if (timer !== null) cancelTimer(timer); timer = null; if (options.onStop) options.onStop(); },
      get active() { return active; },
      get inFlight() { return inFlight; }
    };
  }
  const api = { isKnown, display, get, entryUrl, stateWord, ageText, uptimeText, sampleState, buildConfigPayload, wifiLabel, bleLabel, sortScanResults, createPoller };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  if (!root || !root.document) return;

  const document = root.document;
  const el = id => document.getElementById(id);
  const text = (id, value) => { el(id).textContent = value; };
  const units = unit => value => isKnown(value) ? `${display(value)} ${unit}` : '—';
  const hexValue = width => value => isKnown(value) ? `0x${Number(value).toString(16).toUpperCase().padStart(width, '0')}` : '—';
  const boolState = (yes, no) => value => value == null ? '—' : value ? yes : no;
  const fields = [];
  let latestStatus = null, revision = null, dirty = false, editVersion = 0, hasPassword = false;
  let configLoading = false, configSaving = false, statusController = null;
  const scanJobs = new Map();
  const machineSpecs = [
    ['machine.state', '機台回報狀態', stateWord, 'machineState'], ['machine.state_age_ms', '狀態回報距今', ageText], ['machine.tracking', '數值追蹤', boolState('追蹤中', '未追蹤')],
    ['machine.age_ms', '最後有效回報', ageText], ['machine.age28_ms', '0x28 數值回報距今', ageText], ['machine.age3f_ms', '0x3F 數值回報距今', ageText],
    ['machine.machine_bpm', '機台回報心率', units('BPM'), 'machine28'],
    ['machine.rpm', '轉速', units('RPM'), 'machine28'], ['machine.minutes', '運動時間 · 分', display, 'machine28'],
    ['machine.seconds', '運動時間 · 秒', display, 'machine28'], ['machine.distance_raw', '距離 · 原始值', display, 'machine28'],
    ['machine.calories_raw', '熱量 · 原始值', display, 'machine28'], ['machine.watt28_raw', '功率 0x28 · 原始值', display, 'machine28'],
    ['machine.watt3f_raw', '功率 0x3F · 原始值', display, 'machine3f'], ['machine.torque_raw', '扭力 · 原始值', display, 'machine3f'],
    ['machine.target_level', '阻力 · 主機要求'], ['machine.sent_level', '阻力 · 已送往機台'],
    ['machine.reported_level', '阻力 · 機台回報', display, 'machine3f'], ['machine.level_queued', '阻力目標等待送出'], ['machine.level_ack_pending', '阻力指令等待 UART ACK'], ['machine.pulse', '啟停 GPIO 脈衝', stateWord]
  ];
  const diagnosticSpecs = [
    ['設定熱點與裝置', [
      ['ap.ssid', '設定 Wi-Fi 名稱'], ['ap.ip', '設定入口 IP'], ['ap.clients', '已連線手機／用戶端數'],
      ['firmware', '韌體版本'], ['uptime_ms', '開機運行時間', uptimeText], ['pending', '有設定等待套用'],
      ['health.free_heap', '可用 Heap', units('bytes')], ['health.min_free_heap', '最低可用 Heap', units('bytes')]
    ]],
    ['目標 Wi-Fi', [
      ['wifi.state', '連線狀態', stateWord], ['wifi.ssid', '目前目標 SSID'], ['wifi.ip', 'STA IP'],
      ['wifi.rssi', 'Wi-Fi 訊號', units('dBm')], ['wifi.last_error', '最近連線錯誤']
    ]],
    ['BLE 心率感測器', [
      ['ble.state', '工作狀態', stateWord], ['ble.connected', 'BLE 已連線'], ['ble.subscribed', '2A37 通知已訂閱'],
      ['ble.ready', '已連續收到三筆有效通知'], ['ble.name', '裝置名稱'], ['ble.address', 'MAC 位址'],
      ['ble.address_type', '位址類型', stateWord], ['ble.rssi', '訊號', units('dBm')],
      ['ble.bpm', 'BLE 心率', units('BPM'), 'ble'], ['ble.battery', '電池電量', units('%'), 'ble'],
      ['ble.notifications', '累計有效通知數'], ['ble.consecutive', '連續有效通知數'], ['ble.age_ms', '最後有效通知', ageText],
      ['ble.disconnects', '斷線次數'], ['ble.last_error', '最近錯誤'], ['ble.last_notification_hex', '最後 BLE 通知 HEX', display, null, 'hex']
    ]],
    ['TCP 連線與狀態查詢', [
      ['tcp.connected', 'TCP 已連線'], ['tcp.server', '目的主機'], ['tcp.port', '目的連接埠'], ['tcp.generation', '目前連線世代'], ['tcp.reconnects', '建立連線次數（含首次）'],
      ['tcp.writing', '正在寫入 TCP'], ['tcp.awaiting_ack', '等待事件回覆（非 0x23）'],
      ['tcp.queued_events', '待送事件數'], ['tcp.queued_replies', '待送優先回覆數'], ['tcp.telemetry_pending', '有尚未送出的最新讀值'],
      ['tcp.last_query', '最後狀態查詢', display, null, 'hex'], ['tcp.last_reply', '實際送出的查詢回覆', display, null, 'hex'],
      ['tcp.card_result', '最近刷卡回報結果', stateWord], ['tcp.checksum_errors', 'Checksum 錯誤'],
      ['tcp.framing_errors', '封包格式錯誤'], ['tcp.timeouts', '等待回覆逾時'], ['tcp.last_error', '最近錯誤'],
      ['tcp.rx_age_ms', '最後收到封包', ageText], ['tcp.tx_age_ms', '最後送出封包', ageText],
      ['tcp.rx_hex', '最後 TCP RX HEX', display, null, 'hex'], ['tcp.tx_hex', '最後 TCP TX HEX', display, null, 'hex']
    ]],
    ['最後寫入 TCP 的數值 · 非 ACK', [
      ['tcp.telemetry.bpm.value', '已送出的 BLE 心率', units('BPM')], ['tcp.telemetry.bpm.age_ms', '心率送出距今', ageText],
      ['tcp.telemetry.rpm.value', '已送出的轉速', units('RPM')], ['tcp.telemetry.rpm.age_ms', '轉速送出距今', ageText],
      ['tcp.telemetry.level.value', '已送出的阻力'], ['tcp.telemetry.level.age_ms', '阻力送出距今', ageText]
    ]],
    ['機台 UART 與 GPIO', [
      ['machine.uart_online', 'UART 有有效回報'], ['machine.state', '機台回報狀態', stateWord, 'machineState'], ['machine.state_age_ms', '狀態回報距今', ageText],
      ['machine.control_state', '本機控制判斷', stateWord], ['machine.tracking', '數值追蹤已啟用'],
      ['machine.age_ms', '最後有效回報', ageText], ['machine.age28_ms', '0x28 回報距今', ageText], ['machine.age3f_ms', '0x3F 回報距今', ageText], ['machine.pulse', 'GPIO 脈衝階段', stateWord],
      ['machine.level_queued', '阻力目標等待送出'], ['machine.level_ack_pending', '阻力指令等待 ACK'],
      ['machine.queued_controls', '待執行的啟停命令數'], ['machine.control_ack_queued', '啟停回覆等待交給 TCP'],
      ['machine.queued_uart_replies', '待送 UART 回覆數'], ['machine.last_error', '最近機台通訊錯誤'],
      ['machine.checksum_errors', 'Checksum 錯誤'], ['machine.framing_errors', '封包格式錯誤'], ['machine.timeouts', 'UART 逾時'],
      ['machine.rx_hex', '最後 UART RX HEX', display, null, 'hex'], ['machine.tx_hex', '最後 UART TX HEX', display, null, 'hex']
    ]],
    ['USB／CCID 讀卡機', [
      ['rfid.state', '工作狀態', stateWord], ['rfid.ready', '讀卡機已就緒'], ['rfid.card_present', '卡片在場'],
      ['rfid.vid', 'USB VID', hexValue(4)], ['rfid.pid', 'USB PID', hexValue(4)], ['rfid.uid', '最近 UID（保留前導零）', display, null, 'hex'],
      ['rfid.age_ms', '最後更新距今', ageText], ['rfid.cards', '累計讀卡次數'], ['rfid.errors', '累計錯誤數'], ['rfid.last_error', '最近錯誤'],
      ['rfid.features', 'CCID 功能旗標', hexValue(8)], ['rfid.max_message_length', 'CCID 最大訊息長度', units('bytes')],
      ['rfid.last_sw', '最後 APDU 狀態字', hexValue(4)], ['rfid.atr', 'ATR HEX', display, null, 'hex'],
      ['rfid.last_tx', '最後 USB CCID TX HEX', display, null, 'hex'], ['rfid.last_rx', '最後 USB CCID RX HEX', display, null, 'hex']
    ]]
  ];
  function addRows(container, specs) {
    specs.forEach(([path, label, format = display, source = null, style = null]) => {
      const row = document.createElement('div'); row.className = 'data-row' + (style === 'hex' ? ' wide' : '');
      const term = document.createElement('dt'); term.textContent = label;
      const value = document.createElement('dd'); value.textContent = '—'; if (style === 'hex') value.classList.add('mono');
      row.append(term, value); container.append(row); fields.push({ path, value, format, source });
    });
  }
  addRows(el('machineReadings'), machineSpecs);
  diagnosticSpecs.forEach(([heading, specs]) => {
    const section = document.createElement('section'); section.className = 'diagnostic-group';
    const title = document.createElement('h3'); title.textContent = heading;
    const list = document.createElement('dl'); list.className = 'data-list';
    section.append(title, list); el('diagnosticGroups').append(section); addRows(list, specs);
  });
  function setBadge(id, label, variant = 'neutral') { text(id, label); el(id).className = `badge ${variant}`; }
  function setEntry(id, ip, available) {
    const link = el(id), url = available ? entryUrl(ip) : null;
    link.textContent = url || '—';
    if (url) { link.setAttribute('href', url); link.removeAttribute('aria-disabled'); }
    else { link.removeAttribute('href'); link.setAttribute('aria-disabled', 'true'); }
  }
  function message(label, variant = '') { text('configMessage', label); el('configMessage').className = `form-message ${variant}`; }
  function markDirty() { dirty = true; editVersion++; el('unsavedBadge').hidden = false; }
  function renderMetric(valueId, badgeId, sourceId, value, online, ageMs, source, maxAge = 10000) {
    const state = sampleState(value, online, ageMs, maxAge);
    text(valueId, display(value));
    text(badgeId, state === 'missing' ? '尚未收到有效值' : state === 'stale' ? '資料已失效／過期' : '即時有效讀值');
    el(badgeId).className = `metric-note${state === 'stale' ? ' stale' : ''}`;
    text(sourceId, `${source}${isKnown(ageMs) ? ' · ' + ageText(ageMs) : ''}`);
  }
  function renderStatus(status, reachable = true) {
    latestStatus = status;
    const ble = status.ble || {}, machine = status.machine || {}, wifi = status.wifi || {}, tcp = status.tcp || {}, rfid = status.rfid || {}, ap = status.ap || {};
    const bleOnline = reachable && ble.connected && ble.ready;
    const uartOnline = reachable && machine.uart_online;
    const age28 = Object.prototype.hasOwnProperty.call(machine, 'age28_ms') ? machine.age28_ms : machine.age_ms;
    const age3f = Object.prototype.hasOwnProperty.call(machine, 'age3f_ms') ? machine.age3f_ms : machine.age_ms;
    renderMetric('metricBpm', 'bpmBadge', 'bpmSource', ble.bpm, bleOnline, ble.age_ms, 'BLE');
    renderMetric('metricRpm', 'rpmBadge', 'rpmSource', machine.rpm, uartOnline, age28, 'UART 0x28', 5000);
    renderMetric('metricLevel', 'levelBadge', 'levelSource', machine.reported_level, uartOnline, age3f, 'UART 0x3F', 5000);
    const stateFreshness = sampleState(machine.state, uartOnline, machine.state_age_ms, 5000);
    setBadge('machineState', !isKnown(machine.state) ? '機台尚未回報狀態' : `機台 ${stateWord(machine.state)}${stateFreshness === 'stale' ? '（過期）' : ''}`, stateFreshness === 'fresh' ? 'neutral' : 'warning');
    setBadge('uartBadge', !reachable ? '連線中斷' : !isKnown(machine.age_ms) ? '尚未收到' : sampleState(0, uartOnline, machine.age_ms) === 'fresh' ? '持續回報' : '資料已過期', sampleState(0, uartOnline, machine.age_ms) === 'fresh' ? 'good' : 'warning');
    text('wifiSummary', display(wifi.ssid));
    text('wifiDetail', reachable ? `${stateWord(wifi.state)}${wifi.ip ? ' · ' + wifi.ip : ''}` : '頁面與裝置連線中斷');
    setEntry('apEntry', ap.ip, reachable);
    setEntry('lanEntry', wifi.ip, reachable && wifi.state === 'connected');
    text('tcpSummary', `${display(tcp.server)}:${display(tcp.port)}`);
    text('tcpDetail', !reachable ? '資料已失效' : tcp.connected ? '已連線 · TCP client' : '尚未連線');
    text('bleSummary', ble.name || ble.address || '尚未設定');
    text('bleDetail', !reachable ? '資料已失效' : `${stateWord(ble.state)}${ble.ready ? ' · 通知已就緒' : ''}`);
    text('rfidSummary', !reachable ? '資料已失效' : rfid.ready ? '讀卡機已就緒' : stateWord(rfid.state));
    text('rfidDetail', rfid.card_present ? '已偵測到卡片' : '目前無卡片');
    el('pendingNotice').hidden = !status.pending;
    text('apFootnote', ap.ssid ? `${ap.ssid} · ${display(ap.clients)} 台已連線` : '設定熱點保持開啟');
    text('firmwareVersion', `韌體 ${display(status.firmware)}`);
    fields.forEach(field => {
      const value = get(status, field.path);
      let stale = !reachable && isKnown(value);
      if (field.source === 'machine28') stale ||= sampleState(value, uartOnline, age28, 5000) === 'stale';
      if (field.source === 'machine3f') stale ||= sampleState(value, uartOnline, age3f, 5000) === 'stale';
      if (field.source === 'machineState') stale ||= sampleState(value, uartOnline, machine.state_age_ms, 5000) === 'stale';
      if (field.source === 'ble') stale ||= sampleState(value, reachable && ble.connected, ble.age_ms) === 'stale';
      field.value.textContent = field.format(value) + (stale ? '（過期）' : '');
      field.value.classList.toggle('stale', stale);
    });
  }
  async function fetchJson(path, options = {}) {
    const response = await root.fetch(path, { cache: 'no-store', credentials: 'omit', ...options });
    let data;
    try { data = await response.json(); } catch (_) { throw new Error('裝置回傳了無法解析的資料，請確認仍連接設定 Wi-Fi 或與裝置位於同一區域網路。'); }
    if (!response.ok) { const error = new Error(data.error || `裝置請求失敗（HTTP ${response.status}）`); error.status = response.status; error.data = data; throw error; }
    return data;
  }
  async function timedJson(path, options = {}) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 10000);
    try { return await fetchJson(path, { ...options, signal: controller.signal }); }
    finally { clearTimeout(timer); }
  }
  function updatePasswordEditor() {
    const editing = el('changePassword').checked, open = el('modeOpen').checked;
    el('passwordEditor').hidden = !editing;
    el('wifiPassword').disabled = !editing || open;
    el('togglePassword').disabled = !editing || open;
    text('passwordHelp', open ? '儲存時將清除已儲存密碼，並使用無密碼 Wi-Fi 連線。' : '輸入 8–63 個位元組，或 64 位十六進位金鑰。');
  }
  async function loadConfig(askBeforeDiscard = false) {
    if (configLoading || configSaving) return;
    if (askBeforeDiscard && dirty && !root.confirm('重新載入會捨棄尚未儲存的修改，確定要重新載入嗎？')) return;
    configLoading = true; el('configFields').disabled = true; el('retryConfig').hidden = true;
    message('正在讀取裝置設定…');
    try {
      const config = await timedJson('/api/config');
      if (!Number.isInteger(config.revision) || !config.settings) throw new Error('設定資料不完整，請重試。');
      revision = config.revision; hasPassword = Boolean(config.settings.has_password);
      el('wifiSsid').value = config.settings.wifi_ssid || '';
      el('socketServer').value = config.settings.socket_server || '';
      el('heartAddress').value = config.settings.heart_rate_address || '';
      el('changePassword').checked = false; el('modeSecured').checked = true; el('wifiPassword').value = '';
      el('wifiPassword').type = 'password'; el('togglePassword').setAttribute('aria-pressed', 'false'); text('togglePassword', '顯示');
      el('togglePassword').setAttribute('aria-label', '顯示密碼');
      text('passwordStored', hasPassword ? '裝置已有密碼' : '目前未儲存密碼'); updatePasswordEditor();
      dirty = false; editVersion++; el('unsavedBadge').hidden = true;
      if (config.storage_ok === false) message('已讀取目前設定，但裝置的 NVS 儲存服務無法使用；修改暫時無法保存。', 'error');
      else message(config.pending ? '已載入已保存的設定；等待運動停止後套用。' : '已載入裝置設定。');
      el('pendingNotice').hidden = !config.pending; el('configFields').disabled = false;
    } catch (error) {
      message(error.name === 'AbortError' ? '讀取設定逾時，請確認仍連接設定 Wi-Fi，或位於同一區網且 IP 未變更。' : error.message, 'error');
      el('retryConfig').hidden = false;
      // Existing user edits stay available after a failed reload.
      el('configFields').disabled = revision === null;
    } finally { configLoading = false; }
  }
  async function saveConfig(event) {
    event.preventDefault(); if (configSaving || configLoading) return;
    let payload;
    try {
      payload = buildConfigPayload({
        wifi_ssid: el('wifiSsid').value, socket_server: el('socketServer').value, heart_rate_address: el('heartAddress').value,
        change_password: el('changePassword').checked, password_mode: el('modeOpen').checked ? 'open' : 'secured', wifi_password: el('wifiPassword').value
      }, revision);
    } catch (error) { message(error.message, 'error'); if (error.field) el(error.field).focus(); return; }
    const submittedEditVersion = editVersion;
    configSaving = true; el('saveConfig').disabled = true; el('reloadConfig').disabled = true; message('正在儲存設定…');
    try {
      const result = await timedJson('/api/config', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) });
      if (!result.ok || !Number.isInteger(result.revision)) throw new Error(result.error || '未能確認儲存結果，請重新載入設定確認。');
      revision = result.revision;
      if (Object.prototype.hasOwnProperty.call(payload, 'wifi_password')) hasPassword = payload.wifi_password !== '';
      text('passwordStored', hasPassword ? '裝置已有密碼' : '目前未儲存密碼');
      if (editVersion === submittedEditVersion) {
        dirty = false; el('unsavedBadge').hidden = true; el('wifiPassword').value = ''; el('changePassword').checked = false; updatePasswordEditor();
      }
      el('pendingNotice').hidden = !result.pending;
      message((result.pending ? '設定已保存，待運動停止後套用。' : '設定已保存，裝置正在套用連線設定。') + (dirty ? ' 您剛才的新修改仍未儲存。' : ''), 'success');
    } catch (error) {
      const label = error.status === 409 ? '設定已被其他頁面修改，您的輸入仍保留。請按「重新載入」，確認最新設定後再儲存。' : error.name === 'AbortError' ? '儲存回覆逾時，結果尚未確認。輸入仍保留；請重新連線後載入設定確認。' : error.message;
      message(label, 'error');
    } finally { configSaving = false; el('saveConfig').disabled = false; el('reloadConfig').disabled = false; }
  }
  function scanElements(kind) { return kind === 'wifi' ? { button: el('scanWifi'), state: el('wifiScanState'), select: el('wifiResults') } : { button: el('scanBle'), state: el('bleScanState'), select: el('bleResults') }; }
  function populateScan(kind, results) {
    const { select } = scanElements(kind); const oldValue = select.value;
    select.replaceChildren();
    const placeholder = document.createElement('option'); placeholder.value = ''; placeholder.textContent = kind === 'wifi' ? '選擇搜尋到的網路…' : '選擇搜尋到的裝置…'; select.append(placeholder);
    sortScanResults(kind, results).forEach(item => {
      const option = document.createElement('option');
      option.value = kind === 'wifi' ? item.ssid || '' : item.address || '';
      option.textContent = kind === 'wifi' ? wifiLabel(item) : bleLabel(item);
      option.disabled = !option.value;
      select.append(option);
    });
    if (Array.from(select.options).some(option => option.value === oldValue)) select.value = oldValue;
  }
  function finishScan(kind, job, error) {
    job.poller.stop(); scanJobs.delete(kind);
    const { button, state } = scanElements(kind); button.disabled = false;
    if (error) state.textContent = error;
  }
  async function beginScan(kind) {
    if (scanJobs.has(kind)) return;
    const ui = scanElements(kind); ui.button.disabled = true; ui.state.textContent = '正在請求搜尋…';
    const job = { started: Date.now(), poller: null };
    job.poller = createPoller(async () => {
      try {
        if (Date.now() - job.started > 60000) { finishScan(kind, job, '搜尋等待逾時，請稍後重新搜尋。'); return; }
        const result = await timedJson(`/api/scan/${kind}`);
        ui.state.textContent = result.state === 'queued' ? '已排入搜尋，正在等待另一項掃描完成…' : result.state === 'scanning' ? '正在搜尋附近裝置…' : stateWord(result.state);
        if (result.state === 'done') {
          populateScan(kind, result.results);
          const count = Array.isArray(result.results) ? result.results.length : 0;
          ui.state.textContent = count ? `找到 ${count} 個${kind === 'wifi' ? '網路' : '裝置'}，請從選單選擇。` : '沒有找到結果，可重新搜尋或手動輸入。';
          finishScan(kind, job);
        } else if (result.state === 'error') finishScan(kind, job, result.error || '搜尋失敗，請重試。');
      } catch (error) { finishScan(kind, job, error.name === 'AbortError' ? '搜尋請求逾時，請重試。' : error.message); }
    });
    scanJobs.set(kind, job);
    try {
      const result = await timedJson(`/api/scan/${kind}`, { method: 'POST' });
      if (result.state === 'error') { finishScan(kind, job, result.error || '無法開始搜尋。'); return; }
      if (!document.hidden) job.poller.start();
    } catch (error) { finishScan(kind, job, error.name === 'AbortError' ? '搜尋請求逾時，請重試。' : error.message); }
  }
  const poller = createPoller(async () => {
    statusController = new AbortController();
    const timer = setTimeout(() => statusController && statusController.abort(), 8000);
    try {
      const status = await fetchJson('/api/status', { signal: statusController.signal });
      if (document.hidden) return;
      renderStatus(status, true); setBadge('connectionBadge', '已連接設定頁', 'good');
      text('refreshState', '每秒更新 · 即時連線'); el('statusError').hidden = true;
    } catch (error) {
      if (document.hidden) return;
      setBadge('connectionBadge', '裝置連線中斷', 'bad'); text('refreshState', '自動重新連線中');
      text('statusError', '無法取得最新資料，保留的數值已標示過期。請確認仍連接設定 Wi-Fi 或同一區網；變更 Wi-Fi 後可能需要重新查詢裝置 IP。'); el('statusError').hidden = false;
      if (latestStatus) renderStatus(latestStatus, false);
    } finally { clearTimeout(timer); statusController = null; }
  }, { onStop() { if (statusController) statusController.abort(); } });

  el('configForm').addEventListener('submit', saveConfig);
  el('configForm').addEventListener('input', markDirty);
  el('changePassword').addEventListener('change', updatePasswordEditor);
  el('modeSecured').addEventListener('change', updatePasswordEditor);
  el('modeOpen').addEventListener('change', updatePasswordEditor);
  el('togglePassword').addEventListener('click', () => {
    const showing = el('wifiPassword').type === 'password'; el('wifiPassword').type = showing ? 'text' : 'password';
    text('togglePassword', showing ? '隱藏' : '顯示'); el('togglePassword').setAttribute('aria-label', showing ? '隱藏密碼' : '顯示密碼'); el('togglePassword').setAttribute('aria-pressed', String(showing));
  });
  el('reloadConfig').addEventListener('click', () => void loadConfig(true));
  el('retryConfig').addEventListener('click', () => void loadConfig(true));
  el('scanWifi').addEventListener('click', () => void beginScan('wifi'));
  el('scanBle').addEventListener('click', () => void beginScan('ble'));
  el('wifiResults').addEventListener('change', () => { if (el('wifiResults').value) { el('wifiSsid').value = el('wifiResults').value; markDirty(); } });
  el('bleResults').addEventListener('change', () => { if (el('bleResults').value) { el('heartAddress').value = el('bleResults').value; markDirty(); } });
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) { poller.stop(); scanJobs.forEach(job => job.poller.stop()); text('refreshState', '頁面已暫停更新'); }
    else { if (latestStatus) renderStatus(latestStatus, false); poller.start(); scanJobs.forEach(job => job.poller.start()); }
  });
  root.addEventListener('pagehide', () => { poller.stop(); scanJobs.forEach(job => job.poller.stop()); });
  root.addEventListener('pageshow', () => { if (!document.hidden) poller.start(); });
  void loadConfig();
  if (!document.hidden) poller.start();
})(typeof window !== 'undefined' ? window : null);
