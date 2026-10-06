(() => {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const WEB = Object.freeze({ staleAfterMs: 8000, offlineAfterMs: 30000,
    commandTimeoutMs: 10000, configTimeoutMs: 15000, ...window.MAYAP_WEB_CONFIG });

  let STORAGE = 'mayap.web.v10';
  let RUNTIME_CACHE = `${STORAGE}.runtime.v1`;
  const THEME_STORAGE = 'mayap.theme';
  const PROTOCOL_VERSION = 1;
  const DEVICE_ID_RE = /^MAP-[A-F0-9]{12}$/;
  const VENT_PROFILE_KEYS = Object.freeze([
    'ventAutoEnabled', 'ventProfileLevel', 'ventCycleMinutes',
    'ventDutyDay1To3', 'ventDutyDay4To7', 'ventDutyDay8To11',
    'ventDutyDay12To15', 'ventDutyDay16To18', 'ventDutyDay19To21'
  ]);
  const REQUIRED_CONFIG_KEYS = Object.freeze([
    'targetTemp', 'tempHysteresis', 'lowTempAlarm', 'highTempAlarm',
    'emergencyTemp', 'kp', 'ki', 'kd', 'lowHumidityAlarm', 'humidifierInstalled',
    'humidifierEnabled', 'targetHumidity', 'humidifierHysteresisRh', 'ventOnTemp',
    'ventOffTemp', 'ventScheduleEnabled', 'ventScheduleCount', 'ventScheduleDurationMin',
    'ventScheduleHour1', 'ventScheduleHour2', 'ventScheduleHour3', 'ventScheduleHour4',
    'ventScheduleHour5', 'ventScheduleHour6', 'tempOffset', 'humidityOffset', 'pidCycleSec',
    'humidityAlarmDelaySec', 'turnIntervalMin', 'turnMaxRunSec',
    'powerRestoreDelaySec', 'sensorTimeoutSec', 'maxHeaterPower',
    'totalIncubationDays', 'circulationFanEnabled', 'turningEnabled',
    'autoResumeAfterPower', 'allowHeatWithoutBatch', 'alarmEnabled',
    'lightAfterBatchAlarmEnabled', 'highTempAlarmWithoutBatch', 'controlMode',
    'nextDirection', 'heaterStuckMinRiseC', 'heaterStuckDurationSec',
    'tempRateLimitC', 'tempRateWindowSec', 'tempOscillationCrossLimit',
    'tempOscillationWindowSec', 'autotuneRelayPowerPercent', 'autotuneBandC',
    'manualTurnReanchorsSchedule', 'sirenSelfTestEnabled'
  ]);
  const CONFIG_KEYS = Object.freeze([...REQUIRED_CONFIG_KEYS, ...VENT_PROFILE_KEYS, 'adaptiveThermalBalanceEnabled']);

  const DEFAULT_BATCH_META = Object.freeze({
    name: 'Mẻ ấp 01',
    startDate: new Date().toISOString().slice(0, 10),
    targetHumidity: 58
  });

  // Chu thich giu NGAN (~30 ky tu) de luon vua DUNG 1 DONG - CSS con chan
  // cung bang white-space:nowrap + ellipsis nen khong bao gio xuong dong 2.
  // Chuoi o day la nguon DUY NHAT: init() goi showPage('device') nen ban
  // trong index.html chi la cho dat tam, khong con nguy co lech 2 noi.
  const pageMeta = {
    device: ['Thiết bị', 'Theo dõi và điều khiển máy.'],
    batch: ['Mẻ ấp', 'Thiết lập và quản lý mẻ ấp.'],
    settings: ['Cài đặt', 'Thông số vận hành và kết nối.']
  };

  const TELEMETRY_WINDOW_MS = 30 * 60 * 1000;
  const TELEMETRY_LIVE_SAMPLE_MS = 5000;
  const TELEMETRY_HISTORY_REFRESH_MS = 5 * 60 * 1000;
  const TELEMETRY_MAX_POINTS = 500;
  const telemetryChart = {
    deviceId: '', points: [], historyLoaded: false, historyLoadedAt: 0,
    historyLoading: false, historyRetryAt: 0, historyRequestSeq: 0,
    activeRequestId: '', nextCursor: 0, historyGap: false,
    lastSampleAt: 0, renderRaf: 0,
  };

  const state = {
    devices: window.MayapAccount ? [] : loadDevices(),
    selectedId: localStorage.getItem(`${STORAGE}.selected`) || '',
    realtime: null,
    realtimeConnected: false,
    realtimeMessage: 'Chưa kết nối với máy',
    realtimeSessionState: 'idle',
    subscriptions: new Set(),
    subscriptionEpoch: 0,
    subscriptionRequests: new Map(),
    realtimeCredentials: null,
    syncRetryTimers: [],
    subscriptionRetryTimer: 0,
    sessionTimer: 0,
    realtimeExpiresAt: 0,
    staleTimer: 0,
    formFlags: new Map(),
    pending: new Map(),
    configChunks: new Map(),
    lastTerminalByDevice: new Map(),
    uncertain: new Map(),
    confirmResolver: null,
    currentActivityStartedAt: 0,
    lastResumePromptBootId: 0
  };
  // Per-tab identity. In account mode pairingToken is a non-secret RAM UI marker;
  // server ownership/bearer session authorizes grants; keys stay in this tab.
  const controlClientId = `w-${Array.from(crypto.getRandomValues(new Uint8Array(8)),
    (b) => b.toString(16).padStart(2, '0')).join('')}`;
  const controlSessions = new Map();
  const transactions = new window.MayapProtocolV2.TransactionLedger();
  const controlSequences = new Map();
  const PACKET_POLICY = window.MayapProtocolV2.PacketPolicy;
  const encoder = new TextEncoder();
  async function storeControlSession(device, control) {
    if (!/^[a-f0-9]{64}$/i.test(control.sessionKey) || typeof control.grant !== 'string' ||
        !/^[a-f0-9]{64}$/i.test(control.grantSig) ||
        !control.grant.length || !Number.isFinite(Number(control.expiresAt)) ||
        Number(control.expiresAt) <= Math.floor(Date.now() / 1000) + 30) throw new Error('PROTOCOL_ERROR');
    control.key = await crypto.subtle.importKey('raw', new Uint8Array(
      control.sessionKey.match(/../g).map((v) => parseInt(v, 16))),
    { name: 'HMAC', hash: 'SHA-256' }, false, ['sign', 'verify']);
    delete control.sessionKey;
    controlSessions.set(device.id, control);
    return control;
  }
  // One in-flight /api/mqtt-session per device. The same reply carries the broker
  // credentials and, for owners/operators, the signed control grant.
  const mqttSessionRequests = new Map();
  const readOnlyDevices = new Set();   // /api/mqtt-session returned control:null
  function fetchMqttSession(device) {
    const running = mqttSessionRequests.get(device.id);
    if (running) return running;
    const request = (async () => {
      const reply = await postCloudJson('/api/mqtt-session',
        { device_id: device.id, client_id: controlClientId }, 8000);
      if (!reply.success || !reply.mqtt) {
        const error = new Error(reply.status === 401 || reply.status === 403
          ? 'Cần đăng nhập lại hoặc không có quyền với máy này'
          : 'Chưa lấy được phiên kết nối máy chủ');
        error.code = reply.status === 401 || reply.status === 403 ? 'AUTH_ERROR' : 'TRANSPORT_ERROR';
        throw error;
      }
      if (reply.control) { readOnlyDevices.delete(device.id); await storeControlSession(device, reply.control); }
      else readOnlyDevices.add(device.id);
      return reply;
    })().finally(() => mqttSessionRequests.delete(device.id));
    mqttSessionRequests.set(device.id, request);
    return request;
  }

  async function controlSession(device) {
    const cached = controlSessions.get(device.id);
    if (cached && cached.expiresAt > Math.floor(Date.now() / 1000) + 30) return cached;
    await fetchMqttSession(device);
    const control = controlSessions.get(device.id);
    if (control) return control;
    // A click must never weaken command authentication: viewers get no grant.
    const error = new Error('Tài khoản này chỉ có quyền xem máy');
    error.code = 'AUTH_ERROR';
    throw error;
  }

  function controlGrantReady(device) {
    return Number(controlSessions.get(device?.id)?.expiresAt || 0) > Math.floor(Date.now() / 1000) + 30;
  }

  function controlReady(device) {
    return state.realtime?.deviceId === device?.id && isDeviceOnline(device) && controlGrantReady(device);
  }

  function cachedRuntime(id) {
    const cached = loadJson(`${RUNTIME_CACHE}.${id}`, null);
    if (cached?.v !== 1 || !cached.snapshot?.runtime || !Number.isFinite(cached.receivedAt) ||
        cached.receivedAt <= 0 || cached.receivedAt > Date.now() + 60000) return {};
    return { presence: cached.presence || null, presenceAt: cached.presenceAt || 0,
      snapshot: cached.snapshot, snapshotAt: cached.receivedAt,
      dataSource: 'cache', liveEpoch: -1, cacheAt: cached.receivedAt,
      features: { humidifierInstalled: cached.features?.humidifierInstalled === true },
      bootId: Number(cached.snapshot.bootId || 0), revision: Number(cached.snapshot.revision || 0) };
  }

  function persistRuntimeCache(device, force = false) {
    if (!device?.snapshot?.runtime || device.dataSource !== 'live') return;
    // Coalesce the 400ms stream; final hidden/pagehide flush preserves the latest sample.
    if (!force && Date.now() - (device.cacheWrittenAt || 0) < 5000) return;
    try {
      const previous = loadJson(`${RUNTIME_CACHE}.${device.id}`, null);
      if (Number(previous?.receivedAt || 0) > device.snapshotAt) return;
      localStorage.setItem(`${RUNTIME_CACHE}.${device.id}`, JSON.stringify({ v: 1,
        presence: device.presence, presenceAt: device.presenceAt,
        features: device.features || {},
        snapshot: device.snapshot, receivedAt: device.snapshotAt }));
      device.cacheWrittenAt = Date.now();
    } catch (_) {} // Private mode/quota failure must not interrupt WebSocket or control.
  }

  function createDevice(id, name, pairingToken = '') {
    return {
      id,
      name,
      pairingToken,
      presence: null,
      presenceAt: 0,
      snapshot: null,
      snapshotAt: 0,
      config: null,
      configAt: 0,
      revision: 0,
      bootId: 0,
      dataSource: '',
      features: {},
      liveEpoch: -1,
      presenceEpoch: -1,
      requestedData: new Set(),
      ...cachedRuntime(id),
      commandSequence: Math.max(1, Number(localStorage.getItem(`${STORAGE}.seq.${id}`) || 0)),
      logs: loadJson(`${STORAGE}.logs.${id}`, []),
      batchMeta: {
        ...DEFAULT_BATCH_META,
        ...loadJson(`${STORAGE}.batch.${id}`, {})
      }
    };
  }

  function loadDevices() {
    if (window.MayapAccount) return []; // D1 ownership is authoritative.
    const stored = loadJson(`${STORAGE}.devices`, []);
    if (!Array.isArray(stored)) return [];
    return stored
      .filter((item) => item && DEVICE_ID_RE.test(String(item.id || '').toUpperCase()))
      .map((item) => createDevice(String(item.id).toUpperCase(), String(item.name || 'Máy ấp'), String(item.pairingToken || '')));
  }

  function loadJson(key, fallback) {
    try {
      const parsed = JSON.parse(localStorage.getItem(key) || 'null');
      return parsed ?? fallback;
    } catch (_) {
      return fallback;
    }
  }

  function saveDevices() {
    localStorage.setItem(`${STORAGE}.devices`, JSON.stringify(
      state.devices.map(({ id, name }) => ({ id, name }))
    ));
    localStorage.setItem(`${STORAGE}.selected`, state.selectedId || '');
  }

  function saveDeviceRuntime(device) {
    if (!device) return;
    localStorage.setItem(`${STORAGE}.batch.${device.id}`, JSON.stringify(device.batchMeta));
    localStorage.setItem(`${STORAGE}.logs.${device.id}`, JSON.stringify(device.logs.slice(0, 100)));
    localStorage.setItem(`${STORAGE}.seq.${device.id}`, String(device.commandSequence));
  }

  function currentDevice() {
    return state.devices.find((device) => device.id === state.selectedId) || null;
  }

  function normalizeDeviceId(value) {
    return String(value || '').trim().toUpperCase().replace(/\s+/g, '');
  }

  // Quet QR: uu tien API BarcodeDetector co san cua trinh duyet (Chrome/Edge/
  // WebView Android tu ban 83) vi chay bang phan cung, khong can bao tri.
  // Safari/iOS (moi phien ban trinh duyet tren iOS deu dung engine WebKit)
  // khong co BarcodeDetector, nen fallback sang jsQR - thu vien JS thuan,
  // nhung san (vendor/jsQR.min.js, khong qua CDN ngoai) - doc tung khung
  // hinh video qua canvas roi tu giai ma. Chi khi ca hai deu khong dung duoc
  // moi rot ve nhap ID bang tay o dung o input da co san.
  let qrStream = null;
  let qrDetector = null;
  let qrScanRaf = 0;
  let qrCanvas = null;
  let qrCanvasCtx = null;

  function qrScanSupported() {
    return typeof window.BarcodeDetector === 'function' || typeof window.jsQR === 'function';
  }

  async function startQrScan() {
    if (!qrScanSupported()) {
      toast('Trình duyệt này chưa hỗ trợ quét QR, hãy nhập ID thủ công.');
      return;
    }
    try {
      qrStream = await navigator.mediaDevices.getUserMedia({
        video: { facingMode: 'environment' },
        audio: false,
      });
    } catch (_) {
      toast('Không mở được camera - kiểm tra quyền truy cập camera cho trang này.');
      return;
    }
    // Khong "await video.play()" truoc khi hien UI/bat dau vong lap detect:
    // video da co attribute autoplay + muted + playsinline nen tu phat khi
    // co srcObject, con promise cua play() co the treo lau (hoac khong bao
    // gio resolve) tren mot so thiet bi/camera - cho no se lam ca khung
    // quet bi "im", nguoi dung tuong nhu khong bam duoc gi.
    const video = $('qrVideo');
    video.srcObject = qrStream;
    video.play().catch(() => {});
    $('qrScanOverlay').hidden = false;

    const useNative = typeof window.BarcodeDetector === 'function';
    if (useNative) {
      qrDetector = new window.BarcodeDetector({ formats: ['qr_code'] });
    } else {
      qrCanvas = document.createElement('canvas');
      qrCanvasCtx = qrCanvas.getContext('2d', { willReadFrequently: true });
    }

    const tick = async () => {
      if (!qrStream) return;
      try {
        let rawValue = null;
        if (useNative) {
          const codes = await qrDetector.detect(video);
          if (codes.length) rawValue = codes[0].rawValue;
        } else if (video.videoWidth && video.videoHeight) {
          qrCanvas.width = video.videoWidth;
          qrCanvas.height = video.videoHeight;
          qrCanvasCtx.drawImage(video, 0, 0, qrCanvas.width, qrCanvas.height);
          const frame = qrCanvasCtx.getImageData(0, 0, qrCanvas.width, qrCanvas.height);
          const result = window.jsQR(frame.data, frame.width, frame.height);
          if (result) rawValue = result.data;
        }
        if (rawValue) {
          const id = normalizeDeviceId(rawValue);
          if (DEVICE_ID_RE.test(id)) {
            $('newDeviceId').value = id;
            stopQrScan();
            toast('Đã quét được ID thiết bị, nhập mã PIN để hoàn tất.');
            setTimeout(() => $('newDevicePin').focus(), 60);
            return;
          }
        }
      } catch (_) {
        // Frame loi (video chua san sang v.v.) - bo qua, thu lai frame sau.
      }
      qrScanRaf = requestAnimationFrame(tick);
    };
    qrScanRaf = requestAnimationFrame(tick);
  }

  function stopQrScan() {
    if (qrScanRaf) cancelAnimationFrame(qrScanRaf);
    qrScanRaf = 0;
    qrDetector = null;
    qrCanvas = null;
    qrCanvasCtx = null;
    if (qrStream) {
      qrStream.getTracks().forEach((track) => track.stop());
      qrStream = null;
    }
    const video = $('qrVideo');
    if (video) video.srcObject = null;
    const overlay = $('qrScanOverlay');
    if (overlay) overlay.hidden = true;
  }

  // Goi thang Cloudflare Worker cho nhom "danh tinh thiet bi" (PIN/ten hien
  // thi) - tach rieng voi push.js vi khong lien quan gi den Web Push, chi
  // dung chung 1 config cloudApiBase.
  function cloudApiUrl(path) {
    const base = String(WEB.cloudApiBase || '').replace(/\/+$/, '');
    return base ? `${base}${path}` : null;
  }

  async function postCloudJson(path, payload, timeoutMs = 0) {
    const url = cloudApiUrl(path);
    if (!url) return { success: false, error: 'Trang web chưa cấu hình máy chủ (cloudApiBase)' };
    const controller = timeoutMs ? new AbortController() : null;
    const timeout = controller ? setTimeout(() => controller.abort(), timeoutMs) : 0;
    try {
      const res = await fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload),
        ...(controller ? { signal: controller.signal } : {}),
      });
      const body = await res.json().catch(() => ({}));
      if (!res.ok || !body.success) return { ...body, success:false, status:res.status, error:body.error || `Máy chủ từ chối (HTTP ${res.status})` };
      return { ...body, status:res.status };
    } catch (error) {
      return { success: false, status: 0, error: String(error?.message || error) };
    } finally {
      if (timeout) clearTimeout(timeout);
    }
  }

  async function getCloudJson(path) {
    const url = cloudApiUrl(path);
    if (!url) return { success: false, error: 'Trang web chưa cấu hình máy chủ (cloudApiBase)' };
    try {
      const res = await fetch(url);
      const body = await res.json().catch(() => ({}));
      if (!res.ok || !body.success) return { ...body, success:false, status:res.status, error:body.error || `Máy chủ từ chối (HTTP ${res.status})` };
      return { ...body, status:res.status };
    } catch (error) {
      return { success: false, error: String(error?.message || error) };
    }
  }


  async function deleteCloudJson(path,payload,timeoutMs=8000) {
    const url=cloudApiUrl(path);if(!url)return {success:false,error:'Trang web chưa cấu hình máy chủ (cloudApiBase)'};
    const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),timeoutMs);
    try{
      const res=await fetch(url,{method:'DELETE',headers:{'Content-Type':'application/json'},
        body:JSON.stringify(payload),signal:controller.signal});
      const body=await res.json().catch(()=>({}));
      if(!res.ok||!body.success)return {...body,success:false,status:res.status,error:body.error||`Máy chủ từ chối (HTTP ${res.status})`};
      return {...body,status:res.status};
    }catch(error){return {success:false,status:0,error:String(error?.message||error)};}
    finally{clearTimeout(timer);}
  }
  function cloudDataMessage(result,fallback='Không lưu được dữ liệu Cloud.') {
    const messages={
      ACCESS_DENIED:'Tài khoản này không có quyền thay đổi dữ liệu của máy.',
      ACCOUNT_LOGIN_REQUIRED:'Phiên đăng nhập đã hết hạn.',
      NOTE_LIMIT_REACHED:'Máy đã đạt giới hạn 200 Ghi chú.',
      REMINDER_LIMIT_REACHED:'Máy đã đạt giới hạn 32 Nhắc nhở.',
      NOTE_CONFLICT:'Ghi chú vừa được thay đổi ở nơi khác. Hãy tải lại trước khi sửa tiếp.',
      REMINDER_CONFLICT:'Nhắc nhở vừa được thay đổi ở nơi khác. Hãy tải lại.',
      NOTE_DELETED:'Ghi chú này đã bị xóa ở nơi khác.',
      SERVICE_UNAVAILABLE:'Cloudflare/D1 đang tạm không sẵn sàng. Nội dung chưa bị mất.'
    };
    return messages[result?.error] || (result?.status===0?'Không kết nối được Cloudflare. Kiểm tra mạng rồi thử lại.':result?.error||fallback);
  }
  function requireCloud(result,fallback) {
    if(result?.success)return result;
    throw new Error(cloudDataMessage(result,fallback));
  }
  const cloudUuid=()=>crypto.randomUUID ? crypto.randomUUID() :
    'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g,c=>{
      const r=crypto.getRandomValues(new Uint8Array(1))[0]&15,v=c==='x'?r:(r&3)|8;return v.toString(16);
    });
  async function listCloudNotes(deviceId) {
    return requireCloud(await getCloudJson(`/api/device/${encodeURIComponent(deviceId)}/notes`),'Không tải được Ghi chú.');
  }
  async function saveCloudNote(deviceId,note) {
    return requireCloud(await postCloudJson(`/api/device/${encodeURIComponent(deviceId)}/notes`,{
      id:note.id,mutation_id:note.mutationId,version:note.version,type:note.type,title:note.title,content:note.content
    },8000),'Không lưu được Ghi chú.');
  }
  async function deleteCloudNote(deviceId,note) {
    return requireCloud(await deleteCloudJson(`/api/device/${encodeURIComponent(deviceId)}/notes/${encodeURIComponent(note.id)}`,{
      mutation_id:note.mutationId,version:note.version
    }),'Không xóa được Ghi chú.');
  }
  async function loadCloudReminders(device,force=false) {
    if(!device?.id)return;
    if(device.remindersLoading)return;
    if(device.remindersLoaded&&!force){renderReminderList(device);return;}
    device.remindersLoading=true;device.remindersError='';renderReminderList(device);
    const result=await getCloudJson(`/api/device/${encodeURIComponent(device.id)}/reminders`);
    if(result.success){device.reminders=Array.isArray(result.reminders)?result.reminders:[];device.remindersLoaded=true;device.remindersError='';}
    else device.remindersError=cloudDataMessage(result,'Không tải được Nhắc nhở.');
    device.remindersLoading=false;if(device.id===state.selectedId)renderReminderList(device);
  }
  async function addCloudReminder(device,day,label) {
    if(!device?.id)throw new Error('Hãy chọn thiết bị trước.');
    const key=`${day}\n${label}`;
    if(!device.reminderDraft||device.reminderDraft.key!==key)
      device.reminderDraft={key,id:cloudUuid(),mutationId:cloudUuid()};
    device.remindersPending=true;renderReminderList(device);
    try{
      const result=requireCloud(await postCloudJson(`/api/device/${encodeURIComponent(device.id)}/reminders`,{
        id:device.reminderDraft.id,mutation_id:device.reminderDraft.mutationId,day,label
      },8000),'Không lưu được Nhắc nhở.');
      device.reminders=(device.reminders||[]).filter(item=>item.id!==result.reminder.id);device.reminders.push(result.reminder);
      device.remindersLoaded=true;device.reminderDraft=null;return result.reminder;
    }finally{device.remindersPending=false;if(device.id===state.selectedId)renderReminderList(device);}
  }
  async function removeCloudReminder(device,item) {
    if(!device?.id)return;
    device.remindersPending=true;renderReminderList(device);
    try{
      requireCloud(await deleteCloudJson(`/api/device/${encodeURIComponent(device.id)}/reminders/${encodeURIComponent(item.id)}`,{
        mutation_id:cloudUuid(),version:Number(item.version||0)
      }),'Không xóa được Nhắc nhở.');
      device.reminders=(device.reminders||[]).filter(entry=>entry.id!==item.id);device.remindersLoaded=true;
    }finally{device.remindersPending=false;if(device.id===state.selectedId)renderReminderList(device);}
  }

  function verifyDevicePin(deviceId, pin) {
    return postCloudJson('/api/account/devices/claim', { device_id: deviceId, pin });
  }

  function renameDeviceRemote(deviceId, name) {
    return postCloudJson('/api/device/rename', { device_id: deviceId, name });
  }

  function changeDevicePin(deviceId, oldPin, newPin) {
    return postCloudJson('/api/device/change-pin', { device_id: deviceId, old_pin: oldPin, new_pin: newPin });
  }

  // Key used to sign each envelope, so a grant refreshed meanwhile cannot change the ACK key.
  const envelopeKeys = new WeakMap();
  async function signRealtimeWrite(device, channel, body) {
    if (!device?.pairingToken) {
      const error = new Error('Cần xác thực lại PIN: bấm + và thêm lại đúng ID thiết bị để làm mới quyền điều khiển.');
      error.code = 'AUTH_ERROR';
      throw error;
    }
    if (Number(device.presence?.proto || 0) >= 2) {
      const session = await controlSession(device);
      if (!device.bootId) {
        const error = new Error('Đang nhận dữ liệu từ máy; vui lòng chờ một chút');
        error.code = 'PROTOCOL_ERROR';
        throw error;
      }
      // Every wire body signed with the V2 domain must itself declare V2.
      // Callers may still construct legacy-shaped objects; normalize here so
      // History must use the same signed V2 application envelope.
      body.v = 2;
      body.bootId = device.bootId;
      const pending = state.pending.get(String(body.requestId || ''));
      if (pending) pending.ackKey = session.key;
      const nowSec = Math.floor(Date.now() / 1000);
      if (channel === 'command') body.expiresAt = nowSec + 8;
      body.clientId = controlClientId;
      body.seq = Math.max(Date.now(), (controlSequences.get(device.id) || 0) + 1);
      controlSequences.set(device.id, body.seq);
      body.nonce ||= Array.from(crypto.getRandomValues(new Uint8Array(8)),
        (b) => b.toString(16).padStart(2, '0')).join('');
      const bodyText = JSON.stringify(body);
      const message = `mayap-mqtt-write:v2\n${device.id}\n${channel}\n${session.grant}\n${bodyText}`;
      const sig = Array.from(new Uint8Array(await crypto.subtle.sign('HMAC', session.key,
        encoder.encode(message))), (b) => b.toString(16).padStart(2, '0')).join('');
      const envelope = { v: 2, grant: session.grant, grantSig: session.grantSig, body: bodyText, sig };
      envelopeKeys.set(envelope, session.key);
      return envelope;
    }
    const error = new Error('Firmware cũ chưa hỗ trợ giao thức V2. Hãy cập nhật máy để điều khiển từ Web.');
    error.code = 'PROTOCOL_ERROR';
    throw error;
  }

  // Ten thiet bi la thuoc tinh CHUNG (luu tren Worker, xem renameDeviceRemote())
  // nhung truoc day web CHI lay ten luc THEM thiet bi lan dau roi luu co dinh
  // vao localStorage cua rieng trinh duyet do - neu doi ten tu 1 trinh duyet
  // khac (cung device_id), cac trinh duyet con lai khong bao gio biet, hien
  // ten cu mai mai. Dinh ky doc lai ten that tu endpoint cong khai (khong can
  // PIN) de moi trinh duyet dang xem cung 1 thiet bi som muon deu dong bo.
  const deviceNameFetchedAt = new Map();
  const DEVICE_NAME_TTL_MS = 2 * 60 * 1000;
  async function refreshDeviceNameIfNeeded(device) {
    if (window.MayapAccount) {
      const row = window.MayapAccount.current?.devices.find(d => d.device_id === device?.id);
      if (row && row.device_name !== device.name) { device.name = row.device_name; renderSelector(); }
      return;
    }
    if (!device) return;
    const last = deviceNameFetchedAt.get(device.id) || 0;
    if (Date.now() - last < DEVICE_NAME_TTL_MS) return;
    deviceNameFetchedAt.set(device.id, Date.now());
    const result = await getCloudJson(`/api/device/${device.id}/status`);
    if (result.success && result.exists && result.device_name && result.device_name !== device.name) {
      device.name = result.device_name;
      saveDeviceRuntime(device);
      renderSelector();
    }
  }

  function escapeHtml(value) {
    return String(value).replace(/[&<>'"]/g, (char) => ({
      '&': '&amp;', '<': '&lt;', '>': '&gt;', "'": '&#39;', '"': '&quot;'
    }[char]));
  }

  function numberVi(value, digits = 1) {
    if (value === null || value === undefined || value === '') return '—';
    const number = Number(value);
    return Number.isFinite(number) ? number.toFixed(digits).replace('.', ',') : '—';
  }

  function bool(value) {
    return value === true;
  }

  let faultPopupTimer = null;
  function closeFaultPopup() {
    const popup = $('faultPopup');
    if (popup) popup.hidden = true;
    if (faultPopupTimer) { clearTimeout(faultPopupTimer); faultPopupTimer = null; }
  }
  // Chi cap nhat NOI DUNG popup (khi co snapshot moi trong luc popup dang
  // mo) - KHONG dung timer dong tu dong, tranh vong lap "cu 5s refresh lai
  // 1 lan lam popup khong bao gio tu dong tat" (renderDevice() co
  // setInterval refresh 5000 ms doc lap voi popup).
  function refreshFaultPopupContent(device) {
    const faults = Array.isArray(device?.activeFaults) ? device.activeFaults : [];
    const popup = $('faultPopup');
    const list = $('faultPopupList');
    if (!faults.length || !popup || !list) return;
    list.replaceChildren(...faults.map((fault) => {
      const code = Number(fault.code);
      const severity = Number(fault.severity || 0);
      const sevClass = severity >= 3 ? 'sev-emergency' : severity >= 2 ? 'sev-stop' : 'sev-warn';
      const item = document.createElement('section');
      item.className = 'faultPopupItem';
      const head = document.createElement('div');
      head.className = 'faultPopupHead';
      const codeElement = document.createElement('span');
      codeElement.className = `faultPopupCode ${sevClass}`;
      codeElement.textContent = `E${code}`;
      const title = document.createElement('span');
      title.className = 'faultPopupTitle';
      title.textContent = FAULT_TITLES[code] || `Mã lỗi ${code}`;
      const description = document.createElement('p');
      description.textContent = FAULT_DESCRIPTIONS[code] || FAULT_TITLES[code] || '';
      head.append(codeElement, title);
      item.append(head, description);
      return item;
    }));
  }
  function openFaultPopup(device) {
    if (!device?.activeFaults?.length) return;
    refreshFaultPopupContent(device);
    $('faultPopup').hidden = false;
    if (faultPopupTimer) clearTimeout(faultPopupTimer);
    faultPopupTimer = setTimeout(closeFaultPopup, 6000);
  }

  // Cap nhat o "Trang thai": binh thuong hien 3 trang thai ngan gon (Dang
  // ap/San sang/Offline). Neu co loi dang active: loi nhe (Warning) chi gan
  // dau "!" vao trang thai hien tai (may van ap duoc); loi nang (Stop/
  // Emergency - khong con ap duoc nua) thay HOAN TOAN bang "Loi E<ma>". O
  // duoc to mau theo muc do, bam vao se hien popup chi tiet (xem
  // openFaultPopup/closeFaultPopup, gan trong bindUi()).
  function renderLiveState(device, runtime) {
    const tile = $('liveStateTile');
    const el = $('liveState');
    if (!tile || !el) return;
    if (!device) {
      el.textContent = 'Ngoại tuyến';
      tile.classList.remove('tile-warn', 'tile-stop', 'tile-emergency', 'tile-clickable');
      closeFaultPopup();
      return;
    }
    const faults = Array.isArray(runtime?.activeFaults) ? runtime.activeFaults : [];
    const fault = faults.length ? faults[0] : null;
    device.activeFaults = faults;
    device.activeFault = fault;

    tile.classList.remove('tile-warn', 'tile-stop', 'tile-emergency');
    tile.classList.toggle('tile-clickable', !!fault);

    if (connectionStatus(device) === 'offline') {
      el.textContent = 'Ngoại tuyến';
      return;
    }

    if (!runtime) {
      el.textContent = connectionStatus(device) === 'offline' ? 'Ngoại tuyến' : 'Đang đồng bộ…';
      closeFaultPopup();
      return;
    }
    const base = runtime?.batchRunning ? 'Đang ấp' : 'Sẵn sàng';
    if (!fault) {
      el.textContent = base;
      closeFaultPopup();
      return;
    }
    const severity = Number(fault.severity || 0);
    if (faultBlocksBatch(severity)) {
      tile.classList.add(severity >= 3 ? 'tile-emergency' : 'tile-stop');
      el.textContent = `Lỗi E${Number(fault.code)}`;
    } else {
      tile.classList.add('tile-warn');
      el.textContent = '';
      el.append(document.createTextNode(base + ' '));
      const mark = document.createElement('span');
      mark.className = 'faultMark';
      mark.setAttribute('aria-hidden', 'true');
      mark.title = 'Có cảnh báo · chạm để xem chi tiết';
      el.append(mark);
    }
    // Neu popup dang mo cho dung loi nay, cap nhat lai noi dung (vd severity
    // doi) thay vi dong mo lai gay nhap nhay.
    if (!$('faultPopup')?.hidden) refreshFaultPopupContent(device);
  }

  // Firmware REALTIME_REQUEST_ID_CAPACITY = 40 byte KE CA null terminator (xem
  // config.h + mayap_web_adapter.h::readString trong firmware RC2), nen chuoi
  // request Id toi da dung duoc la 39 ky tu. crypto.randomUUID() co dau gach
  // ngang dai 36 ky tu; kem tien to "cfg-"/"cmd-" (4 ky tu) la du 40 ky tu va
  // BI FIRMWARE TU CHOI TOAN BO GOI (readString tra ve false -> "invalid").
  // Bo dau gach ngang de con 32 ky tu hex, luon nam duoi gioi han an toan.
  const REQUEST_ID_MAX = 24;
  function requestId(prefix = 'req') {
    const raw = crypto.randomUUID
      ? crypto.randomUUID().replace(/-/g, '')
      : `${Date.now().toString(36)}${Math.random().toString(36).slice(2)}${Math.random().toString(36).slice(2)}`;
    return `${prefix}-${raw}`.slice(0, REQUEST_ID_MAX);
  }

  function routes(deviceId) {
    const route = channel => ({ deviceId, channel });
    return { presence: route('presence'), bootstrap: route('bootstrap'), snapshot: route('snapshot'),
      report: route('config/reported'), ack: route('ack'),
      log: route('log'), historyReport: route('history/reported'), config: route('config/set'),
      command: route('command'), historyRequest: route('history/request'),
      session: route('session') };
  }

  function toast(message, timeout = 2600) {
    const element = $('toast');
    if (!element) return;
    clearTimeout(toast.timer);
    element.textContent = message;
    element.classList.add('show');
    toast.timer = setTimeout(() => element.classList.remove('show'), timeout);
  }

  function timeText(epochOrDate) {
    const date = epochOrDate instanceof Date
      ? epochOrDate
      : new Date(Number(epochOrDate) > 1000000000000 ? Number(epochOrDate) : Number(epochOrDate) * 1000);
    if (Number.isNaN(date.getTime())) return '--:--';
    return date.toLocaleTimeString('vi-VN', { hour: '2-digit', minute: '2-digit' });
  }

  function confirmAction({ title, message, accept = 'Xác nhận', danger = false }) {
    $('confirmTitle').textContent = title;
    $('confirmMessage').textContent = message;
    $('confirmAccept').textContent = accept;
    $('confirmAccept').className = danger ? 'dangerButton' : 'primary dark';
    $('confirmDialog').showModal();
    return new Promise((resolve) => { state.confirmResolver = resolve; });
  }

  function finishConfirm(value) {
    if ($('confirmDialog').open) $('confirmDialog').close();
    const resolve = state.confirmResolver;
    state.confirmResolver = null;
    if (resolve) resolve(value);
  }

  function ensureFormState(formId) {
    const form = $(formId);
    if (!form) return null;
    let element = form.querySelector('.formState');
    if (!element) {
      // Giữ trạng thái cho logic và trình đọc màn hình nhưng không chiếm chỗ giao diện.
      element = document.createElement('output');
      element.className = 'formState';
      element.hidden = true;
      element.setAttribute('aria-live', 'polite');
      form.append(element);
    }
    return element;
  }

  function updateSubmitState(formId, mode) {
    const submit = $(formId)?.querySelector('button[type="submit"]');
    if (!submit) return;
    if (!submit.dataset.defaultLabel) submit.dataset.defaultLabel = submit.textContent.trim();
    submit.classList.toggle('attention', mode === 'dirty');
    submit.disabled = mode === 'pending';
    submit.textContent = mode === 'pending' ? 'Đang lưu…' : submit.dataset.defaultLabel;
  }

  function setFormState(formId, mode, text) {
    const element = ensureFormState(formId);
    if (!element) return;
    element.hidden = !['dirty', 'pending', 'unconfirmed', 'error'].includes(mode);
    element.dataset.mode = mode;
    element.textContent = text;
    const old = state.formFlags.get(formId) || {};
    state.formFlags.set(formId, { ...old, dirty: mode === 'dirty', mode, text });
    updateSubmitState(formId, mode);
  }

  function setFormError(formId, message = '') {
    const form = $(formId);
    if (!form) return;
    let error = form.querySelector('.formError');
    if (!error) {
      error = document.createElement('div');
      error.className = 'formError';
      const submit = form.querySelector('button[type="submit"]');
      form.insertBefore(error, submit || null);
    }
    error.textContent = message;
    error.classList.toggle('show', Boolean(message));
  }

  function clearInvalid(formId) {
    $(formId)?.querySelectorAll('.invalid').forEach((element) => element.classList.remove('invalid'));
    setFormError(formId, '');
  }

  function invalidate(formId, id, message) {
    const element = $(id);
    element?.classList.add('invalid');
    element?.closest('.compactTile')?.classList.add('invalid');
    setFormError(formId, message);
    element?.focus();
    return false;
  }

  function markDirty(formId) {
    setFormState(formId, 'dirty', 'Có thay đổi chưa lưu');
  }

  function registerDirty(formId) {
    const form = $(formId);
    if (!form) return;
    ensureFormState(formId);
    form.querySelectorAll('input,select').forEach((element) => {
      element.addEventListener('input', () => markDirty(formId));
      element.addEventListener('change', () => markDirty(formId));
    });
  }

  function showPage(name) {
    document.body.dataset.page = name;
    document.querySelectorAll('.page').forEach((element) => {
      element.classList.toggle('active', element.id === `page-${name}`);
    });
    document.querySelectorAll('.nav button').forEach((element) => {
      element.classList.toggle('active', element.dataset.page === name);
      if (element.dataset.page === name) element.setAttribute('aria-current', 'page');
      else element.removeAttribute('aria-current');
    });
    $('pageTitle').textContent = pageMeta[name][0];
    $('pageSubtitle').textContent = pageMeta[name][1];
    if (name === 'batch') { requestDeviceData('config'); requestDeviceData('history'); loadTelemetryHistory(); requestTemperatureChartRender(); }
    if (name === 'settings') { requestDeviceData('config'); renderBatchLogs(); refreshFirmwareLatestIfNeeded(); }
    document.querySelector('.main')?.scrollTo({ top: 0, left: 0, behavior: 'instant' });
    window.scrollTo({ top: 0, left: 0, behavior: 'instant' });
  }

  function connectionStatus(device) {
    if (!device) return 'none';
    if (!state.realtimeConnected) return device.snapshot ? 'cache' : 'connecting';
    // Only a presence received on this connection proves the device's LWT state.
    if (device.presenceEpoch === state.subscriptionEpoch && device.presence?.online === false) return 'offline';
    if (!device.snapshot || !device.snapshotAt) return 'waiting';
    if (device.dataSource !== 'live' || device.liveEpoch !== state.subscriptionEpoch) return 'cache';
    if (Date.now() - device.snapshotAt > WEB.staleAfterMs || device.presence?.online !== true) return 'degraded';
    return 'online';
  }

  function isDeviceOnline(device = currentDevice()) {
    const connection = connectionStatus(device);
    // Long telemetry gaps disable control without inventing an offline presence.
    return (connection === 'online' || connection === 'degraded') &&
      Date.now() - device.snapshotAt <= Math.max(WEB.staleAfterMs, WEB.offlineAfterMs);
  }

  function freshnessText(device) {
    if (!device?.snapshotAt) return 'Đang chờ dữ liệu từ máy';
    const time = new Date(device.snapshotAt).toLocaleString('vi-VN');
    if (device.dataSource === 'bootstrap' && !device.bootstrapTimeKnown)
      return `Bản lưu trên máy chủ · chưa biết tuổi dữ liệu · nhận lúc ${time}`;
    return connectionStatus(device) === 'online' ? `Dữ liệu live · ${time}` :
      `${device.dataSource === 'bootstrap' ? 'Bản lưu trên máy chủ' : 'Dữ liệu đã nhận'} · ${time}`;
  }

  function freshnessLabel(device) {
    if (!device?.snapshotAt) return '';
    const date = new Date(device.snapshotAt);
    const clock = date.toLocaleTimeString('vi-VN', { hour: '2-digit', minute: '2-digit', second: '2-digit' });
    if (connectionStatus(device) === 'online') return `Live · ${clock}`;
    if (device.dataSource === 'bootstrap' && !device.bootstrapTimeKnown) return 'Máy chủ · tuổi chưa rõ';
    const day = date.toLocaleDateString('vi-VN', { day: '2-digit', month: '2-digit', year: '2-digit' });
    return `${device.dataSource === 'bootstrap' ? 'Máy chủ' : 'Lưu'} · ${day} ${clock.slice(0, 5)}`;
  }

  function renderSelector() {
    const select = $('deviceSelector');
    select.replaceChildren();
    if (!state.devices.length) {
      select.add(new Option('Chưa có thiết bị', ''));
      select.disabled = true;
      state.selectedId = '';
    } else {
      select.disabled = false;
      state.devices.forEach((device) => select.add(new Option(device.name, device.id)));
      if (!state.devices.some((device) => device.id === state.selectedId)) {
        state.selectedId = state.devices[0].id;
      }
      select.value = state.selectedId;
    }
    saveDevices();
    renderDevice();
    renderDeviceList();
    syncSelectedDevice(true);
    renderPushStatus();
    syncDeviceSelectorUi();
  }

  function closeDeviceSelectorPanel() {
    const panel = $('deviceSelectorPanel');
    const trigger = $('deviceSelectorTrigger');
    if (!panel || panel.hidden) return;
    panel.hidden = true;
    trigger?.setAttribute('aria-expanded', 'false');
  }

  function openDeviceSelectorPanel() {
    const panel = $('deviceSelectorPanel');
    const trigger = $('deviceSelectorTrigger');
    if (!panel || !trigger || trigger.disabled) return;
    panel.hidden = false;
    trigger.setAttribute('aria-expanded', 'true');
    panel.querySelector('[aria-selected="true"]')?.focus();
  }

  function syncDeviceSelectorUi() {
    const select = $('deviceSelector');
    const trigger = $('deviceSelectorTrigger');
    const label = $('deviceSelectorLabel');
    const panel = $('deviceSelectorPanel');
    if (!select || !trigger || !label || !panel) return;
    trigger.disabled = select.disabled;
    const current = select.options[select.selectedIndex];
    label.textContent = current ? current.textContent : 'Chưa có thiết bị';
    panel.replaceChildren();
    Array.from(select.options).forEach((opt) => {
      const btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'customSelectOption';
      btn.setAttribute('role', 'option');
      btn.textContent = opt.textContent;
      btn.dataset.value = opt.value;
      const selected = opt.value === select.value;
      btn.setAttribute('aria-selected', selected ? 'true' : 'false');
      btn.addEventListener('click', () => {
        if (select.value !== opt.value) {
          select.value = opt.value;
          select.dispatchEvent(new Event('change', { bubbles: true }));
        }
        closeDeviceSelectorPanel();
        trigger.focus();
      });
      panel.append(btn);
    });
  }

  function renderDevice() {
    notesUi?.contextChanged();
    const device = currentDevice();
    if (device?.config) device.features.humidifierInstalled = bool(device.config.humidifierInstalled);
    syncHumidifierFeatureUi(device?.config || {
      humidifierInstalled: bool(device?.features?.humidifierInstalled) || bool(device?.snapshot?.runtime?.humidifierOn)
    });
    syncVentilationFeatureUi(device?.config);
    const connection = connectionStatus(device);
    const pill = $('onlinePill');

    $('sideDevice').textContent = device?.name || 'Chưa có thiết bị';
    refreshDeviceNameIfNeeded(device);

    const labels = {
      online: ['TRỰC TUYẾN', 'online', 'Wi‑Fi đã kết nối'],
      degraded: ['DỮ LIỆU CHẬM', 'soft', 'Chờ dữ liệu mới từ máy'],
      cache: ['ĐANG ĐỒNG BỘ', 'soft', state.realtimeConnected ? 'Đã nối máy chủ · chờ máy' : 'Đang nối máy chủ…'],
      connecting: ['ĐANG KẾT NỐI', 'soft', 'Đang nối máy chủ…'],
      waiting: ['CHỜ THIẾT BỊ', 'soft', 'Đã nối máy chủ · chờ máy'],
      offline: ['NGOẠI TUYẾN', 'offline', 'Máy đã ngắt kết nối'],
      none: ['CHƯA CÓ MÁY', 'soft', 'Thêm máy để bắt đầu']
    };
    const [label, css, connectionDetail] = labels[connection];
    const detail = ['online', 'degraded'].includes(connection) && !controlGrantReady(device) && state.realtimeSessionState !== 'auth-required'
      ? `${connection === 'degraded' ? 'Chờ dữ liệu · ' : ''}Đang chuẩn bị quyền điều khiển…` : connectionDetail;
    pill.textContent = label;
    pill.className = `pill ${css}`;
    $('wifiConnectionText').textContent = state.realtimeSessionState === 'auth-required' && device
      ? `${detail} · ghép nối lại để điều khiển` : state.realtimeSessionState === 'error' && !state.realtime
        ? 'Chưa nối máy chủ · đang thử lại' : detail;
    $('sideStatus').textContent = label;
    if ($('dataFreshness')) {
      $('dataFreshness').textContent = freshnessLabel(device);
      $('dataFreshness').title = freshnessText(device);
      $('dataFreshness').setAttribute('aria-label', freshnessText(device));
    }
    document.body.dataset.connection = connection;
    document.querySelectorAll('form.stackForm button[type="submit"], #quickForm button[type="submit"]').forEach((button) => {
      const reminder = button.closest('form')?.id === 'remindersForm';
      button.disabled = reminder
        ? (!device?.id || device.accountRole==='viewer' || Boolean(device.remindersPending))
        : (!controlReady(device) || !device?.config);
    });
    // Defaults must not become an accidental patch while lazy config is loading.
    // Readonly quick fields still accept focus, which initiates their lazy read.
    for (const formId of ['quickForm', 'temperatureForm', 'ventForm', 'turningForm',
      'sensorForm', 'lightAlarmForm', 'humidifierForm', 'advancedForm']) {
      $(formId)?.querySelectorAll('input,select').forEach(input => {
        const unsupportedAdaptive = input.id === 'adaptiveThermalBalanceEnabled' &&
          typeof device?.config?.adaptiveThermalBalanceEnabled !== 'boolean';
        if (unsupportedAdaptive) { input.disabled = true; return; }
        if (input.type === 'checkbox' || input.tagName === 'SELECT') input.disabled = !device?.config;
        else input.readOnly = !device?.config;
      });
    }
    if (!device?.config) for (const id of ['quickTarget', 'quickTurn']) {
      $(id).value = '';
      $(id).placeholder = '—';
      $(id).title = 'Chạm để đọc thông số từ máy';
    }
    if ($('batchTarget')) $('batchTarget').readOnly = !device?.config;

    // Safety thresholds are immutable during a running/resume-pending batch.
    // SV remains editable; buildConfig() re-anchors the full thermal envelope.
    const thermalSafetyLocked = Boolean(device?.snapshot?.runtime?.batchRunning ||
                                        device?.snapshot?.runtime?.resumeConfirmationRequired);
    for (const id of ['lowAlarm', 'highAlarm', 'emergencyTemp']) {
      const input = $(id);
      if (input) input.readOnly = !device?.config || thermalSafetyLocked;
    }
    const outsideBatchAlarm = $('highTempAlarmWithoutBatch');
    if (outsideBatchAlarm) outsideBatchAlarm.disabled = !device?.config || thermalSafetyLocked;

    const ssid = device?.presence?.ssid || '';
    $('wifiSettingSummary').textContent = connection === 'online'
      ? (ssid ? `Đang kết nối: ${ssid}` : 'Wi‑Fi đã kết nối')
      : 'Đổi mạng Wi‑Fi trên màn hình máy';

    applySnapshotToUi(device);
    renderBatchLogs();
    renderFirmwareCard(device);
  }

  // Muc "Cap nhat firmware" trong Cai dat: lay phien ban dang chay tu truong
  // "fw" da co san trong ban tin presence (WebSocket, retained) - khong can them
  // gi phia firmware. Phien ban MOI NHAT lay tu Cloudflare (Worker tu doc
  // GitHub Releases cua repo nay, xem cloudflare/src/index.js). Chi hien nut
  // "Cap nhat" khi THAT SU co ban moi hon; nguoc lai hien thong tin thiet bi.
  let firmwareLatestCache = null;
  let firmwareLatestFetchedAt = 0;
  const FIRMWARE_LATEST_TTL_MS = 5 * 60 * 1000;

  function isWebFirmwareNewer(a, b) {
    if (!a || !b) return false;
    const pa = String(a).split('.').map((n) => parseInt(n, 10) || 0);
    const pb = String(b).split('.').map((n) => parseInt(n, 10) || 0);
    for (let i = 0; i < 3; i += 1) {
      const diff = (pa[i] || 0) - (pb[i] || 0);
      if (diff !== 0) return diff > 0;
    }
    return false;
  }

  async function refreshFirmwareLatestIfNeeded() {
    const now = Date.now();
    if (firmwareLatestFetchedAt && (now - firmwareLatestFetchedAt) < FIRMWARE_LATEST_TTL_MS) return;
    firmwareLatestFetchedAt = now;
    const result = await getCloudJson('/api/firmware/latest');
    if (result.success) {
      firmwareLatestCache = result.version ? { version: result.version, notes: result.notes || '' } : null;
      renderFirmwareCard();
    }
  }

  // Nhac dinh ky (12h/lan, tinh rieng cho tung thiet bi qua localStorage - chi
  // luu tren trinh duyet nay, khong dong bo giua cac thiet bi dang xem cung
  // dashboard) - tranh lam phien nguoi dung moi lan trang tu render lai, chi
  // hien toast noi bat 1 lan roi im lang toi khi du 12h.
  const FIRMWARE_NOTICE_INTERVAL_MS = 12 * 60 * 60 * 1000;
  function maybeNotifyFirmwareUpdate(device, latestVersion) {
    try {
      const key = `mayap_fw_notice_${device.id}`;
      const last = Number(localStorage.getItem(key) || 0);
      if (last && (Date.now() - last) < FIRMWARE_NOTICE_INTERVAL_MS) return;
      localStorage.setItem(key, String(Date.now()));
      toast(`🔔 Có bản phần mềm mới v${latestVersion} cho ${device.name || device.id}`, 6000);
    } catch (error) {
      // localStorage co the bi chan (che do rieng tu, cai dat trinh duyet...)
      // - khong lam gi them, chi la mat tinh nang nhac dinh ky, khong loi.
    }
  }

  function renderFirmwareCard(deviceArg) {
    const device = deviceArg || currentDevice();
    const summaryEl = $('firmwareSummary');
    const bodyEl = $('firmwareBody');
    const dotEl = $('firmwareUpdateDot');
    if (!summaryEl || !bodyEl) return;
    if (!device) {
      summaryEl.textContent = 'Chờ kết nối máy';
      bodyEl.innerHTML = '';
      if (dotEl) dotEl.hidden = true;
      return;
    }

    if (document.body.dataset.page === 'settings') refreshFirmwareLatestIfNeeded();
    const currentVersion = device.presence?.fw || '';
    if (!currentVersion) {
      summaryEl.textContent = 'Đang chờ đồng bộ từ máy…';
      bodyEl.innerHTML = '<p class="settingFootnote">Chưa nhận được thông tin phiên bản từ thiết bị.</p>';
      if (dotEl) dotEl.hidden = true;
      return;
    }

    const latest = firmwareLatestCache;
    const hasUpdate = latest && isWebFirmwareNewer(latest.version, currentVersion);
    if (dotEl) dotEl.hidden = !hasUpdate;
    // Nut "Quay lai ban truoc" LUON hien (khong phu thuoc co ban moi hay
    // khong) - ESP32 giu san 1 ban firmware truoc do trong vi tri OTA con
    // lai (thiet ke phan cung 2-vi-tri), muc nay chi la doi con tro khoi
    // dong ve do. Khong biet truoc tren web thiet bi co THAT SU con ban de
    // quay lai khong (chi co the kiem tra tren chinh ESP32) nen cu hien nut,
    // neu khong co gi de quay lai thi ACK se bao ro "KHONG CO BAN CU DE
    // QUAY LAI" (xem RAW_ACK_MESSAGES) thay vi an nut di truoc.
    const rollbackButtonHtml = '<button class="dangerButton full" id="firmwareRollbackBtn" type="button">Quay lại phần mềm trước đó</button>';
    if (hasUpdate) {
      summaryEl.textContent = `Có bản mới: v${latest.version}`;
      bodyEl.innerHTML = `<p class="settingFootnote">Đang chạy v${escapeHtml(currentVersion)} · có bản v${escapeHtml(latest.version)} mới hơn.</p><button class="primary full" id="firmwareUpdateBtn" type="button">Cập nhật lên v${escapeHtml(latest.version)}</button>${rollbackButtonHtml}`;
      $('firmwareUpdateBtn')?.addEventListener('click', () => {
        sendCommand('firmware_check_now');
        toast('Đã yêu cầu kiểm tra. Trên màn hình máy: Cài đặt chung → Hệ thống → Cập nhật. Dừng mẻ và tắt công tắc nhiệt trước khi xác nhận cập nhật.', 8000);
      });
      maybeNotifyFirmwareUpdate(device, latest.version);
    } else if (latest) {
      summaryEl.textContent = `Phiên bản v${currentVersion} · đã mới nhất`;
      bodyEl.innerHTML = `<p class="settingFootnote">Đang chạy phiên bản v${escapeHtml(currentVersion)} - đây đã là bản mới nhất.</p>${rollbackButtonHtml}`;
    } else {
      summaryEl.textContent = `Phiên bản v${currentVersion} · chưa xác định bản mới nhất`;
      bodyEl.innerHTML = `<p class="settingFootnote">Chưa lấy được danh sách phiên bản. Kiểm tra lại trên màn hình máy: Cài đặt chung → Hệ thống → Cập nhật.</p>${rollbackButtonHtml}`;
    }
    $('firmwareRollbackBtn')?.addEventListener('click', () => {
      // Rollback chi duoc xac nhan vat ly tren HMI; web khong gui WebSocket.
      toast('Quay lại phần mềm phải xác nhận trực tiếp trên màn hình máy: Cài đặt chung → Hệ thống → Cập nhật → Quay lại bản cũ.', 6500);
    });
  }

  function renderDeviceList() {
    const root = $('deviceList');
    root.replaceChildren();
    state.devices.forEach((device) => {
      const row = document.createElement('div');
      row.className = 'deviceListItem';
      const text = document.createElement('div');
      text.innerHTML = `<strong>${escapeHtml(device.name)}</strong><small>${escapeHtml(device.id)}</small>`;
      const remove = document.createElement('button');
      remove.type = 'button';
      remove.textContent = 'Xóa cache';
      remove.addEventListener('click', async () => {
        const ok = await confirmAction({
          title: 'Xóa dữ liệu đã lưu?',
          message: `Cache của ${device.name} trên trình duyệt sẽ được xóa. Máy vẫn thuộc tài khoản và dữ liệu live sẽ đồng bộ lại.`,
          accept: 'Xóa cache'
        });
        if (!ok) return;
        try { localStorage.removeItem(`${RUNTIME_CACHE}.${device.id}`); } catch (_) {}
        toast('Đã xóa cache trên trình duyệt');
      });
      row.append(text, remove);
      root.append(row);
    });
  }

  function updateOutput(id, on, onText = 'BẬT', offText = 'TẮT') {
    const element = $(id);
    if (!element) return;
    element.textContent = on ? onText : offText;
    element.classList.toggle('on', Boolean(on));
    element.parentElement?.classList.toggle('on', Boolean(on));
  }

  function clearBatchActionPending(device) {
    if (!device) return;
    device.batchUiPendingTarget = '';
    device.batchUiPendingUntil = 0;
  }

  // Anh xa action WebSocket -> trang thai batchRunning mong doi, dung de doi
  // chieu voi snapshot runtime (xem batchUiAwaitingConfirmTarget ben duoi).
  function batchTargetForAction(action) {
    if (action === 'batch_start') return 'running';
    if (action === 'batch_stop') return 'stopped';
    return '';
  }

  function renderBatchAction(device, runtime = device?.snapshot?.runtime) {
    const button = $('batchAction');
    if (!button) return;

    const running = Boolean(runtime?.batchRunning);
    const pending = Boolean(
      device?.batchUiPendingTarget &&
      Number(device.batchUiPendingUntil || 0) > Date.now()
    );

    if (pending) {
      const starting = device.batchUiPendingTarget === 'running';
      button.disabled = true;
      button.textContent = starting ? 'Đang bắt đầu…' : 'Đang kết thúc…';
      button.className = starting ? 'primary dark' : 'dangerButton';
      button.setAttribute('aria-label', button.textContent);
      return;
    }

    clearBatchActionPending(device);
    button.disabled = !controlReady(device);
    button.textContent = running ? 'Kết thúc' : 'Bắt đầu';
    button.className = running ? 'dangerButton' : 'primary dark';
    button.setAttribute('aria-label', running ? 'Kết thúc mẻ ấp' : 'Bắt đầu mẻ ấp');
  }

  function beginBatchActionPending(device, target) {
    if (!device) return;
    device.batchUiPendingTarget = target;
    // Huy moi "cho xac nhan tre" cua lan bam truoc - lenh moi nay se co ket
    // qua rieng cua no (xem batchUiAwaitingConfirmTarget o applySnapshotToUi()).
    device.batchUiAwaitingConfirmTarget = '';
    device.batchUiPendingUntil = Date.now() + Math.max(10000, Number(WEB.commandTimeoutMs || 0) + 2000);
    const expectedUntil = device.batchUiPendingUntil;
    renderBatchAction(device);
    setTimeout(() => {
      if (device.batchUiPendingUntil !== expectedUntil || Date.now() < expectedUntil) return;
      clearBatchActionPending(device);
      if (device.id === state.selectedId) renderBatchAction(device);
    }, Math.max(0, expectedUntil - Date.now() + 50));
  }

  function applySnapshotToUi(device) {
    const runtime = device?.snapshot?.runtime;
    if (!runtime) {
      $('liveTemp').textContent = '—';
      $('liveHumidity').textContent = '—';
      renderLiveState(device, null);
      updateOutput('outputHeater', false);
      updateOutput('outputCirculation', false);
      updateOutput('outputVent', false);
      updateOutput('outputHumidifier', false);
      updateOutput('outputTurn', false, 'ĐANG ĐẢO', 'CHỜ');
      updateOutput('outputLight', false);
      updateOutput('outputSiren', false);
      if ($('sirenActionHint')) $('sirenActionHint').textContent = 'Chỉ khi còi đang kêu';
      if ($('outputLightBtn')) $('outputLightBtn').disabled = true;
      if ($('outputSirenBtn')) $('outputSirenBtn').disabled = true;
      // F-10: chua co snapshot = chua biet may co dang chay me hay khong -
      // khoa "Tong so ngay ap" theo huong an toan (gia dinh co the dang chay)
      // thay vi mac dinh cho sua.
      if ($('totalDays')) $('totalDays').disabled = true;
      setCurrentActivity('Chưa có dữ liệu vận hành', 'Đang chờ dữ liệu từ máy', 'idle');
      renderBatchAction(device, null);
      return;
    }

    $('liveTemp').textContent = `${numberVi(runtime.temperature)}°C`;
    $('liveHumidity').textContent = `${numberVi(runtime.humidity, 0)}%`;
    renderLiveState(device, runtime);
    // Thanh nhiet dong cat theo burst PDM; cong suat la yeu cau trung binh, trang thai SSR
    // tuc thoi (heaterOn) nhap nhay rat nhanh - snapshot 400ms-6s de bat trung
    // luc OFF giua 2 xung, nguoi dung thay "may dang nong" nhung wed bao OFF.
    // heaterPower > 0 phan anh dung y dinh dieu khien (PID dang can nhiet),
    // on dinh hon nhieu cho hien thi cho nguoi dung.
    const heaterActive = runtime.heaterPower == null ? bool(runtime.heaterOn) : Number(runtime.heaterPower) > 0;
    updateOutput('outputHeater', heaterActive);
    updateOutput('outputCirculation', bool(runtime.circulationFanOn));
    updateOutput('outputVent', bool(runtime.ventFanOn));
    updateOutput('outputHumidifier', bool(runtime.humidifierOn));
    updateOutput('outputLight', bool(runtime.lightOn));
    updateOutput('outputSiren', bool(runtime.sirenOn));
    if ($('sirenActionHint')) $('sirenActionHint').textContent = bool(runtime.sirenOn)
      ? 'Chạm để tạm tắt' : 'Chỉ khi còi đang kêu';
    // Nut Den bam duoc bat cu luc nao thiet bi online; nut Coi CHI bam duoc
    // khi coi dang thuc su keu (giong het dieu kien mo man Alarm tren HMI).
    if ($('outputLightBtn')) $('outputLightBtn').disabled = !controlReady(device);
    if ($('outputSirenBtn')) {
      $('outputSirenBtn').disabled = !controlReady(device) || !bool(runtime.sirenOn);
    }

    const turnMap = { 0: 'DỪNG', 1: 'TRÁI', 2: 'PHẢI', 3: 'CHỜ', 4: 'LỖI' };
    const turn = Number(runtime.turnState);
    const outputTurn = $('outputTurn');
    // Dang dao (1/2) thi khong con "lan dao tiep theo" de dem nguoc; chi thay
    // "CHO" bang so phut khi may dang CHO (3) va ESP32 that su co lich dao ke
    // tiep (nextTurnMinutes > 0 - bang 0 nghia la dang khong lap lich, vd dao
    // tay hoac dang o nhanh test tat dao) - van gon 1 dong nhu cac o khac.
    const etaMinutes = Number(runtime.nextTurnMinutes) || 0;
    outputTurn.textContent = (turn === 3 && etaMinutes > 0)
      ? `Đảo sau ${etaMinutes} phút`
      : (turnMap[turn] || '—');
    outputTurn.classList.toggle('on', turn === 1 || turn === 2);
    outputTurn.parentElement?.classList.toggle('on', turn === 1 || turn === 2);

    $('batchPill').textContent = runtime.batchRunning ? `NGÀY ${runtime.currentDay || 1}` : 'CHƯA BẮT ĐẦU';
    $('batchPill').className = runtime.batchRunning ? 'pill online' : 'pill soft';

    // F-10 (audit truoc phat hanh v3.7.1): HMI da khoa "Tong so ngay ap"
    // trong luc dang chay me (settingLockedDuringBatch() trong hmi.h), nhung
    // web truoc day KHONG khoa - nguoi dung sua o day bi ESP32 tu choi ca
    // giao dich (bao gom moi thay doi khac trong cung form), khong ro ly do.
    // Khoa ngay tren giao dien de khop voi HMI, tranh gap phai tinh huong do.
    const totalDaysInput = $('totalDays');
    if (totalDaysInput) {
      totalDaysInput.disabled = Boolean(runtime.batchRunning);
      totalDaysInput.title = runtime.batchRunning
        ? 'Đang có mẻ chạy - khoá số ngày ấp (giống trên máy), dừng mẻ để đổi'
        : '';
    }

    if (device.dataSource !== 'live' || device.liveEpoch !== state.subscriptionEpoch || !isDeviceOnline(device)) {
      if (totalDaysInput) totalDaysInput.disabled = true;
      renderBatchAction(device, runtime);
      setCurrentActivity('Đang đồng bộ…', freshnessText(device), 'idle');
      return;
    }

    if ((device?.batchUiPendingTarget === 'running' && runtime.batchRunning) ||
        (device?.batchUiPendingTarget === 'stopped' && !runtime.batchRunning)) {
      clearBatchActionPending(device);
    }
    // Runtime snapshot la SU THAT tu may, doc lap hoan toan voi duong ACK.
    // Neu 1 lenh Bat dau/Ket thuc me truoc do da bi bao loi/het han tren web
    // (batchUiAwaitingConfirmTarget con giu lai muc tieu cua lenh do - xem
    // sendCommand()/handleAck()) nhung snapshot nay lai cho thay may THUC RA
    // da thuc hien dung (mang cham lam goi ACK that lac/den tre hon 8-10s so
    // voi may da lam xong), phai SUA LAI thanh thong bao THANH CONG thay vi
    // de nguyen canh bao sai tren form - dung yeu cau "gui thanh cong hay
    // khong thanh cong deu phai bao ro rang", khong duoc de lai thong bao
    // trai voi thuc te.
    if ((device?.batchUiAwaitingConfirmTarget === 'running' && runtime.batchRunning) ||
        (device?.batchUiAwaitingConfirmTarget === 'stopped' && !runtime.batchRunning)) {
      const startedNow = device.batchUiAwaitingConfirmTarget === 'running';
      const lateOkMessage = startedNow
        ? 'Trạng thái máy cho thấy mẻ đã bắt đầu; máy chưa gửi xác nhận thao tác'
        : 'Trạng thái máy cho thấy mẻ đã kết thúc; máy chưa gửi xác nhận thao tác';
      device.batchUiAwaitingConfirmTarget = '';
      if (device.id === state.selectedId) setFormError('batchForm', '');
      addBatchLog(device, lateOkMessage);
      if (device.id === state.selectedId) toast(lateOkMessage, 4500);
    }
    renderBatchAction(device, runtime);

    if (runtime.adaptiveThermal && $('adaptiveThermalStatus')) {
      const names = ['Tắt', 'Đang học', 'Đã đủ dữ liệu', 'Đang cân bằng', 'Đang tự làm mát', 'Đang học lại', 'Tạm ngưng'];
      const adapt = runtime.adaptiveThermal;
      $('adaptiveThermalStatus').textContent = names[Number(adapt.state)] || 'Tạm ngưng';
    }
    const autoTuneState = Number(runtime.autoTuneState || 0);
    const autoTuneProgress = Math.max(0, Math.min(100, Number(runtime.autoTuneProgress || 0)));
    $('tuneBar').style.width = `${autoTuneProgress}%`;
    if (autoTuneState === 1) {
      $('tuneText').textContent = `Đang chạy · ${autoTuneProgress}%`;
      $('pidSummary').textContent = `Đang tự dò · ${autoTuneProgress}%`;
      $('startTune').disabled = false;
      $('startTune').textContent = 'Hủy tự dò PID';
      $('startTune').classList.remove('primary');
      $('startTune').classList.add('dangerButton');
    } else {
      $('startTune').disabled = false;
      $('startTune').textContent = 'Bắt đầu tự dò PID';
      $('startTune').classList.remove('dangerButton');
      $('startTune').classList.add('primary');
      if (autoTuneState === 2) {
        $('tuneText').textContent = 'Hoàn tất · thông số đã được máy lưu';
        $('pidSummary').textContent = 'Đã hoàn tất và tự lưu';
      } else if (autoTuneState === 3) {
        $('tuneText').textContent = 'Tự dò không hoàn tất';
        $('pidSummary').textContent = 'Tự dò thất bại';
      } else {
        $('tuneText').textContent = 'Sẵn sàng';
        $('pidSummary').textContent = 'Máy tự tìm và lưu thông số';
      }
    }

    if (autoTuneState === 1) {
      setCurrentActivity('Đang tự dò PID', `Tiến độ ${autoTuneProgress}%`, 'warning');
    } else if (turn === 1 || turn === 2) {
      setCurrentActivity(`Đang đảo trứng sang ${turn === 1 ? 'trái' : 'phải'}`, 'Đang chờ công tắc hành trình', 'active');
    } else if (runtime.ventFanOn) {
      setCurrentActivity('Quạt thông gió đang chạy', `Nhiệt độ ${numberVi(runtime.temperature)}°C`, 'active');
    } else if (heaterActive) {
      setCurrentActivity(`Đang gia nhiệt ${numberVi(runtime.temperature)}°C`, `Công suất ${numberVi(runtime.heaterPower, 0)}%`, 'active');
    } else if (runtime.batchRunning) {
      setCurrentActivity(`Đang giữ nhiệt ${numberVi(runtime.temperature)}°C`, `Còn ${runtime.nextTurnMinutes || 0} phút tới lần đảo`, 'active');
    } else {
      setCurrentActivity('Sẵn sàng', runtime.machineState || 'Không có tác vụ đang chạy', 'idle');
    }

    if (runtime.resumeConfirmationRequired && device.bootId && state.lastResumePromptBootId !== device.bootId) {
      state.lastResumePromptBootId = device.bootId;
      promptResumeAfterPower(device);
    }

    // F-06 (audit trước phát hành v3.7.1): mẻ quá số ngày ấp dự kiến - máy
    // đã tự bật còi + đếm ngược tự dừng 12h (xem updateBatchOverdue() trong
    // machine_control.h); chỉ hỏi 1 LẦN cho mỗi lần chuyển từ "chưa quá hạn"
    // sang "quá hạn" (không hỏi lại mỗi vài giây theo nhịp snapshot).
    if (runtime.batchOverdueConfirmationPending) {
      if (!device.overdueContinuePrompted) {
        device.overdueContinuePrompted = true;
        promptBatchOverdueContinue(device);
      }
    } else {
      device.overdueContinuePrompted = false;
    }
  }

  async function promptResumeAfterPower(device) {
    const continueBatch = await confirmAction({
      title: 'Tiếp tục mẻ sau mất điện?',
      message: 'Máy phát hiện mẻ ấp đang chờ xác nhận sau khi có điện trở lại.',
      accept: 'Tiếp tục mẻ'
    });
    await sendCommand(continueBatch ? 'resume_yes' : 'resume_no', { device });
  }

  async function promptBatchOverdueContinue(device) {
    const keepGoing = await confirmAction({
      title: 'Mẻ ấp đã quá số ngày dự kiến',
      message: 'Máy đã ấp quá số ngày đã cấu hình. Bạn có muốn tiếp tục ủ ấm không? Nếu không xác nhận, máy sẽ tự động dừng sau 12 giờ.',
      accept: 'Tiếp tục ủ ấm'
    });
    // Chon "Huy": khong gui lenh gi ca - coi van keu lai dinh ky (tu tam tat
    // duoc qua nut ACK/coi bao), va nguoi dung van bam "Kết thúc mẻ" binh
    // thuong bat cu luc nao neu muon dung ngay thay vi cho du 12h.
    if (keepGoing) await sendCommand('batch_overdue_continue', { device });
  }

  function updateSettingSummaries() {
    $('temperatureSummary').textContent = `Đặt ${numberVi($('targetTemp').value)}°C · ngắt khẩn ${numberVi($('emergencyTemp').value)}°C`;
    $('ventSummary').textContent = currentDevice()?.config && !supportsVentProfile(currentDevice().config)
      ? 'Điều khiển theo ngưỡng nhiệt độ' : $('ventAutoEnabled').checked
      ? `Theo ngày ấp · ${['Thấp', 'Tiêu chuẩn', 'Cao'][Number($('ventProfileLevel').value)] || 'Tiêu chuẩn'}`
      : 'Tự động theo ngày ấp đang tắt';
    $('turningSummary').textContent = $('turningEnabled').checked
      ? `Tự động · mỗi ${$('turnInterval').value || '—'} phút`
      : 'Đang tắt đảo tự động';
    $('sensorSummary').textContent = `Bù ${numberVi($('tempOffset').value)}°C · ngắt nhiệt khi mất cảm biến`;
    $('humidifierSummary').textContent = `Bật ≤${$('humidifierOnHumidity').value || '—'}% · tắt ≥${$('humidifierOffHumidity').value || '—'}%RH`;
  }

  function supportsVentProfile(config) {
    return Boolean(config && VENT_PROFILE_KEYS.every((key) => Object.hasOwn(config, key)));
  }

  function syncVentilationFeatureUi(config) {
    const available = supportsVentProfile(config);
    $('ventProfileFields').disabled = !available;
    $('ventAvailability').hidden = available;
    $('ventAvailability').textContent = !config
      ? 'Đang chờ cài đặt từ máy.'
      : 'Máy đang dùng phần mềm cũ. Bạn vẫn có thể chỉnh ngưỡng nhiệt độ; cập nhật phần mềm máy để dùng thông gió theo ngày ấp.';
    if (config && !available) $('ventSummary').textContent = 'Điều khiển theo ngưỡng nhiệt độ';
  }

  function swipeDestination(page, dx, dy, elapsed) {
    if (elapsed > 700 || Math.abs(dx) < 60 || Math.abs(dx) < Math.abs(dy) * 1.5) return null;
    const pages = ['device', 'batch', 'settings'];
    return pages[pages.indexOf(page) + (dx < 0 ? 1 : -1)] || null;
  }

  function bindMobileSwipe() {
    const root = document.querySelector('.main');
    let gesture = null, frame = 0, settling = false;
    const clearDrag = (page) => {
      page?.style.removeProperty('transform');
      page?.style.removeProperty('will-change');
    };
    const settle = async (start, next = null) => {
      if (!start) return;
      cancelAnimationFrame(frame); frame = 0;
      const page = start.element, from = start.page;
      const canNavigate = () => document.body.dataset.page === from && !document.querySelector('dialog[open]');
      const reduced = window.matchMedia?.('(prefers-reduced-motion: reduce)')?.matches;
      if (!start.horizontal || reduced || !page.animate) {
        clearDrag(page);
        if (next && canNavigate()) showPage(next);
        return;
      }
      settling = true;
      try {
        const offset = start.offset || 0;
        clearDrag(page);
        if (next && canNavigate()) {
          showPage(next);
          const incoming = $(`page-${next}`);
          const distance = Math.min(48, Math.max(24, Math.abs(offset)));
          await incoming.animate([
            { transform: `translateX(${-Math.sign(start.dx) * distance}px)` },
            { transform: 'translateX(0)' }
          ], { duration: 100, easing: 'cubic-bezier(.16,1,.3,1)' }).finished;
        } else {
          await page.animate([
            { transform: `translateX(${offset}px)` },
            { transform: 'translateX(0)' }
          ], { duration: 80, easing: 'cubic-bezier(.16,1,.3,1)' }).finished;
        }
      } catch (e) { /* Cancelled animation leaves the current tab usable. */ }
      finally { clearDrag(page); settling = false; }
    };
    const cancel = () => {
      const start = gesture; gesture = null;
      settle(start);
    };
    root.addEventListener('pointerdown', (event) => {
      if (gesture) { cancel(); return; }
      if (settling || event.pointerType !== 'touch' || !event.isPrimary ||
          !window.matchMedia('(max-width:800px), (max-height:500px)').matches ||
          event.clientX < 24 || event.clientX > innerWidth - 24 ||
          document.querySelector('dialog[open]') ||
          event.target.closest('input, textarea, select, button, a, label, summary, canvas, .tile-clickable, [contenteditable], [role="listbox"], [role="dialog"]')) return;
      gesture = { id: event.pointerId, x: event.clientX, y: event.clientY, at: performance.now(),
        page: document.body.dataset.page, element: document.querySelector('.page.active'), dx: 0, offset: 0, horizontal: false };
    }, { passive: true });
    root.addEventListener('pointermove', (event) => {
      if (!gesture || event.pointerId !== gesture.id) return;
      gesture.dx = event.clientX - gesture.x;
      const dx = Math.abs(gesture.dx), dy = Math.abs(event.clientY - gesture.y);
      if (dy > 12 && dy > dx) { cancel(); return; }
      if (dx > 8 && dx > dy * 1.5) gesture.horizontal = true;
      if (!gesture.horizontal) return;
      if (window.matchMedia?.('(prefers-reduced-motion: reduce)')?.matches) return;
      gesture.offset = Math.max(-96, Math.min(96, gesture.dx * .6));
      if (!frame) frame = requestAnimationFrame(() => {
        frame = 0;
        if (!gesture?.horizontal) return;
        gesture.element.style.willChange = 'transform';
        gesture.element.style.transform = `translateX(${gesture.offset}px)`;
      });
    }, { passive: true });
    root.addEventListener('pointerup', (event) => {
      const start = gesture;
      gesture = null;
      if (!start || start.id !== event.pointerId) { clearDrag(start?.element); return; }
      const next = swipeDestination(document.body.dataset.page, event.clientX - start.x,
        event.clientY - start.y, performance.now() - start.at);
      settle(start, next);
    }, { passive: true });
    root.addEventListener('pointercancel', cancel, { passive: true });
    window.addEventListener('blur', cancel);
    window.addEventListener('resize', cancel, { passive: true });
  }

  function hasDirtyForm(formId) {
    return Boolean(state.formFlags.get(formId)?.dirty);
  }

  function syncHumidifierFeatureUi(config) {
    const installed = Boolean(config?.humidifierInstalled);
    if ($('outputHumidifierTile')) $('outputHumidifierTile').hidden = !installed;
    $('outputStrip')?.classList?.toggle('hasHumidifier', installed);
    $('humidifierSetting').hidden = !installed;
    if (!installed) $('humidifierSetting').open = false;
    $('batchHumidityTile').hidden = installed;
    $('batchGrid')?.classList?.toggle('noHumidity', installed);
  }

  function applyConfigToUi(device, force = false) {
    const config = device?.config;
    if (!config) return;
    syncHumidifierFeatureUi(config);
    syncVentilationFeatureUi(config);
    const assign = (formId, id, value) => {
      if (!force && hasDirtyForm(formId)) return;
      const element = $(id);
      if (!element) return;
      // So thuc (float) ben ESP32 (32-bit) khong bieu dien chinh xac tuyet
      // doi cac gia tri thap phan (vd 30.1 luu thanh 30.099999904632568...),
      // lo ra khi gan thang vao o input dang so ("37,00000001..." rat kho
      // chiu). Lam tron ve toi da 3 chu so thap phan (moi truong nay chi
      // dung toi 1 chu so) truoc khi gan de xoa het phan du sai so.
      if (typeof value === 'number' && Number.isFinite(value)) {
        value = Math.round(value * 1000) / 1000;
      }
      element.value = value;
    };
    const check = (formId, id, value) => {
      if (!force && hasDirtyForm(formId)) return;
      const element = $(id);
      if (element) element.checked = Boolean(value);
    };

    assign('quickForm', 'quickTarget', config.targetTemp);
    assign('quickForm', 'quickTurn', config.turnIntervalMin);

    assign('batchForm', 'batchTarget', config.targetTemp);
    assign('batchForm', 'totalDays', config.totalIncubationDays);
    check('batchForm', 'resumeAfterPowerLoss', config.autoResumeAfterPower);
    check('humidifierForm', 'humidifierEnabled', config.humidifierEnabled);
    assign('humidifierForm', 'humidifierOffHumidity', config.targetHumidity);
    assign('humidifierForm', 'humidifierOnHumidity',
      Math.round(Number(config.targetHumidity) - Number(config.humidifierHysteresisRh ?? 2)));
    if (!hasDirtyForm('batchForm') || force) {
      $('batchName').value = device.batchMeta.name;
      $('startDate').value = device.batchMeta.startDate;
      if (!config.humidifierInstalled) $('targetHumidity').value = device.batchMeta.targetHumidity;
    }

    assign('temperatureForm', 'targetTemp', config.targetTemp);
    assign('temperatureForm', 'lowAlarm', config.lowTempAlarm);
    assign('temperatureForm', 'highAlarm', config.highTempAlarm);
    assign('temperatureForm', 'emergencyTemp', config.emergencyTemp);
    check('temperatureForm', 'highTempAlarmWithoutBatch', config.highTempAlarmWithoutBatch);
    assign('ventForm', 'ventOn', config.ventOnTemp);
    assign('ventForm', 'ventOff', config.ventOffTemp);
    $('ventOn').min = (Number(config.targetTemp) + 0.1).toFixed(1);
    $('ventOn').max = Number(config.highTempAlarm).toFixed(1);
    $('ventOff').min = Number(config.targetTemp).toFixed(1);
    $('ventOff').max = Number(config.highTempAlarm).toFixed(1);
    check('ventForm', 'ventAutoEnabled', config.ventAutoEnabled);
    assign('ventForm', 'ventProfileLevel', config.ventProfileLevel ?? 1);
    assign('ventForm', 'ventCycleMinutes', config.ventCycleMinutes ?? 40);
    VENT_PROFILE_KEYS.slice(3).forEach((key, index) =>
      assign('ventForm', key, config[key] ?? [10, 15, 25, 35, 50, 70][index]));
    $('ventAutoOptions').hidden = !$('ventAutoEnabled').checked;

    check('turningForm', 'turningEnabled', config.turningEnabled);
    check('turningForm', 'manualTurnReanchorsSchedule', config.manualTurnReanchorsSchedule);
    assign('turningForm', 'turnInterval', config.turnIntervalMin);
    assign('turningForm', 'limitAlarmTime', config.turnMaxRunSec);
    assign('turningForm', 'nextDirection', Number(config.nextDirection) === 1 ? 'right' : 'left');

    assign('sensorForm', 'tempOffset', config.tempOffset);
    assign('sensorForm', 'humidityOffset', config.humidityOffset);

    check('lightAlarmForm', 'lightAfterBatchAlarmEnabled', config.lightAfterBatchAlarmEnabled);
    check('lightAlarmForm', 'sirenSelfTestEnabled', config.sirenSelfTestEnabled);

    assign('advancedForm', 'advKp', config.kp);
    assign('advancedForm', 'advKi', config.ki);
    assign('advancedForm', 'advKd', config.kd);
    assign('advancedForm', 'advMaxHeaterPower', config.maxHeaterPower);
    const adaptiveSupported = typeof config.adaptiveThermalBalanceEnabled === 'boolean';
    $('adaptiveThermalBalanceEnabled').disabled = !adaptiveSupported;
    check('advancedForm', 'adaptiveThermalBalanceEnabled', config.adaptiveThermalBalanceEnabled === true);
    $('adaptiveThermalStatus').textContent = adaptiveSupported
      ? (config.adaptiveThermalBalanceEnabled ? 'Bật · máy sẽ học trước khi điều chỉnh.' : 'Tắt · sử dụng PID hiện tại.')
      : 'Firmware hiện tại chưa hỗ trợ Tự cân bằng nhiệt.';
    assign('advancedForm', 'advTempRateLimitC', config.tempRateLimitC);
    assign('advancedForm', 'advTempRateWindowSec', config.tempRateWindowSec);
    assign('advancedForm', 'advTempOscillationCrossLimit', config.tempOscillationCrossLimit);
    assign('advancedForm', 'advTempOscillationWindowSec', config.tempOscillationWindowSec);
    assign('advancedForm', 'advHeaterStuckMinRiseC', config.heaterStuckMinRiseC);
    assign('advancedForm', 'advHeaterStuckDurationSec', config.heaterStuckDurationSec);
    assign('advancedForm', 'advAutotuneRelayPowerPercent', config.autotuneRelayPowerPercent);
    assign('advancedForm', 'advAutotuneBandC', config.autotuneBandC);

    ['quickForm', 'batchForm', 'temperatureForm', 'ventForm', 'turningForm', 'sensorForm', 'lightAlarmForm', 'humidifierForm', 'advancedForm'].forEach((formId) => {
      if (force || !hasDirtyForm(formId)) setFormState(formId, 'saved', 'Đã nhận cài đặt từ máy');
    });
    updateSettingSummaries();
  }

  function validateFullConfig(config) {
    return REQUIRED_CONFIG_KEYS.every((key) => Object.prototype.hasOwnProperty.call(config, key)) &&
      Number.isFinite(Number(config.targetTemp));
  }

  function configEquals(left, right) {
    if (!left || !right) return false;
    return CONFIG_KEYS.every((key) => {
      const a = left[key];
      const b = right[key];
      if (typeof a === 'number' || typeof b === 'number') return Math.abs(Number(a) - Number(b)) < 0.0005;
      return a === b;
    });
  }

  // Doi nhiet do dat (SV) qua form nhanh (quick/batch, chi co 1 o SV, khong
  // co cac nguong bao/hut) can keo theo cac nguong lien quan cung 1 khoang
  // dich chuyen - giong het commitSetting() ben firmware (hmi.h) da lam khi
  // doi SV tren man hinh may. Neu khong, cac nguong nay "cu" so voi SV moi,
  // gay loi thuc te: doi SV tu 37.5 xuong 30 qua form nhanh, sau do vao HMI
  // chinh "Bao cao" (dang con la 38.2) thi khong the ha xuong gan SV moi
  // duoc vi bi chinh cai nguong lac hau nay khoa lai.
  function shiftTempThresholds(config, oldTarget, newTarget) {
    const delta = newTarget - oldTarget;
    if (!Number.isFinite(delta) || delta === 0) return;
    config.lowTempAlarm = Number(config.lowTempAlarm) + delta;
    config.highTempAlarm = Number(config.highTempAlarm) + delta;
    config.emergencyTemp = Number(config.emergencyTemp) + delta;
    config.ventOnTemp = Number(config.ventOnTemp) + delta;
    config.ventOffTemp = Number(config.ventOffTemp) + delta;
  }

  function buildConfig(group) {
    const device = currentDevice();
    if (!device?.config || !validateFullConfig(device.config)) {
      toast('Chưa nhận đủ cấu hình từ máy. Hãy chờ máy đồng bộ.');
      return null;
    }
    const config = { ...device.config };
    if (group === 'quick') {
      const newTarget = Number($('quickTarget').value);
      shiftTempThresholds(config, config.targetTemp, newTarget);
      config.targetTemp = newTarget;
      config.turnIntervalMin = Number($('quickTurn').value);
    } else if (group === 'batch') {
      const newTarget = Number($('batchTarget').value);
      shiftTempThresholds(config, config.targetTemp, newTarget);
      config.targetTemp = newTarget;
      config.totalIncubationDays = Number($('totalDays').value);
      config.autoResumeAfterPower = $('resumeAfterPowerLoss').checked;
    } else if (group === 'temperature') {
      const newTarget = Number($('targetTemp').value);
      const thermalSafetyLocked = Boolean(device.snapshot?.runtime?.batchRunning ||
                                          device.snapshot?.runtime?.resumeConfirmationRequired);
      if (thermalSafetyLocked) {
        // Trong me/resume, safety thresholds khong duoc sua truc tiep. Re-anchor
        // ca envelope theo SV moi de MachineController co the chap nhan dung
        // invariant thay vi de nguong cu cach SV hang nhieu do C.
        shiftTempThresholds(config, config.targetTemp, newTarget);
        config.targetTemp = newTarget;
      } else {
        config.targetTemp = newTarget;
        config.lowTempAlarm = Number($('lowAlarm').value);
        config.highTempAlarm = Number($('highAlarm').value);
        config.emergencyTemp = Number($('emergencyTemp').value);
        config.highTempAlarmWithoutBatch = $('highTempAlarmWithoutBatch').checked;
      }
    } else if (group === 'vent') {
      config.ventOnTemp = Number($('ventOn').value);
      config.ventOffTemp = Number($('ventOff').value);
      if (supportsVentProfile(device.config)) {
        config.ventAutoEnabled = $('ventAutoEnabled').checked;
        config.ventProfileLevel = Number($('ventProfileLevel').value);
        config.ventCycleMinutes = Number($('ventCycleMinutes').value);
        for (const key of VENT_PROFILE_KEYS.slice(3)) config[key] = Number($(key).value);
        if (config.ventAutoEnabled) config.ventScheduleEnabled = false;
      }
    } else if (group === 'turning') {
      config.turningEnabled = $('turningEnabled').checked;
      config.manualTurnReanchorsSchedule = $('manualTurnReanchorsSchedule').checked;
      config.turnIntervalMin = Number($('turnInterval').value);
      config.turnMaxRunSec = Number($('limitAlarmTime').value);
      config.nextDirection = $('nextDirection').value === 'right' ? 1 : 0;
    } else if (group === 'sensor') {
      config.tempOffset = Number($('tempOffset').value);
      config.humidityOffset = Number($('humidityOffset').value);
    } else if (group === 'lightAlarm') {
      config.lightAfterBatchAlarmEnabled = $('lightAfterBatchAlarmEnabled').checked;
      config.sirenSelfTestEnabled = $('sirenSelfTestEnabled').checked;
    } else if (group === 'humidifier') {
      if (!config.humidifierInstalled) return null;
      const off = Number($('humidifierOffHumidity').value);
      const on = Number($('humidifierOnHumidity').value);
      config.targetHumidity = off;
      config.humidifierHysteresisRh = off - on;
      config.humidifierEnabled = $('humidifierEnabled').checked;
    } else if (group === 'advanced') {
      config.kp = Number($('advKp').value);
      config.ki = Number($('advKi').value);
      config.kd = Number($('advKd').value);
      config.maxHeaterPower = Number($('advMaxHeaterPower').value);
      if (typeof device.config.adaptiveThermalBalanceEnabled === 'boolean')
        config.adaptiveThermalBalanceEnabled = $('adaptiveThermalBalanceEnabled').checked;
      config.tempRateLimitC = Number($('advTempRateLimitC').value);
      config.tempRateWindowSec = Number($('advTempRateWindowSec').value);
      config.tempOscillationCrossLimit = Number($('advTempOscillationCrossLimit').value);
      config.tempOscillationWindowSec = Number($('advTempOscillationWindowSec').value);
      config.heaterStuckMinRiseC = Number($('advHeaterStuckMinRiseC').value);
      config.heaterStuckDurationSec = Number($('advHeaterStuckDurationSec').value);
      config.autotuneRelayPowerPercent = Number($('advAutotuneRelayPowerPercent').value);
      config.autotuneBandC = Number($('advAutotuneBandC').value);
    }
    return config;
  }

  function nextRevision(device) {
    return Math.max(Number(device.revision || 0) + 1, Math.floor(Date.now() / 1000));
  }

  function publish(route, payload, options = {}) {
    const wire = JSON.stringify(payload);
    const bytes = encoder.encode(wire).length + encoder.encode(route.channel).length + PACKET_POLICY.MQTT_OVERHEAD;
    if (bytes > PACKET_POLICY.NORMAL_CAP) {
      const error = new Error(`Gói MQTT vượt giới hạn ${PACKET_POLICY.NORMAL_CAP} B (${bytes} B)`);
      error.code = 'PROTOCOL_ERROR';
      throw error;
    }
    const transport = state.realtime;
    if (!transport || transport.deviceId !== route.deviceId || !transport.connected) {
      const error = new Error('Chưa kết nối với máy chủ');
      error.code = 'TRANSPORT_ERROR';
      if (options.awaitAck) return Promise.reject(error);
      throw error;
    }
    if (!options.awaitAck) return transport.publish(route.channel, wire, { qos: options.qos ?? 1 });
    return transport.publish(route.channel, wire, { qos: 1 }).then(() => {
      const id = options.requestId;
      const pending = state.pending.get(id) || state.uncertain.get(id);
      const tPuback = performance.now();
      if (pending && pending.tBrokerPuback == null) pending.tBrokerPuback = tPuback;
      const outcome = terminalOutcomes.get(id);
      if (outcome && outcome.tBrokerPuback == null) outcome.tBrokerPuback = tPuback;
      // Broker PUBACK is transport delivery only; the controller outcome is still pending.
    });
  }

  function startTransaction(id, pending, timeoutMs) {
    transactions.create(id, pending.operation);
    pending.phase = 'CREATED';
    pending.tCreated = performance.now();
    pending.timeoutMs = timeoutMs;
    pending.onTimeout = () => {
      if (state.pending.get(id) !== pending) return;
      moveToUncertain(id, pending);
      const device = state.devices.find((item) => item.id === pending.deviceId);
      if (pending.kind === 'config') setFormState(pending.formId, 'unconfirmed', 'Chưa nhận xác nhận cuối từ máy · đang đồng bộ trạng thái');

      if (pending.kind === 'history') {
        telemetryChart.historyLoading = false;
        telemetryChart.historyRetryAt = Date.now() + 30_000;
        telemetrySetStatus('Máy chưa xác nhận đọc lịch sử · kết quả chưa chắc chắn');
      }
      if (pending.action === 'batch_start' || pending.action === 'batch_stop') {
        clearBatchActionPending(device);
        device.batchUiAwaitingConfirmTarget = batchTargetForAction(pending.action);
        if (device.id === state.selectedId) setFormError('batchForm', 'Chưa nhận xác nhận cuối · đang kiểm tra trạng thái máy');
      }
      // Doc lich su EEPROM la tac vu nen tu chay khi mo trang Me ap. Neu
      // ESP32 tra cham, chi cap nhat trang thai ngay trong bieu do; khong
      // hien toast nhu mot thao tac that bai do nguoi dung vua thuc hien.
      if (pending.kind !== 'history')
        toast('Chưa nhận xác nhận cuối từ máy; kết quả chưa chắc chắn', 5000);
      // Ask the device to republish state so a lost ACK can be reconciled.
      if (device) sendSession(device.id, true, true);
    };
    state.pending.set(id, pending);
    return pending;
  }

  function moveToUncertain(id, pending) {
    if (state.pending.get(id) !== pending) return;
    clearTimeout(pending.timeout);
    transactions.uncertain(id);
    pending.phase = 'UNCERTAIN';
    pending.uncertainAt = performance.now();
    state.pending.delete(id);
    state.uncertain.set(id, pending);
  }

  function sweepUncertain() {
    const now = performance.now();
    for (const id of transactions.expireUncertain(now)) {
      const pending = state.uncertain.get(id);
      if (!pending) continue;
      clearTimeout(pending.retryTimer);
      state.uncertain.delete(id);
      const device = state.devices.find((item) => item.id === pending.deviceId);
      if (pending.observed) {
        if (pending.kind === 'config' && Number(device?.revision || 0) <= pending.revision)
          setFormState(pending.formId, 'unconfirmed', 'Đã thấy cấu hình trên máy; không nhận được ACK cho giao dịch');
        if (pending.kind === 'history' && telemetryChart.activeRequestId === id)
          telemetrySetStatus('Đã nhận lịch sử trong bộ nhớ máy; không nhận được ACK cuối');
        continue;
      }
      if (pending.kind === 'config' && Number(device?.revision || 0) <= pending.revision)
        setFormState(pending.formId, 'unconfirmed', 'Không nhận được kết quả cuối; kiểm tra lại cấu hình trên máy');
      if (pending.kind === 'history' && telemetryChart.activeRequestId === id)
        telemetrySetStatus('Chưa xác nhận được lịch sử trong bộ nhớ máy · có thể thử đọc lại');
      if (pending.action === 'batch_start' || pending.action === 'batch_stop') {
        if (device) device.batchUiAwaitingConfirmTarget = '';
        if (device?.id === state.selectedId) setFormError('batchForm', 'Chưa xác nhận được kết quả; kiểm tra trạng thái trên máy');
      }
      console.warn('[TX] Hết thời gian đối soát', pending.operation, id);
    }
  }

  function armTransaction(id) {
    const pending = state.pending.get(id);
    if (pending) pending.timeout = setTimeout(pending.onTimeout, pending.timeoutMs);
  }

  function transactionPublished(id) {
    transactions.published(id);
    const pending = state.pending.get(id);
    if (pending) { pending.phase = 'PUBLISHED'; pending.tPublished = performance.now(); }
  }

  function retrySameRequest(id, route, envelope) {
    const pending = state.pending.get(id) || state.uncertain.get(id);
    if (!pending) return;
    const body = JSON.parse(envelope.body);
    const grantExpiry = Number(String(envelope.grant || '').split('|')[1]) * 1000;
    const executionExpiry = Number(body.expiresAt) * 1000;
    const remaining = Math.min(120000,
      grantExpiry > 0 ? grantExpiry - Date.now() : Infinity,
      executionExpiry > 0 ? executionExpiry - Date.now() : Infinity);
    pending.retryWire = { route, envelope, bootId: body.bootId };
    pending.retryDeadline = performance.now() + Math.max(0, remaining);
    pending.retryAttempts = 0;
    const retry = () => {
      clearTimeout(pending.retryTimer);
      if ((state.pending.get(id) || state.uncertain.get(id)) !== pending ||
          performance.now() >= pending.retryDeadline || pending.retryAttempts >= 6) return;
      const device = state.devices.find(item => item.id === pending.deviceId);
      if (body.bootId && device?.bootId && body.bootId !== device.bootId) return;
      if (state.realtimeConnected) {
        ++pending.retryAttempts;
        // Never re-sign, extend expiry or invent a new requestId after uncertainty.
        publish(route, envelope, { awaitAck: true, requestId: id }).catch(error => {
          console.warn('[TX retry]', pending.operation, error);
        });
      }
      pending.retryTimer = setTimeout(retry, 3500);
    };
    pending.retry = retry;
    pending.retryTimer = setTimeout(retry, 3500);
  }

  function resumeExactRetries() {
    for (const pending of [...state.pending.values(), ...state.uncertain.values()])
      if (pending.deviceId === state.selectedId) pending.retry?.();
  }

  async function sendConfig(formId, group) {
    const device = currentDevice();
    if (!device) return toast('Hãy thêm máy trước');
    if (!isDeviceOnline(device)) {
      setFormState(formId, 'unconfirmed', 'Máy đang ngoại tuyến · chưa gửi');
      toast('Máy đang ngoại tuyến · chưa gửi');
      return;
    }
    const config = buildConfig(group);
    if (!config) return;

    // Gia tri khong doi so voi cau hinh dang co thi thoi, khong can gui/bao
    // gi ca - giong HMI (xem commitSetting() trong hmi.h). "batch" la hanh
    // dong BAT DAU ME (khong phai chi luu thong so) nen luon phai gui du
    // config trung, khong ap dung guard nay.
    if (group !== 'batch' && configEquals(config, device.config)) return;

    if (group === 'batch') {
      device.batchMeta = {
        name: $('batchName').value.trim(),
        startDate: $('startDate').value,
        targetHumidity: Number($('targetHumidity').value)
      };
      saveDeviceRuntime(device);
    }

    const revision = nextRevision(device);
    const id = requestId('cfg');
    const patch = Object.fromEntries(Object.entries(config).filter(([key, value]) =>
      CONFIG_KEYS.includes(key) && !Object.is(value, device.config?.[key])));
    if (!Object.keys(patch).length) return;
    const payload = { v: Number(device.presence?.proto || 0) >= 2 ? 2 : PROTOCOL_VERSION,
      revision, requestId: id, config: patch };
    setFormState(formId, 'pending', 'Đang gửi tới máy…');
    startTransaction(id, { kind: 'config', operation: 'config.save', deviceId: device.id,
      formId, revision, config, patch, bootId: device.bootId }, WEB.configTimeoutMs);
    try {
      // This command is a one-shot configuration save.
      // 1 lan, khong phai trang thai. Voi retain:true truoc day, ESP32 se
      // nhan lai CHINH payload nay moi lan subscribe lai topic config/set -
      // dieu nay xay ra o MOI lan ket noi lai WebSocket (ke ca WiFi chap chon
      // thoang qua, khong chi luc khoi dong lai), gay ghi EEPROM lai vo ich
      // va co the DE LEN cau hinh moi hon nguoi dung vua sua truc tiep tren
      // HMI sau lan luu web gan nhat - loi im lang, rat kho tu phat hien.
      const envelope = await signRealtimeWrite(device, 'config/set', payload);
      armTransaction(id);
      transactionPublished(id);
      retrySameRequest(id, routes(device.id).config, envelope);
      await publish(routes(device.id).config, envelope, { awaitAck: true, requestId: id });

    } catch (error) {
      if (state.uncertain.has(id) || pendingOutcomeKnown(id)) return;
      if (error.code === 'UNCERTAIN') { state.pending.get(id)?.onTimeout(); return; }
      clearPending(id);
      setFormState(formId, 'error', error.message);
      toast(error.message, 3600);
    }
  }

  async function sendCommand(action, options = {}) {
    const device = options.device || currentDevice();
    if (!device) return false;
    if (!isDeviceOnline(device)) {
      toast('Máy đang ngoại tuyến');
      return false;
    }
    if (!device.bootId) {
      toast('Đang nhận dữ liệu từ máy. Vui lòng chờ một chút.');
      return false;
    }

    device.commandSequence = Math.max(device.commandSequence + 1, Math.floor(Date.now() / 1000));
    saveDeviceRuntime(device);
    const id = requestId('cmd');
    const payload = {
      v: PROTOCOL_VERSION,
      sequence: device.commandSequence,
      requestId: id,
      bootId: device.bootId,
      expiresAt: Math.floor(Date.now() / 1000) + 8,
      action,
      validForMs: options.validForMs ?? 5000,
      leaseMs: options.leaseMs ?? 0,
      alarmMask: options.alarmMask ?? 0,
      arg0: options.arg0 ?? 0,
      arg1: options.arg1 ?? 0,
      value: options.value ?? 0
    };
    if (Number(device.presence?.proto || 0) >= 2) {
      payload.v = 2;
      delete payload.sequence;
      delete payload.validForMs;
      payload.clientId = controlClientId;
      payload.nonce = Array.from(crypto.getRandomValues(new Uint8Array(8)),
        (b) => b.toString(16).padStart(2, '0')).join('');
      // No optional zero fields in the hot path.
      for (const key of ['leaseMs', 'alarmMask', 'arg0', 'arg1', 'value']) if (!payload[key]) delete payload[key];
    }
    startTransaction(id, { kind: 'command', operation: action.replaceAll('_', '.'),
      deviceId: device.id, action }, WEB.commandTimeoutMs);

    try {
      const envelope = await signRealtimeWrite(device, 'command', payload);
      armTransaction(id);
      transactionPublished(id);
      retrySameRequest(id, routes(device.id).command, envelope);
      await publish(routes(device.id).command, envelope, { awaitAck: true, requestId: id });

      return true;
    } catch (error) {
      if (state.uncertain.has(id) || pendingOutcomeKnown(id)) return true;
      if (error.code === 'UNCERTAIN') { state.pending.get(id)?.onTimeout(); return true; }
      clearPending(id);
      toast(error.message);
      return false;
    }
  }


  function clearPending(id) {
    const pending = state.pending.get(id);
    if (pending?.timeout) clearTimeout(pending.timeout);
    if (pending?.retryTimer) clearTimeout(pending.retryTimer);
    state.pending.delete(id);
    transactions.remove(id);
  }

  // A terminal ACK can beat the WebSocket.js PUBACK callback (or its error).
  // Keep a bounded terminal record so that the later callback cannot undo it.
  const terminalOutcomes = new Map();
  function pendingOutcomeKnown(id) { return terminalOutcomes.has(id); }
  function rememberOutcome(id, ok, pending) {
    terminalOutcomes.set(id, { ok, at: performance.now(), operation: pending.operation,
      tCreated: pending.tCreated, tPublished: pending.tPublished,
      tSubmitted: pending.tSubmitted });
  }

  function handleAck(device, ack) {
    const id = String(ack.requestId || '');
    const pending = state.pending.get(id) || state.uncertain.get(id);
    if (!pending || pending.deviceId !== device.id) return;

    if (Number(ack.v) === 2 && ack.operation === pending.operation && ack.phase === 'completed' &&
        ack.ok === true && typeof ack.code === 'string' && pending.kind === 'history' && (telemetryChart.activeRequestId !== id ||
        !telemetryChart.historyLoaded || telemetryChart.historyGap)) {
      // A valid controller ACK proves execution, not delivery of every chunk.
      if (state.pending.has(id)) moveToUncertain(id, pending);
      pending.historyTerminalAck = ack;
      telemetryChart.historyLoading = false;
      telemetryChart.historyRetryAt = Date.now() + 10_000;
      telemetrySetStatus('Thiếu gói lịch sử · sẽ đọc lại');
      return;
    }


    const transition = Number(ack.v) === 2 ? transactions.ack(id, ack) : null;
    if (transition === 'IGNORED') return;
    if (transition === 'PROTOCOL_ERROR') {
      toast('Máy gửi phản hồi chưa hợp lệ. Chưa xác nhận được thao tác.');
      return;
    }
    if (Number.isFinite(Number(ack.bootId))) device.bootId = Number(ack.bootId);
    const v2 = Number(ack.v) === 2;
    const result = String(ack.result || '').toLowerCase();
    const phase = v2 ? String(ack.phase || '').toLowerCase()
      : result === 'accepted' ? 'received' : 'completed';
    if (phase === 'received') {
      if (pending.phase === 'UNCERTAIN') return;
      pending.phase = 'RECEIVED';
      pending.tDeviceReceived = performance.now();
      if (pending.kind === 'config') setFormState(pending.formId, 'pending', 'Máy đã nhận · đang lưu vào bộ nhớ máy…');
      else toast('Máy đã nhận yêu cầu · đang thực hiện');
      return;
    }
    if (phase === 'uncertain') {
      moveToUncertain(id, pending);
      if (pending.kind === 'config') setFormState(pending.formId, 'unconfirmed',
        'Máy chưa xác nhận lưu vào bộ nhớ máy · đang đồng bộ');
      if (pending.action === 'batch_start' || pending.action === 'batch_stop') {
        clearBatchActionPending(device);
        device.batchUiAwaitingConfirmTarget = batchTargetForAction(pending.action);
      }
      toast('Chưa xác định kết quả thực hiện; đang đọc lại trạng thái máy', 5000);
      return;
    }
    if (phase !== 'completed' || (v2 && typeof ack.ok !== 'boolean')) {
      toast('Máy gửi phản hồi chưa hợp lệ. Chưa xác nhận được thao tác.');
      return;
    }
    const ok = v2 ? ack.ok : result === 'applied';
    const message = humanAckMessage(ack);
    clearTimeout(pending.retryTimer);
    rememberOutcome(id, ok, pending);
    pending.phase = ok ? 'APPLIED' : 'REJECTED';
    const tAckBrowser = performance.now();
    const late = state.uncertain.has(id);
    pending.tAckBrowser = tAckBrowser;
    const deviceReceived = Number(ack.tDeviceReceived);
    const deviceCompleted = Number(ack.tDeviceCompleted);
    console.info('[TX latency]', { operation: pending.operation, code: ack.code || result,
      late, tCreatedBrowser: pending.tCreated, tPublishBrowser: pending.tPublished,
      tAckBrowser,
      tDeviceReceivedEsp: ack.tDeviceReceived, tDeviceCompletedEsp: ack.tDeviceCompleted,
      webToPublishMs: Math.round((pending.tPublished || pending.tCreated) - pending.tCreated),
      publishToReceivedAckMs: pending.tDeviceReceived && pending.tPublished
        ? Math.round(pending.tDeviceReceived - pending.tPublished) : null,
      clickToTerminalAckMs: Math.round(tAckBrowser - pending.tCreated),
      deviceProcessMs: Number.isFinite(deviceReceived) && Number.isFinite(deviceCompleted)
        ? (deviceCompleted - deviceReceived) >>> 0 : null });
    state.lastTerminalByDevice.set(device.id, { operation: pending.operation,
      tAckBrowser, at: Date.now() });
    if (late) { state.uncertain.delete(id); transactions.remove(id); }
    else clearPending(id);

    if (pending.kind === 'config') {
      const superseded = Number(device.revision || 0) > Number(ack.revision || pending.revision);
      if (ok) {
        if (Number(ack.revision || pending.revision) >= device.revision) {
          device.config = { ...pending.config };
          device.revision = Number(ack.revision || pending.revision);
        }
        if (!superseded) setFormState(pending.formId, 'saved', 'Máy đã lưu và xác nhận cài đặt');
        toast('Đã lưu cấu hình trên máy');
      } else {
        if (!superseded) setFormState(pending.formId, 'error', message);
        toast(`Máy từ chối: ${message}`, 5000);
      }
      return;
    }

    if (pending.kind === 'history') {
      telemetryChart.historyLoading = false;
      const complete = ok && telemetryChart.historyLoaded && !telemetryChart.historyGap;
      telemetryChart.historyRetryAt = complete ? 0 : Date.now() + 10_000;
      if (ok && !complete) { telemetrySetStatus('Thiếu gói lịch sử · sẽ đọc lại'); return; }
      if (!ok) telemetrySetStatus(ack.code === 'HISTORY_EEPROM_ERROR'
        ? 'Lỗi đọc bộ nhớ lịch sử' : message);
      else telemetrySetStatus(ack.code === 'HISTORY_EMPTY'
        ? 'Bộ nhớ máy chưa có dữ liệu · cập nhật trực tiếp' : 'Đã đọc lịch sử trong bộ nhớ máy');
      return;
    }
    if (pending.action === 'batch_start' || pending.action === 'batch_stop') {
      device.batchUiAwaitingConfirmTarget = '';
      if (!ok) {
        clearBatchActionPending(device);
        if (device.id === state.selectedId) {
          renderBatchAction(device);
          setFormError('batchForm', message);
        }
        addBatchLog(device, message);
      } else if (device.id === state.selectedId) setFormError('batchForm', '');
    }
    toast(ok ? message : `Máy từ chối: ${message}`, 5000);
  }

  async function verifyDeviceAck(device, ack) {
    const pending = state.pending.get(String(ack?.requestId || '')) ||
      state.uncertain.get(String(ack?.requestId || ''));
    if (!pending || pending.deviceId !== device.id) return false;
    if (!pending.ackKey || !/^[a-f0-9]{64}$/i.test(String(ack.sig || ''))) {
      console.warn('[TX] ACK thiếu chữ ký phiên', ack.requestId);
      return false;
    }
    const fields = ['mayap-mqtt-ack:v2', device.id, ack.requestId, ack.operation,
      ack.phase, ack.ok ? '1' : '0', ack.code, ack.bootId, ack.revision, ack.message];
    if (fields.some((field) => field === undefined || field === null)) return false;
    const bytes = new Uint8Array(ack.sig.match(/../g).map((hex) => parseInt(hex, 16)));
    return crypto.subtle.verify('HMAC', pending.ackKey, bytes,
      encoder.encode(fields.join('\n')));
  }

  // Toan bo chuoi "message" ma firmware co the tra ve trong ack (xem
  // machine_control.h/transaction_bridge.h) - LUON viet HOA khong dau theo quy
  // uoc noi bo cho Serial/HMI. Truoc day web dung 1 regex de "doan" xem raw
  // co phai cau da dep san khong, nhung vi quy uoc firmware LUON viet hoa
  // khong dau nen regex do LUON coi la "ma chung" va VUT BO ly do cu the
  // (vd bam "Bat dau me" khi chua bat cong tac nhiet se chi thay "ESP32 tu
  // choi yeu cau" thay vi ly do that). Danh sach nay dich TOAN BO cac message
  // co that trong firmware sang tieng Viet co dau - phai cap nhat neu firmware
  // them message moi.
  const RAW_ACK_MESSAGES = {
    'LENH KHONG HOP LE': 'Lệnh không hợp lệ',
    'DA THOAT TEST': 'Đã thoát chế độ kiểm tra',
    'DA TAT THIET BI': 'Đã tắt thiết bị đang kiểm tra',
    'DA HUY KIEM TRA': 'Đã hủy kiểm tra',
    'DA HUY AUTO TUNE': 'Đã hủy tự dò PID',
    'AUTO TUNE KHONG CHAY': 'Không có quá trình tự dò PID đang chạy',
    'DA DONG CONG WIFI': 'Đã đóng cổng đổi Wi-Fi',
    'DA GUI YEU CAU DAT LAI PIN': 'Đã gửi yêu cầu đặt lại mã PIN lên máy chủ',
    'DANG TAI FIRMWARE...': 'Máy đang tải phần mềm mới - không tắt nguồn',
    'DANG KIEM TRA BAN MOI': 'Máy đang kiểm tra phiên bản mới',
    'DANG QUAY LAI FIRMWARE CU...': 'Máy đang quay lại phần mềm trước đó - sắp khởi động lại',
    'KHONG CO BAN CU DE QUAY LAI': 'Không còn bản phần mềm trước đó để quay lại',
    'DA CHON TIEP TUC ME': 'Đã chọn tiếp tục mẻ ấp dở',
    'KHONG CO ME CHO XAC NHAN': 'Không có mẻ nào đang chờ xác nhận',
    'HAY THOAT TEST TRUOC': 'Hãy thoát chế độ kiểm tra trước',
    'ME DANG CHAY': 'Đang có mẻ ấp chạy, không thể thực hiện',
    'DANG XOA DU LIEU ME CU': 'Máy đang xóa dữ liệu mẻ cũ, thử lại sau',
    'LOI NHAT KY AN TOAN': 'Lỗi nhật ký an toàn - cần kiểm tra máy',
    'LOI BO NHO CAU HINH': 'Lỗi bộ nhớ cấu hình - cần kiểm tra máy',
    'HAY XAC NHAN RESET LOI': 'Hãy xác nhận lỗi khởi động lại bất thường trên máy trước',
    'DUNG ME CU TRUOC': 'Hãy dừng mẻ cũ trước',
    'AUTO TUNE DANG CHAY': 'Tự dò đang chạy, không thể thực hiện',
    'HAY CHUYEN SANG AUTO': 'Hãy chuyển công tắc trên máy sang chế độ Tự động trước',
    'CAM BIEN CHUA SAN SANG': 'Cảm biến nhiệt độ/độ ẩm chưa sẵn sàng',
    'INVALID_PID_GAINS': 'PID không hợp lệ: Kp, Ki và Kd không được đồng thời bằng 0.',
    'RTC CHUA HOP LE': 'Đồng hồ thời gian thực (RTC) chưa hợp lệ',
    'LOI 2 HANH TRINH': 'Lỗi cả 2 công tắc hành trình cùng tác động',
    'DANG CO LOI DAO': 'Đang có lỗi cơ cấu đảo trứng, cần xử lý trước',
    'NHIET DANG QUA CAO': 'Nhiệt độ đang quá cao, không thể thực hiện',
    'HAY BAT CONG TAC NHIET': 'Hãy bật công tắc thanh nhiệt trước',
    'HAY BAT TU DONG DAO': 'Hãy bật chế độ tự động đảo trứng trước',
    'LOI LUU TRANG THAI ME': 'Lỗi lưu trạng thái mẻ ấp vào bộ nhớ',
    'DA BAT DAU ME': 'Đã bắt đầu mẻ ấp mới',
    'DANG XOA DU LIEU ME': 'Đang xóa dữ liệu mẻ, chưa có mẻ nào chạy',
    'KHONG CO ME DANG CHAY': 'Không có mẻ nào đang chạy',
    'DUNG ME TRUOC': 'Hãy dừng mẻ đang chạy trước',
    'KHOANG NHIET KHONG DU': 'Khoảng nhiệt độ hiện tại không đủ rộng để chạy tự dò',
    'AUTO TUNE DA BAT DAU': 'Đã bắt đầu tự dò',
    'DANG CO ME - KHONG TEST DUOC': 'Đang có mẻ ấp chạy, không vào được chế độ kiểm tra',
    'DANG QUA NHIET KHAN CAP': 'Đang quá nhiệt khẩn cấp, không thể thực hiện',
    'DA VAO CHE DO TEST': 'Đã vào chế độ kiểm tra thiết bị',
    'CHUA VAO CHE DO TEST': 'Chưa ở chế độ kiểm tra thiết bị',
    'THIET BI KHONG HOP LE': 'Thiết bị chọn để kiểm tra không hợp lệ',
    'DANG BAT THIET BI': 'Đang bật thử thiết bị',
    'HAY TAC DONG CONG TAC HANH TRINH': 'Hãy tác động công tắc hành trình để kiểm tra',
    'DANG MO CONG DOI WIFI': 'Đang mở cổng đổi Wi-Fi trên máy',
    'CHI DUNG DUOC KHI ONLINE': 'Chỉ dùng được khi máy đang trực tuyến',
    'DA HUY ME CU': 'Đã hủy mẻ cũ',
    'DA HUY - CHO XOA BO NHO': 'Đã hủy - đang chờ xóa bộ nhớ',
    'DA DUNG ME': 'Đã dừng mẻ ấp',
    'DA DUNG - CHO XOA BO NHO': 'Đã dừng - đang chờ xóa bộ nhớ',
    'LOI HE THONG CHUA XOA': 'Còn lỗi hệ thống chưa được xóa',
    // F-12 (audit truoc phat hanh v3.7.1): chuoi nay gio duoc firmware sinh
    // dong tu SIREN_TEMPORARY_MUTE_MS (machine_control.h) thay vi hard-code
    // "5 PHUT" sai le thuc te (hang so la 60000ms = 1 phut) - neu doi hang so
    // do, sua ca key nay cho khop.
    'COI TAM DUNG 1 PHUT': 'Đã tạm dừng còi 1 phút',
    // F-09 (audit truoc phat hanh v3.7.1): ACK coi khan cap/loi dao qua web
    // bi tu choi - 2 truong hop nay bat buoc xac nhan tai may (xem
    // HmiCommandSource trong config.h, case AlarmAck trong machine_control.h).
    'COI KHAN CAP CAN ACK TAI MAY': 'Còi báo khẩn cấp cần xác nhận trực tiếp tại máy, không thể tắt từ xa',
    'LOI DAO CAN ACK TAI MAY': 'Lỗi cơ cấu đảo trứng cần kiểm tra và xác nhận trực tiếp tại máy',
    // F-06: phan hoi cho lenh batch_overdue_continue.
    'DA XAC NHAN TIEP TUC U AM': 'Đã xác nhận tiếp tục ủ ấm',
    'DA XAC NHAN RESET LOI': 'Đã xác nhận lỗi khởi động lại bất thường',
    'DA XOA LOI DAO': 'Đã xóa lỗi cơ cấu đảo trứng',
    'THA NUT/KT HANH TRINH': 'Hãy thả nút nhấn hoặc kiểm tra công tắc hành trình',
    'DA XAC NHAN': 'Đã xác nhận',
    'CHUA CO CAU HINH GOC': 'Máy chưa có cấu hình gốc để so sánh',
    'THIEU CONFIG': 'Thiếu dữ liệu cấu hình gửi lên',
    'LUU CAU HINH BI TU CHOI': 'Máy từ chối lưu (đang có mẻ chạy khoá cấu hình, hoặc lỗi bộ nhớ) - thử lại sau',
    'LUU NHAC NHO BI TU CHOI': 'Máy từ chối lưu danh sách nhắc nhở (lỗi bộ nhớ) - thử lại sau',
    'THIEU REMINDERS': 'Thiếu dữ liệu nhắc nhở gửi lên',
    // F-01 (audit truoc phat hanh v3.7.1): may tu choi lenh vi dang dung
    // The command requires authenticated application validation.
    // realtimeCommandChannelTrusted() trong transaction_bridge.h.
    'BROKER CONG KHAI - LENH TU XA BI KHOA': 'Máy chưa được thiết lập kết nối điều khiển bảo mật nên lệnh điều khiển từ xa bị khoá để an toàn - vui lòng thao tác trực tiếp trên máy',
    'CHU KY LENH KHONG HOP LE': 'Yêu cầu điều khiển không có chữ ký hợp lệ - hãy xác thực lại PIN nếu vừa đổi hoặc đặt lại PIN'
  };

  function humanAckMessage(ack) {
    const result = String(ack.result || '');
    const raw = String(ack.message || '').trim();
    const map = {
      accepted: 'Máy đã tiếp nhận yêu cầu',
      applied: 'Máy đã lưu và áp dụng cấu hình',
      rejected: 'Máy từ chối yêu cầu (không rõ lý do)',
      invalid: 'Dữ liệu gửi xuống không hợp lệ',
      duplicate: 'Yêu cầu này đã được xử lý',
      busy: 'Máy đang xử lý yêu cầu khác',
      expired: 'Lệnh đã hết thời gian hiệu lực',
      stale: 'Lệnh thuộc lần khởi động cũ',
      unsupported: 'Phần mềm máy chưa hỗ trợ thao tác này',
      unauthorized: 'Yêu cầu điều khiển chưa được máy chủ xác thực'
    };
    if (raw) return RAW_ACK_MESSAGES[raw] || raw;
    return map[result] || `Phản hồi: ${result || 'không xác định'}`;
  }

  function handleConfigReport(device, report) {
    if (Number(report.v) === 2) {
      const key = `${device.id}:${report.bootId}:${report.revision}`;
      let assembly = state.configChunks.get(key);
      if (!assembly || Number(report.part) === 0) {
        assembly = { next: 0, config: {} };
        state.configChunks.set(key, assembly);
      }
      if (Number(report.part) !== assembly.next || !report.config) return;
      Object.assign(assembly.config, report.config);
      assembly.next += 1;
      if (!report.done) return;
      state.configChunks.delete(key);
      report = { ...report, config: assembly.config };
    }
    if (!report.config || !validateFullConfig(report.config)) return;
    if (Number(report.revision || 0) < Number(device.revision || 0)) return;
    device.config = { ...report.config };
    device.revision = Number(report.revision || 0);
    device.configAt = Date.now();
    device.bootId = Number(report.bootId || device.bootId || 0);

    for (const [id, pending] of [...state.pending, ...state.uncertain]) {
      if (pending.deviceId !== device.id || pending.kind !== 'config' ||
          pending.bootId !== device.bootId || report.requestId !== id ||
          device.revision !== pending.revision ||
          !Object.entries(pending.patch).every(([key, value]) =>
            Object.is(device.config[key], value))) continue;
      // Firmware publishes this revision only after EEPROM save and readback.
      // A matching complete report is sufficient when the terminal ACK was lost.
      rememberOutcome(id, true, pending);
      if (state.uncertain.has(id)) {
        state.uncertain.delete(id);
        transactions.remove(id);
      } else clearPending(id);
      setFormState(pending.formId, 'saved', 'Máy đã lưu và xác nhận cài đặt');
      toast('Đã lưu cấu hình trên máy');
      console.info('[CFG-TX] reconciled from verified config/reported', id);
    }

    if (device.id === state.selectedId) applyConfigToUi(device);
  }



  function renderReminderList(device=currentDevice()) {
    const root=$('reminderList');if(!root)return;root.replaceChildren();
    const summary=$('remindersSummary'),list=Array.isArray(device?.reminders)?device.reminders:[];
    if(summary)summary.textContent=!device?'Chọn máy để xem Nhắc nhở':
      device.remindersLoading?'Đang tải từ Cloud…':device.remindersError?'Chưa tải được Nhắc nhở':
      list.length?`${list.length}/32 nhắc đã lưu trên Cloud`:'Chưa có nhắc nào';
    if(device?.remindersError){
      const error=document.createElement('p');error.className='formError show';error.textContent=device.remindersError;root.append(error);
      const retry=document.createElement('button');retry.type='button';retry.className='ghost full';retry.textContent='Thử tải lại';
      retry.addEventListener('click',()=>loadCloudReminders(device,true));root.append(retry);
    }
    list.slice().sort((a,b)=>a.day-b.day||a.label.localeCompare(b.label,'vi')).forEach(item=>{
      const row=document.createElement('div');row.className='deviceListItem';
      const text=document.createElement('div'),strong=document.createElement('strong'),small=document.createElement('small');
      strong.textContent=`Ngày ${item.day}`;small.textContent=item.label;text.append(strong,small);
      const remove=document.createElement('button');remove.type='button';remove.textContent='Xóa';
      remove.disabled=device.accountRole==='viewer'||Boolean(device.remindersPending);
      remove.addEventListener('click',async()=>{
        const ok=await confirmAction({title:'Xóa nhắc nhở này?',message:`Ngày ${item.day} · ${item.label}`,accept:'Xóa',danger:true});
        if(!ok)return;
        try{await removeCloudReminder(device,item);toast('Đã xóa Nhắc nhở khỏi Cloud');}
        catch(error){toast(error.message,5000);await loadCloudReminders(device,true);}
      });
      row.append(text,remove);root.append(row);
    });
    const addBtn=$('addReminderBtn');
    if(addBtn)addBtn.disabled=!device?.id||device.accountRole==='viewer'||Boolean(device.remindersPending)||
      Boolean(device.remindersLoading)||!device.remindersLoaded||Boolean(device.remindersError)||list.length>=32;
  }

  function handleSnapshot(device, snapshot) {
    if (!snapshot?.runtime || !Number(snapshot.bootId)) return;
    if (device.bootId && Number(snapshot.bootId) !== device.bootId) {
      device.revision = 0;
      device.config = null;
      device.configAt = device.configRevision = 0;
    }
    const last = state.lastTerminalByDevice.get(device.id);
    if (last) {
      console.info('[TX state]', { operation: last.operation,
        ackToStateMs: Math.round(performance.now() - last.tAckBrowser) });
      state.lastTerminalByDevice.delete(device.id);
    }
    for (const pending of state.uncertain.values()) {
      if (pending.deviceId !== device.id || pending.observed ||
          !['batch_start', 'batch_stop'].includes(pending.action)) continue;
      if (Boolean(snapshot.runtime?.batchRunning) ===
          (batchTargetForAction(pending.action) === 'running')) pending.observed = true;
    }
    device.snapshot = snapshot;
    device.snapshotAt = Date.now();
    device.dataSource = 'live';
    device.liveEpoch = state.subscriptionEpoch;
    // A new non-retained runtime packet supersedes an earlier LWT.
    if (device.presence?.online === false) device.presence = { ...device.presence, online: true };
    persistRuntimeCache(device);
    feedTelemetrySnapshot(device, snapshot);
    device.bootId = Number(snapshot.bootId || device.bootId || 0);
    if (Number(snapshot.revision || 0) > device.revision) device.revision = Number(snapshot.revision);
    if (device.id === state.selectedId) {
      renderDevice();
      if (document.body.dataset.page === 'batch' && controlReady(device)) loadTelemetryHistory();
    }
  }

  function handlePresence(device, presence) {
    device.presence = presence;
    device.presenceAt = Date.now();
    device.presenceEpoch = state.subscriptionEpoch;
    const nextBoot = Number(presence.bootId || 0);
    if (nextBoot && nextBoot !== device.bootId) {
      device.liveEpoch = -1;
      device.bootId = nextBoot;
      device.revision = 0;
      device.config = null;
      device.configAt = device.configRevision = 0;
    }
    persistRuntimeCache(device, true);
    if (device.id === state.selectedId) renderDevice();
  }

  function handleBootstrap(device, hint) {
    if (!hint || hint.v !== 1 || !Number(hint.bootId) || typeof hint.machineState !== 'string') return;
    // A retained message can be arbitrarily old. Never replace a full live sample,
    // learn control bootId/protocol from it, or reconcile a pending transaction.
    if (device.dataSource === 'live' && device.liveEpoch === state.subscriptionEpoch) return;
    const publishedAt = Number(hint.publishedAt || 0) * 1000;
    if (device.snapshotAt && (!publishedAt || publishedAt < device.snapshotAt)) return;
    const runtime = { ...hint, activeFaults: hint.faultCode ?
      [{ code: hint.faultCode, severity: hint.faultSeverity || 0 }] : [] };
    device.snapshot = { bootId: hint.bootId, revision: hint.revision, runtime };
    if (typeof hint.humidifierInstalled === 'boolean') device.features.humidifierInstalled = hint.humidifierInstalled;
    device.snapshotAt = publishedAt > 0 && publishedAt <= Date.now() + 60000 ? publishedAt : Date.now();
    device.dataSource = 'bootstrap';
    device.bootstrapTimeKnown = publishedAt > 0 && publishedAt <= Date.now() + 60000;
    device.liveEpoch = -1;
    if (device.id === state.selectedId) renderDevice();
  }

  // Trang thai mang gui kem trong "value" cua su kien code 90 (NetStateChanged),
  // doi chieu enum NetState trong config.h.
  const NET_STATE_TEXT = {
    0: 'đã tắt mạng',
    1: 'đang bật Wi‑Fi',
    2: 'chưa có Wi‑Fi',
    3: 'có Wi‑Fi, chưa lên máy chủ',
    4: 'đã kết nối máy chủ'
  };

  // event.code - 1000 = FaultCode, doi chieu bang loi faultDescriptor() trong
  // machine_control.h. CAP NHAT DONG BO khi firmware them ma loi moi vao
  // FaultCode (danh sach nay khop dung 35 ma dang co trong enum FaultCode -
  // xem audit/OPERATION_RECOVERY_MANUAL.md va audit/manual/manual.html).
  const FAULT_TITLES = {
    101: 'Mất cảm biến', 102: 'Cảm biến sai', 103: 'Cảm biến bất thường',
    110: 'Nhiệt độ thấp', 111: 'Nhiệt độ cao', 112: 'Quá nhiệt khẩn cấp',
    113: 'Nhiệt độ biến thiên nhanh', 114: 'Nhiệt độ không ổn định',
    115: 'Thanh nhiệt không nóng', 120: 'Độ ẩm thấp', 121: 'Độ ẩm cao',
    130: 'Tắt công tắc nhiệt', 132: 'Cần chuyển sang Tự động',
    133: 'Tự động bị tắt giữa mẻ', 134: 'Tự động đảo bị tắt',
    135: 'Chờ xác nhận áp lại quá lâu', 136: 'Mẻ ấp quá hạn',
    201: 'Lỗi 2 hành trình', 202: 'Đảo quá thời gian', 203: 'Hành trình bị kẹt',
    204: 'Xung đột lệnh đảo', 205: 'Cần kiểm tra cơ khí đảo',
    301: 'Mất kết nối bộ nhớ máy', 302: 'Bộ nhớ máy suy giảm',
    303: 'Khởi động lại bất thường', 304: 'Xung đột ngõ ra', 305: 'Relay đóng cắt nhiều',
    306: 'Lỗi đồng hồ RTC', 313: 'Chưa xoá dữ liệu mẻ',
    314: 'Lỗi nhật ký an toàn', 315: 'Mất nhật ký mẻ',
    401: 'RAM thấp (cảnh báo sớm)', 402: 'RAM cạn - tự khởi động lại',
    403: 'Dự đoán sắp chạm ngưỡng nhiệt', 404: 'Bộ nhớ máy cần thử lại nhiều',
    501: 'Mất liên lạc mạch báo mất điện (ATtiny)', 502: 'Pin còi sắp hết — hãy thay pin'
  };

  // Mô tả ngắn dùng cho popup chi tiết khi bấm vào ô Trạng thái (Phần "Ý
  // nghĩa" trong Fault Card - audit/manual/manual.html). Không có trong bản
  // đồ này = dùng lại FAULT_TITLES làm mô tả rút gọn.
  const FAULT_DESCRIPTIONS = {
    101: 'Không nhận được tín hiệu hợp lệ từ đầu dò nhiệt/ẩm trong thời gian dài.',
    102: 'Có dữ liệu từ cảm biến nhưng giá trị vượt ngoài dải vật lý cho phép.',
    103: 'Nhiệt độ tụt đột ngột, đang chờ xác nhận có thật hay không.',
    110: 'Nhiệt độ đang dưới ngưỡng cấu hình.',
    111: 'Nhiệt độ vượt ngưỡng cao, đã cắt thanh nhiệt và ép quạt.',
    112: 'Nhiệt độ vượt ngưỡng khẩn cấp — mức nghiêm trọng nhất.',
    113: 'Tốc độ thay đổi nhiệt độ vượt ngưỡng bình thường.',
    114: 'Nhiệt độ dao động vượt biên độ cho phép quanh mức đặt.',
    115: 'Thanh nhiệt đang bật nhưng nhiệt độ không tăng đủ.',
    120: 'Độ ẩm đang dưới ngưỡng cấu hình.',
    121: 'Độ ẩm đang vượt ngưỡng cấu hình.',
    130: 'Công tắc vật lý cho phép nhiệt đang tắt trong lúc mẻ chạy.',
    132: 'Đang chờ áp lại mẻ cũ nhưng công tắc Tự động chưa bật.',
    133: 'Công tắc đang gạt sang Bằng tay trong lúc đang chạy mẻ.',
    134: 'Cấu hình tự động đảo bị tắt trong lúc mẻ đang chạy.',
    135: 'Màn hình "Áp lại mẻ cũ?" đã hiện quá lâu chưa ai xác nhận.',
    136: 'Số ngày ấp thực tế đã vượt số ngày cấu hình.',
    201: 'Cả 2 công tắc hành trình cùng báo tích cực — xung đột vật lý.',
    202: 'Động cơ đảo chạy quá thời gian tối đa mà chưa chạm công tắc hành trình.',
    203: 'Công tắc hành trình không nhả ra sau khi lệnh đảo đã dừng.',
    204: 'Lệnh đảo trái và đảo phải xung đột nhau cùng lúc.',
    205: 'Lỗi đảo lặp lại nhiều lần — cần vào Test Mode xác nhận cơ khí.',
    301: 'Module lưu cấu hình mất kết nối/lỗi nhiều lần liên tiếp.',
    302: 'Bộ nhớ máy lỗi tạm thời, đang tự thử kết nối lại.',
    303: 'Bộ điều khiển vừa khởi động lại do lỗi phần mềm/nguồn.',
    304: 'Phát hiện 2 đầu ra loại trừ nhau cùng bật.',
    305: 'Relay đóng/cắt vượt tần suất cho phép.',
    306: 'Đồng hồ thời gian thực mất kết nối hoặc dữ liệu không hợp lệ.',
    313: 'Đã dừng/huỷ mẻ nhưng bộ nhớ chưa xác nhận ghi xong.',
    314: 'Bộ nhớ nội bộ đảm bảo an toàn khi mất điện bị lỗi ghi.',
    315: 'Log chi tiết mẻ ấp không ghi được (không ảnh hưởng an toàn).',
    401: 'Bộ nhớ RAM còn lại thấp hơn ngưỡng an toàn - hệ thống đang tự theo dõi, chưa ảnh hưởng vận hành.',
    402: 'RAM cạn kiệt nghiêm trọng - máy tự khởi động lại có kiểm soát để phòng tránh treo máy đột ngột. Nhiệt/đảo trứng phục hồi ngay sau khi khởi động lại xong.',
    403: 'Theo tốc độ thay đổi nhiệt độ hiện tại, dự đoán sắp chạm ngưỡng cảnh báo trong ít phút tới - cảnh báo sớm, không phải đã vượt ngưỡng.',
    404: 'Bộ nhớ máy phải thử lại nhiều lần bất thường khi đọc/ghi - dấu hiệu suy giảm sớm của chip nhớ, nên theo dõi thêm.',
    501: 'Mạch báo mất điện độc lập (ATtiny13A) không phản hồi lệnh/ping từ máy - có thể mạch mất nguồn pin dự phòng hoặc dây tín hiệu bị đứt. Không ảnh hưởng nhiệt/đảo trứng, nhưng nếu mất điện lưới xảy ra lúc này, còi báo dự phòng có thể không kêu.',
    502: 'ATtiny phát hiện pin 9V nuôi còi đã xuống dưới khoảng 7V. Đây là cảnh báo nhẹ, không khóa vận hành; hãy thay pin sớm để còi vẫn hoạt động khi mất điện. Hệ thống sẽ nhắc lại định kỳ cho tới khi ATtiny xác nhận nguồn 9V đã phục hồi.'
  };

  // severity: 0=Info,1=Warning,2=Stop,3=Emergency (khop enum FaultSeverity
  // trong machine_control.h). >=2 la muc "khong cho phep ap duoc nua".
  function faultBlocksBatch(severity) {
    return Number(severity) >= 2;
  }

  function eventText(event) {
    const code = Number(event.code || 0);
    const value = Number(event.value || 0);
    const type = Number(event.type || 0);
    const known = {
      1: 'Máy vừa được cấp nguồn',
      2: 'Máy vừa khởi động lại từ bên ngoài',
      3: 'Firmware vừa khởi động lại',
      4: 'Máy khôi phục sau lỗi hệ thống',
      5: 'Máy khôi phục sau watchdog',
      6: 'Nguồn điện từng bị sụt áp',
      20: 'Mẻ ấp đã bắt đầu',
      21: 'Mẻ ấp đã kết thúc',
      22: 'Mẻ ấp đã được khôi phục',
      23: 'Đang chờ xác nhận tiếp tục mẻ',
      24: 'Đã xác nhận tiếp tục mẻ',
      25: 'Đã hủy khôi phục mẻ',
      30: 'Máy đã chuyển sang chế độ tự động',
      31: 'Máy đã chuyển sang chế độ bằng tay',
      40: 'Cảm biến đã hoạt động trở lại',
      41: 'Mất tín hiệu cảm biến',
      50: 'Cấu hình đã được lưu',
      51: 'Tự dò PID đã bắt đầu',
      52: 'Tự dò PID đã hoàn tất',
      53: 'Tự dò PID không hoàn tất',
      450: 'Trạng thái Tự cân bằng nhiệt thay đổi',
      451: 'Mô hình nhiệt cần học lại',
      452: 'Bật Tự cân bằng nhiệt',
      453: 'Tắt Tự cân bằng nhiệt',
      60: 'Bắt đầu đảo trứng sang trái',
      61: 'Bắt đầu đảo trứng sang phải',
      62: 'Đang đưa khay về gốc trái',
      63: 'Đang đưa khay về gốc phải',
      64: 'Đảo trái đã hoàn tất',
      65: 'Đảo phải đã hoàn tất',
      80: 'Một lệnh điều khiển đã bị từ chối',
      81: 'Còi vừa tự kiểm tra (tiếng bíp ngắn định kỳ)'
    };
    if (known[code]) return known[code];
    if (code === 90) return `Trạng thái mạng: ${NET_STATE_TEXT[value] ?? `mã ${value}`}`;
    if (code === 91) return 'Đã mở trang đổi Wi‑Fi từ màn hình máy';
    if (code === 92) return 'Mở cổng cấu hình Wi‑Fi thất bại';
    if (code >= 1000) {
      const title = FAULT_TITLES[code - 1000] || `mã ${code - 1000}`;
      if (type === 5) return `Đã hết lỗi: ${title}`;
      if (type === 6) return `Đã xác nhận lỗi: ${title}`;
      return `Phát sinh lỗi: ${title}`;
    }
    if (code >= 200) return `Đầu ra #${code - 200} ${value ? 'đã bật' : 'đã tắt'}`;
    if (code >= 100) return `Tín hiệu vào #${code - 100} ${value ? 'đã tác động' : 'đã nhả'}`;
    return `Sự kiện máy #${code}`;
  }

  function handleLog(device, event) {
    const entry = {
      sequence: Number(event.sequence || 0),
      time: Number(event.epoch || 0) > 1700000000 ? Number(event.epoch) * 1000 : Date.now(),
      message: eventText(event),
      duration: ''
    };
    if (device.logs.some((item) => item.sequence && item.sequence === entry.sequence)) return;
    device.logs.unshift(entry);
    device.logs = device.logs.slice(0, 100);
    saveDeviceRuntime(device);
    if (device.id === state.selectedId) renderBatchLogs();
  }

  function addBatchLog(device, message, duration = '') {
    if (!device) return;
    device.logs.unshift({ sequence: 0, time: Date.now(), message, duration });
    device.logs = device.logs.slice(0, 100);
    saveDeviceRuntime(device);
    if (device.id === state.selectedId) renderBatchLogs();
  }


  // ==================== Bieu do nhiet do 30 phut / AT24C32 ====================
  // ESP32 luu 24h vao EEPROM ngoai (5 phut/mau). Web chi yeu cau 30 phut qua
  // WebSocket khi can va tiep tuc chen mau live 5s tu snapshot; khong ghi Cloud/D1.
  function telemetryEnsureDevice(device = currentDevice()) {
    const deviceId = device?.id || '';
    if (telemetryChart.deviceId === deviceId) return;
    telemetryChart.deviceId = deviceId;
    telemetryChart.points = [];
    telemetryChart.historyLoaded = false;
    telemetryChart.historyLoadedAt = 0;
    telemetryChart.historyLoading = false;
    telemetryChart.historyRetryAt = 0;
    telemetryChart.historyRequestSeq += 1;
    telemetryChart.activeRequestId = '';
    telemetryChart.lastSampleAt = 0;
    requestTemperatureChartRender();
  }

  function telemetryPoint(raw) {
    const t = Number(raw?.t ?? raw?.time);
    const temperature = Number(raw?.temperature);
    if (!Number.isFinite(t) || t <= 0 || !Number.isFinite(temperature) || temperature < -20 || temperature > 100) return null;
    return { t, temperature };
  }

  function telemetryMerge(points) {
    const cutoff = Date.now() - TELEMETRY_WINDOW_MS - 10 * 60_000;
    const map = new Map();
    [...telemetryChart.points, ...points].forEach((raw) => {
      const point = telemetryPoint(raw);
      if (!point || point.t < cutoff) return;
      map.set(Math.round(point.t / 1000), point);
    });
    telemetryChart.points = [...map.values()].sort((a, b) => a.t - b.t).slice(-TELEMETRY_MAX_POINTS);
  }

  function telemetrySetStatus(text) {
    const element = $('temperatureChartStatus');
    if (element) element.textContent = text;
  }

  async function loadTelemetryHistory(force = false) {
    const device = currentDevice();
    telemetryEnsureDevice(device);
    if (!device) {
      telemetrySetStatus('Đang chờ dữ liệu');
      requestTemperatureChartRender();
      return;
    }
    const fresh = telemetryChart.historyLoaded &&
      Date.now() - telemetryChart.historyLoadedAt < TELEMETRY_HISTORY_REFRESH_MS;
    if (!force && (fresh || telemetryChart.historyLoading || Date.now() < telemetryChart.historyRetryAt)) return;
    if (!state.realtimeConnected || !state.realtime?.connected) {
      telemetrySetStatus('Đang chờ kết nối thiết bị');
      telemetryChart.historyRetryAt = Date.now() + 5000;
      return;
    }
    await requestDeviceData('history');
    if (!controlReady(device)) { telemetryChart.historyRetryAt = Date.now() + 1000; return; }
    if (!device.pairingToken) {
      telemetrySetStatus('Cần xác thực PIN để đọc lịch sử');
      telemetryChart.historyRetryAt = Date.now() + 60_000;
      return;
    }

    telemetryChart.historyLoading = true;
    telemetryChart.historyRetryAt = 0;
    const seq = ++telemetryChart.historyRequestSeq;
    const requestId = `hist-${Date.now().toString(36)}-${seq.toString(36)}`.slice(0, 39);
    telemetryChart.activeRequestId = requestId;
    telemetryChart.nextCursor = 0;
    telemetryChart.historyGap = false;
    telemetrySetStatus('Đang đọc lịch sử 30 phút gần nhất…');
    try {
      const body = { v: 1, requestId, minutes: 30 };
      const envelope = await signRealtimeWrite(device, 'history/request', body);
      if (telemetryChart.deviceId !== device.id || telemetryChart.activeRequestId !== requestId) return;
      if (Number(device.presence?.proto || 0) >= 2) {
        startTransaction(requestId, { kind: 'history', operation: 'history.read',
          deviceId: device.id }, 15_000);
        state.pending.get(requestId).ackKey = envelopeKeys.get(envelope) || controlSessions.get(device.id)?.key;
        armTransaction(requestId);
        transactionPublished(requestId);
      }
      retrySameRequest(requestId, routes(device.id).historyRequest, envelope);
      await publish(routes(device.id).historyRequest, envelope, { awaitAck: true, requestId });

      window.setTimeout(() => {
        if (state.pending.has(requestId)) return; // V2 transaction owns its timeout.
        if (telemetryChart.activeRequestId !== requestId || !telemetryChart.historyLoading) return;
        telemetryChart.historyLoading = false;
        telemetryChart.historyRetryAt = Date.now() + 30_000;
        telemetrySetStatus('Không nhận được lịch sử · vẫn cập nhật trực tiếp');
        requestTemperatureChartRender();
      }, 8000);
    } catch (error) {
      if (state.uncertain.has(requestId) || pendingOutcomeKnown(requestId)) return;
      if (error.code === 'UNCERTAIN') { state.pending.get(requestId)?.onTimeout(); return; }
      clearPending(requestId);
      if (telemetryChart.activeRequestId !== requestId) return;
      telemetryChart.historyLoading = false;
      telemetryChart.historyRetryAt = Date.now() + 30_000;
      telemetrySetStatus(error?.message || 'Không đọc được lịch sử');
    }
  }

  function handleTemperatureHistory(device, payload) {
    if (!device || device.id !== state.selectedId) return;
    telemetryEnsureDevice(device);
    if (String(payload?.requestId || '') !== telemetryChart.activeRequestId) return;
    const cursor = Number(payload?.cursor);
    if (Number.isFinite(cursor) && cursor !== telemetryChart.nextCursor) {
      telemetryChart.historyGap = true;
    }
    telemetryChart.nextCursor = Number.isFinite(cursor) ? cursor + 12 : telemetryChart.nextCursor;
    const samples = Array.isArray(payload?.samples) ? payload.samples : [];
    telemetryMerge(samples.map((row) => ({
      t: Number(row?.[0]) * 1000,
      temperature: Number(row?.[1]),
    })));
    if (payload?.done) {
      const pending = state.uncertain.get(String(payload.requestId || ''));
      if (pending?.deviceId === device.id && pending.kind === 'history' && !telemetryChart.historyGap)
        pending.observed = true;
      telemetryChart.historyLoading = false;
      telemetryChart.historyLoaded = !telemetryChart.historyGap;
      telemetryChart.historyLoadedAt = Date.now();
      const transaction = state.pending.get(String(payload.requestId)) || state.uncertain.get(String(payload.requestId));
      if (telemetryChart.historyLoaded && transaction?.historyTerminalAck) handleAck(device, transaction.historyTerminalAck);
      telemetryChart.historyRetryAt = telemetryChart.historyGap ? Date.now() + 10_000 : 0;
      telemetrySetStatus(telemetryChart.historyGap ? 'Thiếu gói lịch sử · sẽ đọc lại' : telemetryChart.points.length
        ? 'Lịch sử mỗi 5 phút · cập nhật trực tiếp'
        : 'Bộ nhớ máy chưa có dữ liệu · cập nhật trực tiếp');
    }
    requestTemperatureChartRender();
  }

  function feedTelemetrySnapshot(device, snapshot) {
    if (!device || device.id !== state.selectedId) return;
    telemetryEnsureDevice(device);
    const temperature = Number(snapshot?.runtime?.temperature);
    if (!Number.isFinite(temperature) || temperature < -20 || temperature > 100) return;
    const value = $('temperatureChartValue');
    if (value) value.textContent = `${numberVi(temperature)}°C`;
    const now = Date.now();
    if (!telemetryChart.lastSampleAt || now - telemetryChart.lastSampleAt >= TELEMETRY_LIVE_SAMPLE_MS) {
      telemetryChart.lastSampleAt = now;
      telemetryMerge([{ t: now, temperature }]);
    }
    if (document.body.dataset.page === 'batch' &&
        (!telemetryChart.historyLoaded || Date.now() - telemetryChart.historyLoadedAt >= TELEMETRY_HISTORY_REFRESH_MS)) {
      loadTelemetryHistory();
    }
    requestTemperatureChartRender();
  }

  function requestTemperatureChartRender() {
    if (telemetryChart.renderRaf || typeof requestAnimationFrame !== 'function') return;
    telemetryChart.renderRaf = requestAnimationFrame(() => {
      telemetryChart.renderRaf = 0;
      renderTemperatureChart();
    });
  }

  function renderTemperatureChart() {
    const canvas = $('temperatureChartCanvas');
    if (!canvas) return;
    const wrap = canvas.parentElement;
    const widthCss = Math.max(1, Math.floor(wrap?.clientWidth || canvas.clientWidth || 280));
    const heightCss = Math.max(1, Math.floor(wrap?.clientHeight || canvas.clientHeight || 240));
    const dpr = Math.max(1, Math.min(2, Number(window.devicePixelRatio) || 1));
    const width = Math.floor(widthCss * dpr);
    const height = Math.floor(heightCss * dpr);
    if (canvas.width !== width || canvas.height !== height) { canvas.width = width; canvas.height = height; }
    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, widthCss, heightCss);

    const now = Date.now();
    const start = now - TELEMETRY_WINDOW_MS;
    const points = telemetryChart.points.filter((point) => point.t >= start && point.t <= now + 5000);
    const empty = $('temperatureChartEmpty');
    if (empty) empty.hidden = points.length > 0;

    const css = getComputedStyle(document.documentElement);
    const color = (name, fallback) => css.getPropertyValue(name).trim() || fallback;
    const gridColor = color('--lineSoft', '#dce7e3');
    const textColor = color('--muted', '#6a7d78');
    const liveColor = color('--chartLine', '#286974');
    const setColor = color('--warning', '#e29b1d');
    const left = 42, right = 10, top = 12, bottom = 12;
    const plotW = Math.max(1, widthCss - left - right);
    const plotH = Math.max(1, heightCss - top - bottom);
    const target = Number(currentDevice()?.config?.targetTemp ?? $('batchTarget')?.value);
    const values = points.map((point) => point.temperature);
    if (Number.isFinite(target)) values.push(target);
    let yMin = values.length ? Math.min(...values) : 36.5;
    let yMax = values.length ? Math.max(...values) : 38.5;
    const spread = Math.max(0.5, yMax - yMin);
    const pad = Math.max(0.25, spread * 0.22);
    yMin = Math.floor((yMin - pad) * 10) / 10;
    yMax = Math.ceil((yMax + pad) * 10) / 10;
    if (yMax - yMin < 1) { const mid = (yMax + yMin) / 2; yMin = mid - 0.5; yMax = mid + 0.5; }
    const xFor = (t) => left + ((t - start) / TELEMETRY_WINDOW_MS) * plotW;
    const yFor = (v) => top + (1 - ((v - yMin) / (yMax - yMin))) * plotH;

    ctx.font = '11px Inter, -apple-system, BlinkMacSystemFont, Segoe UI, sans-serif';
    ctx.textAlign = 'right'; ctx.textBaseline = 'middle'; ctx.lineWidth = 1;
    const intervals = heightCss < 110 ? 2 : 3;
    for (let i = 0; i <= intervals; i += 1) {
      const ratio = i / intervals, y = top + ratio * plotH, value = yMax - ratio * (yMax - yMin);
      ctx.strokeStyle = gridColor; ctx.setLineDash([]); ctx.beginPath(); ctx.moveTo(left, y); ctx.lineTo(widthCss - right, y); ctx.stroke();
      ctx.fillStyle = textColor; ctx.fillText(`${numberVi(value)}°`, left - 7, y);
    }
    if (Number.isFinite(target) && target >= yMin && target <= yMax) {
      const y = yFor(target); ctx.strokeStyle = setColor; ctx.lineWidth = 1.25; ctx.setLineDash([6, 5]);
      ctx.beginPath(); ctx.moveTo(left, y); ctx.lineTo(widthCss - right, y); ctx.stroke();
    }
    if (points.length) {
      ctx.strokeStyle = liveColor; ctx.lineWidth = 2.25; ctx.lineJoin = 'round'; ctx.lineCap = 'round'; ctx.setLineDash([]); ctx.beginPath();
      points.forEach((point, index) => { const x = xFor(point.t), y = yFor(point.temperature); if (!index) ctx.moveTo(x, y); else ctx.lineTo(x, y); });
      ctx.stroke();
      const last = points[points.length - 1]; ctx.fillStyle = liveColor; ctx.beginPath(); ctx.arc(xFor(last.t), yFor(last.temperature), 3, 0, Math.PI * 2); ctx.fill();
      const value = $('temperatureChartValue'); if (value) value.textContent = `${numberVi(last.temperature)}°C`;
    } else {
      const value = $('temperatureChartValue'); if (value && !value.textContent) value.textContent = '—';
    }
  }


  function renderBatchLogs() {
    const root = $('batchLogList');
    if (!root) return;
    const logs = currentDevice()?.logs?.slice(0, 30) || [];
    root.replaceChildren();
    root.classList.toggle('empty', !logs.length);
    if (!logs.length) {
      const empty = document.createElement('div');
      empty.className = 'batchLogEmpty';
      empty.textContent = 'Chưa có hoạt động từ máy.';
      root.append(empty);
      return;
    }
    logs.forEach((item) => {
      const row = document.createElement('div');
      row.className = 'batchLogRow';
      const time = document.createElement('time');
      time.dateTime = new Date(item.time).toISOString();
      time.textContent = timeText(new Date(item.time));
      const message = document.createElement('span');
      message.className = 'logMessage';
      message.textContent = item.message;
      message.title = item.message;
      const duration = document.createElement('span');
      duration.className = 'logDuration';
      duration.textContent = item.duration || '';
      row.append(time, message, duration);
      root.append(row);
    });
  }

  function setCurrentActivity(text, meta, mode = 'active') {
    $('currentActivityText').textContent = text;
    $('currentActivityMeta').textContent = meta;
    $('activityPulse').className = `activityPulse ${mode === 'active' ? '' : mode}`.trim();
    state.currentActivityStartedAt = mode === 'idle' ? 0 : Date.now();
  }

  // ---- Realtime: MQTT 3.1.1 over WSS, one connection per selected device ------
  let realtimeToken = 0;
  let connectingDeviceId = '';

  function sendSession(deviceId, active = true, sync = false) {
    if (!state.realtime?.connected || state.realtime.deviceId !== deviceId) return;
    try {
      publish(routes(deviceId).session, { clientId: controlClientId, active,
        ttlMs: active ? WEB.sessionTtlMs : 1000, sync }, { qos: 0 }).catch(() => {});
    } catch (_) {}
  }

  function selectedNeedsSync() {
    const device = currentDevice();
    return !device?.snapshotAt || Date.now() - device.snapshotAt > WEB.staleAfterMs ||
      !device.config || !validateFullConfig(device.config) ||
      Number(device.snapshot?.revision || 0) > Number(device.revision || 0);
  }

  function clearSyncRetries() {
    state.syncRetryTimers.forEach(clearTimeout);
    state.syncRetryTimers = [];
  }

  // Grants last 300 s and gate the controls (controlReady), so refresh before
  // expiry instead of waiting for a click that the disabled UI would never allow.
  function ensureControlGrant(device) {
    if (readOnlyDevices.has(device.id)) return;
    const control = controlSessions.get(device.id);
    if (control && control.expiresAt > Math.floor(Date.now() / 1000) + 90) return;
    fetchMqttSession(device).then(() => { if (device.id === state.selectedId) renderDevice(); }).catch(() => {});
  }

  // The broker token is short-lived and bound to this device and account: reconnect
  // with a fresh one shortly before it expires or whenever the broker refuses it.
  function renewDeviceChannel(device) {
    if (!device || device.id !== state.selectedId) return;
    disconnectDeviceChannel();
    connectDeviceChannel(device).catch(console.error);
  }

  function activateSelectedSession(sync = false) {
    clearInterval(state.sessionTimer);
    clearSyncRetries();
    const device = currentDevice();
    if (!device || !state.realtime?.connected || state.realtime.deviceId !== device.id || document.hidden) return;
    sendSession(device.id, true, sync);
    if (sync) state.syncRetryTimers = [700, 1600].map((ms) => setTimeout(() => {
      if (device.id === state.selectedId && !document.hidden && selectedNeedsSync()) sendSession(device.id, true, true);
    }, ms));
    // session is QoS 0: keep asking for sync until a complete config and a fresh
    // snapshot arrive, then fall back to plain keep-alive refreshes.
    state.sessionTimer = setInterval(() => {
      if (state.realtimeExpiresAt && state.realtimeExpiresAt - Date.now() < 120000) return renewDeviceChannel(device);
      ensureControlGrant(device);
      sendSession(device.id, true, selectedNeedsSync());
    }, WEB.sessionRefreshMs);
  }

  function deactivateSession() {
    clearInterval(state.sessionTimer);
    clearSyncRetries();
    if (state.selectedId) sendSession(state.selectedId, false, false);
  }

  function setRealtimeStatus(sessionState, message) {
    state.realtimeSessionState = sessionState;
    state.realtimeMessage = message;
    renderDevice();
  }

  function disconnectDeviceChannel() {
    ++realtimeToken;
    connectingDeviceId = '';
    clearTimeout(state.subscriptionRetryTimer);
    clearInterval(state.sessionTimer);
    clearSyncRetries();
    const previous = state.realtime;
    state.realtime = null;
    state.realtimeConnected = false;
    state.subscriptionEpoch++;
    state.subscriptions.clear();
    previous?.end();
  }

  function dispatchRealtimeMessage(device, channel, payload) {
    if (channel === 'presence') handlePresence(device, payload);
    else if (channel === 'snapshot') handleSnapshot(device, payload);
    else if (channel === 'config/reported') handleConfigReport(device, payload);
    else if (channel === 'log') handleLog(device, payload);
    else if (channel === 'history/reported') handleTemperatureHistory(device, payload);
    else if (channel === 'ack') {
      // The ACK must verify against the session key of the transaction it settles.
      verifyDeviceAck(device, payload).then((valid) => {
        if (valid) handleAck(device, payload);
        else if (state.pending.has(String(payload.requestId || '')) ||
            state.uncertain.has(String(payload.requestId || ''))) {
          console.warn('[TX] PROTOCOL_ERROR: ACK không xác thực được');
        }
      }).catch((error) => console.error('[TX] ACK verify', error));
    }
  }

  async function connectDeviceChannel(device) {
    if (!device || connectingDeviceId === device.id) return;
    disconnectDeviceChannel();
    const token = realtimeToken;
    connectingDeviceId = device.id;
    setRealtimeStatus('connecting', 'Đang kết nối với máy…');
    const retryLater = () => {
      clearTimeout(state.subscriptionRetryTimer);
      state.subscriptionRetryTimer = setTimeout(() => {
        if (token === realtimeToken && device.id === state.selectedId) {
          connectingDeviceId = '';
          connectDeviceChannel(device);
        }
      }, 5000);
    };
    let reply;
    try { reply = await fetchMqttSession(device); }
    catch (error) {
      if (token !== realtimeToken) return;
      connectingDeviceId = '';
      setRealtimeStatus(error.code === 'AUTH_ERROR' ? 'auth-required' : 'error',
        error.code === 'AUTH_ERROR' ? error.message : 'Kết nối máy chủ gặp lỗi. Đang thử lại…');
      retryLater();
      return;
    }
    if (token !== realtimeToken) return;
    state.realtimeExpiresAt = Number(reply.mqtt.expiresAt || 0) * 1000;
    const transport = window.MayapMqttTransport.create({
      deviceId: device.id, url: reply.mqtt.url, username: reply.mqtt.username, password: reply.mqtt.password,
      clientId: `mayap-web-${Array.from(crypto.getRandomValues(new Uint8Array(6)),
        (b) => b.toString(16).padStart(2, '0')).join('')}`,
      keepalive: WEB.keepaliveSeconds, reconnectPeriod: WEB.reconnectPeriodMs,
      connectTimeout: WEB.connectTimeoutMs,
      onMessage: (channel, payload) => {
        if (token !== realtimeToken) return;
        const target = state.devices.find((item) => item.id === device.id);
        if (target) dispatchRealtimeMessage(target, channel, payload);
      },
      onState: (next, error) => {
        if (token !== realtimeToken) return;
        // Every connection change starts a new epoch so presence/snapshot from a
        // previous socket can never prove the current device state.
        state.subscriptionEpoch++;
        state.subscriptions.clear();
        if (next === 'subscribing') {
          state.realtimeConnected = false;
          setRealtimeStatus('connecting', 'Đang đăng ký kênh dữ liệu…');
          return;
        }
        if (next === 'ready') {
          state.realtimeConnected = true;
          ensureControlGrant(device);
          setRealtimeStatus('ready', 'Đã kết nối máy chủ');
          activateSelectedSession(true);
          return;
        }
        state.realtimeConnected = false;
        clearInterval(state.sessionTimer);
        clearSyncRetries();
        // CONNACK 4/5: the token expired or was refused; fetch a new one.
        if (next === 'error' && error && (error.code === 4 || error.code === 5)) {
          setTimeout(() => { if (token === realtimeToken) renewDeviceChannel(device); }, 1000);
        }
        setRealtimeStatus(next === 'error' && !state.realtime ? 'error' : 'connecting',
          next === 'reconnecting' ? 'Đang kết nối lại với máy…'
            : next === 'offline' ? 'Thiết bị của bạn đang mất Internet'
              : error ? 'Kết nối máy chủ gặp lỗi. Đang thử lại…' : 'Kết nối bị gián đoạn');
      },
    });
    state.realtime = transport;
  }

  function syncSelectedDevice(force = false) {
    const device = currentDevice();
    if (!device) disconnectDeviceChannel();
    else if (state.realtime?.deviceId !== device.id) connectDeviceChannel(device).catch(console.error);
    else if (state.realtime.connected) activateSelectedSession(force);
    if (device?.config) applyConfigToUi(device, false);
    renderReminderList(device);
    renderDevice();
    renderPushStatus();
  }

  let lastSyncRequestAt = 0;
  async function requestDeviceData(name) {
    // config/log are republished by the device on a session sync request.
    if (['config', 'log'].includes(name) && Date.now() - lastSyncRequestAt > 2000 && state.selectedId) {
      lastSyncRequestAt = Date.now();
      sendSession(state.selectedId, true, true);
    }
    return ['config', 'history', 'log'].includes(name);
  }

  function validateQuickForm() {
    clearInvalid('quickForm');
    const target = Number($('quickTarget').value);
    const interval = Number($('quickTurn').value);
    if (!(target >= 30 && target <= 40)) return invalidate('quickForm', 'quickTarget', 'Nhiệt độ đặt phải từ 30,0 đến 40,0°C.');
    if (!(interval >= 15 && interval <= 720)) return invalidate('quickForm', 'quickTurn', 'Chu kỳ đảo phải từ 15 đến 720 phút.');
    return true;
  }

  function validateBatchForm() {
    clearInvalid('batchForm');
    if (!$('batchName').value.trim()) return invalidate('batchForm', 'batchName', 'Hãy nhập tên mẻ ấp.');
    if (!$('startDate').value) return invalidate('batchForm', 'startDate', 'Hãy chọn ngày bắt đầu.');
    const days = Number($('totalDays').value);
    const temperature = Number($('batchTarget').value);
    const humidity = Number($('targetHumidity').value);
    if (!(days >= 1 && days <= 40)) return invalidate('batchForm', 'totalDays', 'Tổng số ngày ấp phải từ 1 đến 40 ngày.');
    if (!(temperature >= 30 && temperature <= 40)) return invalidate('batchForm', 'batchTarget', 'Nhiệt độ đặt phải từ 30,0 đến 40,0°C.');
    if (!currentDevice()?.config?.humidifierInstalled && !(humidity >= 20 && humidity <= 95)) {
      return invalidate('batchForm', 'targetHumidity', 'Độ ẩm tham khảo phải từ 20 đến 95%RH.');
    }
    return true;
  }

  function validateHumidifierForm() {
    clearInvalid('humidifierForm');
    const on = Number($('humidifierOnHumidity').value);
    const off = Number($('humidifierOffHumidity').value);
    if (!Number.isInteger(off) || off < 30 || off > 90)
      return invalidate('humidifierForm', 'humidifierOffHumidity', 'Ngưỡng tắt phải từ 30 đến 90%RH.');
    if (!Number.isInteger(on) || on < 20 || on > 89 || off - on < 1 || off - on > 10)
      return invalidate('humidifierForm', 'humidifierOnHumidity', 'Ngưỡng bật phải thấp hơn ngưỡng tắt từ 1 đến 10%RH.');
    return true;
  }

  function initSettingHints() {
    let index = 0;
    const addToggle = (label, content) => {
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'settingHintTrigger';
      button.textContent = label.textContent;
      content.id ||= `settingHint${++index}`;
      content.classList.add('settingHintContent');
      content.hidden = true;
      button.setAttribute('aria-controls', content.id);
      button.setAttribute('aria-expanded', 'false');
      button.addEventListener('click', () => {
        content.hidden = !content.hidden;
        button.setAttribute('aria-expanded', String(!content.hidden));
      });
      label.replaceWith(button);
    };
    document.querySelectorAll('#page-settings .notificationLine > div').forEach((row) => {
      const label = row.querySelector(':scope > strong');
      const note = row.querySelector(':scope > small');
      if (label && note) addToggle(label, note);
    });
    const footnotes = {
      'startTune': 'Điều kiện tự dò PID',
      'advancedForm': 'Lưu ý cài đặt nâng cao',
      'renameDeviceForm': 'Phạm vi đổi tên',
      'changePinForm': 'Yêu cầu mã PIN'
    };
    for (const [parentId, label] of Object.entries(footnotes)) {
      const parent = $(parentId);
      const note = parentId === 'startTune'
        ? parent?.parentElement?.querySelector(':scope > .settingFootnote')
        : parent?.querySelector(':scope > .settingFootnote');
      if (!note || note.querySelector('strong')) continue;
      const details = document.createElement('details');
      details.className = 'settingHintNote';
      const summary = document.createElement('summary');
      summary.textContent = label;
      note.replaceWith(details);
      details.append(summary, note);
    }
  }

  function validateTemperatureForm() {
    clearInvalid('temperatureForm');
    const target = Number($('targetTemp').value);
    const low = Number($('lowAlarm').value);
    const high = Number($('highAlarm').value);
    const emergency = Number($('emergencyTemp').value);
    if (!(target >= 30 && target <= 40)) return invalidate('temperatureForm', 'targetTemp', 'Nhiệt độ đặt phải từ 30,0 đến 40,0°C.');
    if (!(low >= 25 && low <= target - 0.1 + 0.0005)) return invalidate('temperatureForm', 'lowAlarm', 'Cảnh báo thấp phải từ 25,0°C và thấp hơn nhiệt độ đặt ít nhất 0,1°C.');
    if (!(high >= target + 0.1 - 0.0005 && high <= 42)) return invalidate('temperatureForm', 'highAlarm', 'Cảnh báo cao phải cao hơn nhiệt độ đặt ít nhất 0,1°C và không vượt 42,0°C.');
    if (!(emergency >= high + 0.1 - 0.0005 && emergency <= 45)) return invalidate('temperatureForm', 'emergencyTemp', 'Ngắt khẩn cấp phải cao hơn cảnh báo cao ít nhất 0,1°C và không vượt 45,0°C.');
    return true;
  }

  function validateVentForm() {
    clearInvalid('ventForm');
    const on = Number($('ventOn').value);
    const off = Number($('ventOff').value);
    const target = Number(currentDevice()?.config?.targetTemp);
    if (!Number.isFinite(on) || on < target + 0.1 - 0.0005)
      return invalidate('ventForm', 'ventOn', 'Ngưỡng bật quạt phải cao hơn nhiệt độ đặt ít nhất 0,1°C.');
    if (!Number.isFinite(off) || off < target - 0.0005)
      return invalidate('ventForm', 'ventOff', 'Ngưỡng tắt quạt không được thấp hơn nhiệt độ đặt.');
    if (on - off < 0.1 - 0.0005) return invalidate('ventForm', 'ventOn', 'Ngưỡng bật phải cao hơn ngưỡng tắt ít nhất 0,1°C.');
    if (!(on <= Number(currentDevice()?.config?.highTempAlarm)))
      return invalidate('ventForm', 'ventOn', 'Ngưỡng bật quạt không được cao hơn cảnh báo nhiệt cao.');
    if (!supportsVentProfile(currentDevice()?.config)) return true;
    const cycle = Number($('ventCycleMinutes').value);
    const level = Number($('ventProfileLevel').value);
    if (!Number.isInteger(level) || level < 0 || level > 2)
      return invalidate('ventForm', 'ventProfileLevel', 'Chọn mức Thấp, Tiêu chuẩn hoặc Cao.');
    if (!Number.isInteger(cycle) || cycle < 40 || cycle > 120 || (cycle - 40) % 10 !== 0)
      return invalidate('ventForm', 'ventCycleMinutes', 'Chu kỳ từ 40 đến 120 phút, tăng giảm mỗi 10 phút như trên máy.');
    for (const key of VENT_PROFILE_KEYS.slice(3)) {
      const duty = Number($(key).value);
      if (!Number.isInteger(duty) || duty < 5 || duty > 90)
        return invalidate('ventForm', key, 'Thời gian chạy từng giai đoạn phải từ 5 đến 90%.');
    }
    return true;
  }

  function validateTurningForm() {
    clearInvalid('turningForm');
    const interval = Number($('turnInterval').value);
    const limit = Number($('limitAlarmTime').value);
    if (!(interval >= 15 && interval <= 720)) return invalidate('turningForm', 'turnInterval', 'Chu kỳ đảo phải từ 15 đến 720 phút.');
    if (!(limit >= 3 && limit <= 120)) return invalidate('turningForm', 'limitAlarmTime', 'Thời gian chờ hành trình phải từ 3 đến 120 giây.');
    return true;
  }

  function validateSensorForm() {
    clearInvalid('sensorForm');
    const temperature = Number($('tempOffset').value);
    const humidity = Number($('humidityOffset').value);
    if (!(temperature >= -5 && temperature <= 5)) return invalidate('sensorForm', 'tempOffset', 'Bù nhiệt độ phải từ −5,0 đến 5,0°C.');
    if (!(humidity >= -20 && humidity <= 20)) return invalidate('sensorForm', 'humidityOffset', 'Bù độ ẩm phải từ −20 đến 20%RH.');
    return true;
  }

  function validateAdvancedForm() {
    clearInvalid('advancedForm');
    const kp = Number($('advKp').value);
    const ki = Number($('advKi').value);
    const kd = Number($('advKd').value);
    const maxHeaterPower = Number($('advMaxHeaterPower').value);
    const tempRateLimitC = Number($('advTempRateLimitC').value);
    const tempRateWindowSec = Number($('advTempRateWindowSec').value);
    const tempOscillationCrossLimit = Number($('advTempOscillationCrossLimit').value);
    const tempOscillationWindowSec = Number($('advTempOscillationWindowSec').value);
    const heaterStuckMinRiseC = Number($('advHeaterStuckMinRiseC').value);
    const heaterStuckDurationSec = Number($('advHeaterStuckDurationSec').value);
    const autotuneRelayPowerPercent = Number($('advAutotuneRelayPowerPercent').value);
    const autotuneBandC = Number($('advAutotuneBandC').value);
    if (!(kp >= 0 && kp <= 100)) return invalidate('advancedForm', 'advKp', 'Hệ số Kp phải từ 0 đến 100.');
    if (!(ki >= 0 && ki <= 20)) return invalidate('advancedForm', 'advKi', 'Hệ số Ki phải từ 0 đến 20.');
    if (!(kd >= 0 && kd <= 200)) return invalidate('advancedForm', 'advKd', 'Hệ số Kd phải từ 0 đến 200.');
    if (kp === 0 && ki === 0 && kd === 0) return invalidate('advancedForm', 'advKp', 'Kp, Ki và Kd không được đồng thời bằng 0.');
    if (!(maxHeaterPower >= 10 && maxHeaterPower <= 100)) return invalidate('advancedForm', 'advMaxHeaterPower', 'Trần công suất phải từ 10 đến 100%.');
    if (!(tempRateLimitC >= 0.1 && tempRateLimitC <= 10)) return invalidate('advancedForm', 'advTempRateLimitC', 'Ngưỡng tốc độ phải từ 0,1 đến 10°C.');
    if (!(tempRateWindowSec >= 30 && tempRateWindowSec <= 1800)) return invalidate('advancedForm', 'advTempRateWindowSec', 'Khung thời gian phải từ 30 đến 1800 giây.');
    if (!(tempOscillationCrossLimit >= 2 && tempOscillationCrossLimit <= 30)) return invalidate('advancedForm', 'advTempOscillationCrossLimit', 'Số lần dao động phải từ 2 đến 30.');
    if (!(tempOscillationWindowSec >= 60 && tempOscillationWindowSec <= 3600)) return invalidate('advancedForm', 'advTempOscillationWindowSec', 'Khung thời gian phải từ 60 đến 3600 giây.');
    if (!(heaterStuckMinRiseC >= 0.05 && heaterStuckMinRiseC <= 5)) return invalidate('advancedForm', 'advHeaterStuckMinRiseC', 'Ngưỡng tăng tối thiểu phải từ 0,05 đến 5°C.');
    if (!(heaterStuckDurationSec >= 60 && heaterStuckDurationSec <= 3600)) return invalidate('advancedForm', 'advHeaterStuckDurationSec', 'Thời gian xác nhận phải từ 60 đến 3600 giây.');
    if (!(autotuneRelayPowerPercent >= 10 && autotuneRelayPowerPercent <= 80)) return invalidate('advancedForm', 'advAutotuneRelayPowerPercent', 'Công suất relay tự dò phải từ 10 đến 80%.');
    if (!(autotuneBandC >= 0.05 && autotuneBandC <= 1)) return invalidate('advancedForm', 'advAutotuneBandC', 'Dải xác nhận tự dò phải từ 0,05 đến 1°C.');
    return true;
  }

  function getThemePreference() {
    try {
      const value = localStorage.getItem(THEME_STORAGE);
      return value === 'light' || value === 'dark' ? value : 'system';
    } catch (e) {
      return 'system';
    }
  }

  function applyTheme(choice) {
    if (choice === 'light' || choice === 'dark') {
      document.documentElement.setAttribute('data-theme', choice);
      try { localStorage.setItem(THEME_STORAGE, choice); } catch (e) { /* ignore */ }
    } else {
      document.documentElement.removeAttribute('data-theme');
      try { localStorage.removeItem(THEME_STORAGE); } catch (e) { /* ignore */ }
    }
    const toggle = $('themeToggle');
    if (toggle) {
      toggle.querySelectorAll('button').forEach((button) => {
        const checked = button.dataset.themeChoice === choice;
        button.setAttribute('aria-checked', checked ? 'true' : 'false');
      });
    }
    const summary = $('themeSummary');
    if (summary) {
      summary.textContent = choice === 'light' ? 'Sáng' : choice === 'dark' ? 'Tối' : 'Theo hệ thống';
    }
    syncBrowserTheme();
  }

  function syncBrowserTheme() {
    const css = getComputedStyle(document.documentElement);
    const background = css.getPropertyValue('--bodyBg').trim();
    if (background) document.querySelector('meta[name="theme-color"]')?.setAttribute('content', background);
    document.querySelector('meta[name="color-scheme"]')?.setAttribute('content', css.colorScheme);
    requestTemperatureChartRender();
  }

  function bindUi() {
    initSettingHints();
    bindMobileSwipe();
    document.querySelectorAll('.notificationLine input[type="checkbox"]').forEach((input) => {
      const name = input.closest('.notificationLine')?.querySelector('strong, .settingHintTrigger')?.textContent;
      if (name) input.setAttribute('aria-label', name.trim());
    });
    $('confirmCancel').addEventListener('click', () => finishConfirm(false));
    $('confirmAccept').addEventListener('click', () => finishConfirm(true));
    $('confirmDialog').addEventListener('cancel', (event) => { event.preventDefault(); finishConfirm(false); });

    document.querySelectorAll('.settingCard').forEach((card) => card.addEventListener('toggle', () => {
      if (!card.open) return;
      document.querySelectorAll('.settingCard').forEach((other) => { if (other !== card) other.open = false; });
    }));
    document.querySelectorAll('.nav button').forEach((button) => button.addEventListener('click', () => showPage(button.dataset.page)));

    $('deviceSelector').addEventListener('change', (event) => {
      const previous = state.selectedId;
      state.selectedId = event.target.value;
      saveDevices();
      syncSelectedDevice(true);
      syncDeviceSelectorUi();
      toast(`Đã chọn ${currentDevice()?.name || 'thiết bị'}`);
    });

    $('deviceSelectorTrigger').addEventListener('click', () => {
      const panel = $('deviceSelectorPanel');
      if (panel.hidden) openDeviceSelectorPanel(); else closeDeviceSelectorPanel();
    });
    document.addEventListener('click', (event) => {
      const field = $('deviceSelectorField');
      if (field && !field.contains(event.target)) closeDeviceSelectorPanel();
    });
    document.addEventListener('keydown', (event) => {
      if (event.key === 'Escape') closeDeviceSelectorPanel();
    });

    $('liveStateTile').addEventListener('click', (event) => {
      const device = currentDevice();
      if (!device?.activeFaults?.length) return;
      event.stopPropagation();
      const popup = $('faultPopup');
      if (popup && !popup.hidden) closeFaultPopup();
      else openFaultPopup(device);
    });
    document.addEventListener('click', () => closeFaultPopup());

    $('addDeviceBtn').addEventListener('click', () => {
      $('newDeviceId').value = '';
      $('deviceDialog').showModal();
      setTimeout(() => $('newDeviceId').focus(), 60);
    });
    $('closeDeviceDialog').addEventListener('click', () => $('deviceDialog').close());
    $('deviceDialog').addEventListener('click', (event) => {
      if (event.target === $('deviceDialog')) $('deviceDialog').close();
    });
    // Dung 1 su kien 'close' chung cho MOI cach dong hop thoai (nut X, bam ra
    // ngoai, phim Esc, hay them thanh cong) thay vi rai stopQrScan() vao tung
    // handler rieng - dam bao camera luon tat, khong bao gio bi "quen" chay
    // ngam sau khi hop thoai da dong.
    $('deviceDialog').addEventListener('close', () => stopQrScan());
    $('scanQrBtn').addEventListener('click', () => startQrScan());
    $('qrScannerCancel').addEventListener('click', () => stopQrScan());
    $('addDeviceForm').addEventListener('submit', async (event) => {
      event.preventDefault();
      const id = normalizeDeviceId($('newDeviceId').value);
      const pin = $('newDevicePin').value.trim();
      if (!DEVICE_ID_RE.test(id)) return toast('Mã máy có dạng MAP-A1B2C3D4E5F6');
      if (!/^[0-9]{4,8}$/.test(pin)) return toast('Mã PIN phải là 4-8 chữ số');

      const submitBtn = event.target.querySelector('button[type="submit"]');
      submitBtn.disabled = true;
      submitBtn.textContent = 'Đang kiểm tra…';
      let result;
      try {
        result = await verifyDevicePin(id, pin);
      } finally {
        submitBtn.disabled = false;
        submitBtn.textContent = 'Thêm và chọn máy';
      }
      if (!result.success) return toast(result.error || 'Sai mã PIN hoặc thiết bị chưa đăng ký');
      await window.MayapAccount.refresh();

      // Ten hien thi lay tu server (da dat san tu truoc, hoac mac dinh la
      // chinh device_id) - KHONG cho nguoi dung tu go ten luc them nua, vi
      // ten gio la thuoc tinh CHUNG cua thiet bi (doi trong Cai dat), khong
      // phai rieng cua tung trinh duyet.
      const name = result.device_name || id;
      const existed = state.devices.find((device) => device.id === id);
      if (existed) {
        existed.name = name;
        existed.pairingToken = 'account-session';
      } else {
        state.devices.push(createDevice(id, name, 'account-session'));
      }
      const previous = state.selectedId;
      state.selectedId = id;
      renderSelector();
      $('deviceDialog').close();
      // Neu thong bao da bat san tren trinh duyet nay, tu lien ket luon may
      // moi them vao (khong bat nguoi dung phai bam lai "Bat thong bao").
      renderPushStatus();
      saveDevices();
      toast('Đã thêm máy');
      controlSessions.delete(id);
    });

    $('remindersForm').addEventListener('submit', async event => {
      event.preventDefault();const device=currentDevice(),errorEl=$('remindersFormError');
      errorEl.textContent='';errorEl.classList.remove('show');
      if(!device)return toast('Hãy chọn thiết bị trước');
      const day=Number($('reminderDayInput').value),label=$('reminderLabelInput').value.trim();
      if(!Number.isInteger(day)||day<1||day>99||!label){errorEl.textContent='Nhập ngày ấp từ 1–99 và nội dung nhắc.';errorEl.classList.add('show');return;}
      try{
        await addCloudReminder(device,day,label);
        $('reminderDayInput').value='';$('reminderLabelInput').value='';toast('Đã lưu Nhắc nhở trên Cloud');
      }catch(error){errorEl.textContent=error.message;errorEl.classList.add('show');}
    });

    $('renameDeviceForm').addEventListener('submit', async (event) => {
      event.preventDefault();
      const device = currentDevice();
      const errorEl = $('renameDeviceError');
      errorEl.classList.remove('show');
      if (!device) return toast('Hãy chọn thiết bị trước');
      const name = $('renameDeviceName').value.trim();
      if (!name) return toast('Hãy nhập tên hiển thị mới');

      const submitBtn = event.target.querySelector('button[type="submit"]');
      submitBtn.disabled = true;
      submitBtn.textContent = 'Đang lưu…';
      let result;
      try {
        result = await renameDeviceRemote(device.id, name);
      } finally {
        submitBtn.disabled = false;
        submitBtn.textContent = 'Đổi tên máy';
      }
      if (!result.success) {
        errorEl.textContent = result.error || 'Không đổi được tên máy';
        errorEl.classList.add('show');
        return;
      }
      device.name = result.device_name || name;
      const accountRow = window.MayapAccount.current?.devices.find(row => row.device_id === device.id);
      if (accountRow) accountRow.device_name = device.name;
      saveDevices();
      renderSelector();
      toast('Đã đổi tên máy');
    });

    $('changePinForm').addEventListener('submit', async (event) => {
      event.preventDefault();
      const device = currentDevice();
      const errorEl = $('changePinError');
      errorEl.classList.remove('show');
      if (!device) return toast('Hãy chọn thiết bị trước');
      const oldPin = $('changePinOld').value.trim();
      const newPin1 = $('changePinNew1').value.trim();
      const newPin2 = $('changePinNew2').value.trim();
      if (!/^[0-9]{6,8}$/.test(newPin1)) return toast('Mã PIN mới phải gồm 6–8 chữ số');
      if (newPin1 !== newPin2) {
        errorEl.textContent = 'Mã PIN mới nhập lại không khớp';
        errorEl.classList.add('show');
        return;
      }

      const submitBtn = event.target.querySelector('button[type="submit"]');
      submitBtn.disabled = true;
      submitBtn.textContent = 'Đang đổi…';
      let result;
      try {
        result = await changeDevicePin(device.id, oldPin, newPin1);
      } finally {
        submitBtn.disabled = false;
        submitBtn.textContent = 'Đổi mã PIN';
      }
      if (!result.success) {
        errorEl.textContent = result.error || 'Không đổi được mã PIN';
        errorEl.classList.add('show');
        return;
      }
      event.target.reset();
      toast('Đã đổi mã PIN thiết bị');
    });

    $('quickForm').addEventListener('focusin', () => requestDeviceData('config'));
    $('remindersForm').closest('details')?.addEventListener('toggle', event => {
      if(event.currentTarget.open)loadCloudReminders(currentDevice());
    });
    $('batchLogList')?.closest('details')?.addEventListener('toggle', (event) => {
      if (event.currentTarget.open) requestDeviceData('log');
    });
    $('quickForm').addEventListener('submit', async (event) => {
      event.preventDefault();
      if (!validateQuickForm()) return;
      $('targetTemp').value = $('quickTarget').value;
      $('batchTarget').value = $('quickTarget').value;
      $('turnInterval').value = $('quickTurn').value;
      updateSettingSummaries();
      await sendConfig('quickForm', 'quick');
    });

    $('batchForm').addEventListener('submit', async (event) => {
      event.preventDefault();
      if (!validateBatchForm()) return;
      $('quickTarget').value = $('batchTarget').value;
      $('targetTemp').value = $('batchTarget').value;
      await sendConfig('batchForm', 'batch');
    });

    $('batchAction').addEventListener('click', async () => {
      const device = currentDevice();
      const running = Boolean(device?.snapshot?.runtime?.batchRunning);

      if (!running) {
        if (hasDirtyForm('batchForm')) return toast('Hãy lưu cấu hình mẻ trước khi bắt đầu');
        if (!validateBatchForm()) return;
        const ok = await confirmAction({
          title: 'Bắt đầu mẻ ấp?',
          message: `Máy sẽ bắt đầu mẻ “${$('batchName').value.trim()}” theo cấu hình đã được máy xác nhận.`,
          accept: 'Bắt đầu mẻ'
        });
        if (!ok) return;
        if (!$('resumeAfterPowerLoss').checked) {
          const confirmed = await confirmAction({
            title: 'Bắt đầu khi đã tắt khôi phục?',
            message: 'Nếu mất điện, mẻ sẽ không tự tiếp tục. Bạn xác nhận vẫn bắt đầu mẻ với lựa chọn này?',
            accept: 'Vẫn bắt đầu mẻ', danger: true
          });
          if (!confirmed) return;
        }
        setFormError('batchForm', '');
        beginBatchActionPending(device, 'running');
        if (!await sendCommand('batch_start')) {
          clearBatchActionPending(device);
          renderBatchAction(device);
        }
        return;
      }

      const ok = await confirmAction({
        title: 'Kết thúc mẻ ấp?',
        message: 'Thanh nhiệt và cơ cấu đảo sẽ dừng theo trình tự an toàn của máy.',
        accept: 'Kết thúc mẻ',
        danger: true
      });
      if (!ok) return;
      setFormError('batchForm', '');
      beginBatchActionPending(device, 'stopped');
      if (!await sendCommand('batch_stop')) {
        clearBatchActionPending(device);
        renderBatchAction(device);
      }
    });

    // O "Den" tren outputStrip gio la nut bam: bat/tat den tuc thi, khong
    // can xac nhan (thao tac nhe, khong anh huong an toan van hanh).
    $('outputLightBtn')?.addEventListener('click', () => sendCommand('light_toggle'));

    // O "Coi bao" chi bam duoc khi coi THUC SU dang keu (xem toggle disabled
    // trong applySnapshotToUi) - giong het nut ACK tren HMI, tat coi tam 5
    // phut chu KHONG tat han canh bao (loi goc van con neu chua khac phuc).
    $('outputSirenBtn')?.addEventListener('click', async () => {
      const device = currentDevice();
      if (!device?.snapshot?.runtime?.sirenOn) return;
      await sendCommand('alarm_ack');
    });

    $('temperatureForm').addEventListener('submit', async (event) => {
      event.preventDefault();
      if (!validateTemperatureForm()) return;
      $('quickTarget').value = $('targetTemp').value;
      $('batchTarget').value = $('targetTemp').value;
      updateSettingSummaries();
      await sendConfig('temperatureForm', 'temperature');
    });

    $('ventAutoEnabled').addEventListener('change', () => {
      $('ventAutoOptions').hidden = !$('ventAutoEnabled').checked;
      updateSettingSummaries();
    });
    $('ventProfileLevel').addEventListener('change', updateSettingSummaries);
    $('ventForm').addEventListener('submit', async (event) => {
      event.preventDefault();
      if (!validateVentForm()) return;
      updateSettingSummaries();
      await sendConfig('ventForm', 'vent');
    });

    $('turningForm').addEventListener('submit', async (event) => {
      event.preventDefault();
      if (!validateTurningForm()) return;
      const device = currentDevice();
      const enabling = $('turningEnabled').checked;
      const turningChanged = device?.config && enabling !== Boolean(device.config.turningEnabled);
      if (turningChanged) {
        const ok = await confirmAction({
          title: enabling ? 'Bật đảo tự động?' : 'Tắt đảo tự động?',
          message: enabling
            ? 'Máy sẽ tự động đảo trứng theo chu kỳ và công tắc hành trình.'
            : 'Máy sẽ ngừng tự động đảo trứng cho tới khi bật lại - kiểm tra kỹ nếu đang có mẻ ấp.',
          accept: enabling ? 'Bật đảo tự động' : 'Tắt đảo tự động',
          danger: !enabling
        });
        if (!ok) {
          $('turningEnabled').checked = Boolean(device.config.turningEnabled);
          return;
        }
      }
      $('quickTurn').value = $('turnInterval').value;
      updateSettingSummaries();
      await sendConfig('turningForm', 'turning');
    });

    $('sensorForm').addEventListener('submit', async (event) => {
      event.preventDefault();
      if (!validateSensorForm()) return;
      updateSettingSummaries();
      await sendConfig('sensorForm', 'sensor');
    });

    $('lightAlarmForm').addEventListener('submit', async (event) => {
      event.preventDefault();
      await sendConfig('lightAlarmForm', 'lightAlarm');
    });

    $('humidifierForm').addEventListener('submit', async (event) => {
      event.preventDefault();
      if (!validateHumidifierForm()) return;
      updateSettingSummaries();
      await sendConfig('humidifierForm', 'humidifier');
    });

    $('advancedForm').addEventListener('submit', async (event) => {
      event.preventDefault();
      if (!validateAdvancedForm()) return;
      await sendConfig('advancedForm', 'advanced');
    });

    $('startTune').addEventListener('click', async () => {
      const runtime = currentDevice()?.snapshot?.runtime;
      if (Number(runtime?.autoTuneState || 0) === 1) {
        const ok = await confirmAction({
          title: 'Hủy tự dò PID?',
          message: 'Dừng ngay quá trình tự dò hiện tại. Các thông số PID cũ vẫn được giữ nguyên.',
          accept: 'Hủy tự dò'
        });
        if (ok) await sendCommand('autotune_cancel');
        return;
      }
      if (runtime?.batchRunning) return toast('Không thể tự dò khi mẻ đang chạy');
      const ok = await confirmAction({
        title: 'Bắt đầu tự dò PID?',
        message: 'Chỉ thực hiện khi khoang ấp trống. Máy sẽ tự điều khiển và lưu kết quả.',
        accept: 'Bắt đầu tự dò'
      });
      if (ok) await sendCommand('autotune_start');
    });

    $('themeToggle').querySelectorAll('button').forEach((button) => {
      button.addEventListener('click', () => applyTheme(button.dataset.themeChoice));
    });

    $('enablePushBtn').addEventListener('click', onEnablePushClick);
    $('testPushBtn').addEventListener('click', onTestPushClick);

    $('openWifiPortal').addEventListener('click', async () => {
      const ok = await confirmAction({
        title: 'Đã kết nối vào MAYAP-XXXX chưa?',
        message: 'Bấm "Mở trang đổi Wi‑Fi" chỉ hoạt động khi điện thoại đang kết nối vào mạng MAYAP-XXXX của máy ấp (bước 1-2 trong hướng dẫn). Nếu chưa, hãy thoát ra làm 2 bước đó trước.',
        accept: 'Mở trang đổi Wi‑Fi'
      });
      if (ok) window.location.href = 'http://192.168.4.1/';
    });

    ['quickForm', 'batchForm', 'temperatureForm', 'ventForm', 'turningForm', 'sensorForm', 'lightAlarmForm', 'humidifierForm', 'advancedForm'].forEach(registerDirty);
  }

  function startTimers() {
    clearInterval(state.staleTimer);
    state.staleTimer = setInterval(() => {
      sweepUncertain();
      for (const [id, entry] of state.lastTerminalByDevice)
        if (Date.now() - entry.at > PACKET_POLICY.UNCERTAIN_TTL_MS)
          state.lastTerminalByDevice.delete(id);
      for (const [id, entry] of terminalOutcomes)
        if (performance.now() - entry.at > PACKET_POLICY.UNCERTAIN_TTL_MS)
          terminalOutcomes.delete(id);
      renderDevice();
    }, 5000);
  }

  // Web Push (thay Telegram) - trang thai va nut bam trong card "Thông báo".
  // Xem push.js cho toan bo logic Service Worker/Push API/VAPID.
  let pushStatusToken = 0;
  const PUSH_STATUS_TEXT = {
    unsupported: ['Không hỗ trợ', 'pill offline', 'Trình duyệt này không hỗ trợ thông báo đẩy.'],
    'not-configured': ['Chưa cấu hình', 'pill offline', 'Trang web chưa cấu hình máy chủ thông báo (cloudApiBase).'],
    // Khong con mo ta rieng ("Lam theo huong dan ben duoi roi bam lai.") -
    // thua va SAI HUONG (huong dan iOS nam TREN dong nay, khong phai duoi),
    // vi #pushIosGuide da tu hien dung luc can (xem duoi) roi.
    'ios-needs-install': ['Cần thêm vào Màn hình chính', 'pill soft', ''],
    denied: ['Bị chặn', 'pill offline', 'Trình duyệt đang chặn thông báo - vào cài đặt trình duyệt để cho phép lại.'],
    'not-enabled': ['Chưa cấp quyền', 'pill soft', ''],
    enabled: ['Đã bật', 'pill online', ''],
    error: ['Lỗi kết nối', 'pill offline', 'Không liên lạc được với máy chủ thông báo, thử lại sau.']
  };

  async function renderPushStatus() {
    const myToken = ++pushStatusToken;
    const pill = $('pushStatusPill');
    const summary = $('notificationSummary');
    const iosGuide = $('pushIosGuide');
    const enableBtn = $('enablePushBtn');
    const testBtn = $('testPushBtn');
    const errorText = $('pushErrorText');
    const device = currentDevice();

    const zeroBanner = $('pushZeroDevicesBanner');
    if (!device) {
      pill.textContent = '—';
      pill.className = 'pill soft';
      summary.textContent = 'Chưa có thiết bị';
      iosGuide.hidden = true;
      errorText.hidden = true;
      enableBtn.disabled = true;
      enableBtn.textContent = 'Bật thông báo';
      testBtn.hidden = true;
      if (zeroBanner) zeroBanner.classList.remove('show');
      return;
    }
    if (!window.MayapPush) return;

    // Truyen TOAN BO device_id dang co (khong chi may dang chon) - "bat thong
    // bao" ap dung cho ca dashboard, tu dong lien ket may moi neu thieu.
    const allDeviceIds = state.devices.map((item) => item.id);
    const pushState = await window.MayapPush.getState(allDeviceIds);
    if (myToken !== pushStatusToken) return; // co yeu cau moi hon xen vao, bo ket qua cu

    const [text, cls, desc] = PUSH_STATUS_TEXT[pushState.status] || PUSH_STATUS_TEXT['not-enabled'];
    pill.textContent = text;
    pill.className = cls;
    summary.textContent = text;
    iosGuide.hidden = pushState.status !== 'ios-needs-install';
    errorText.hidden = !desc;
    if (desc) errorText.textContent = desc;
    enableBtn.disabled = false;
    enableBtn.textContent = pushState.status === 'enabled' ? 'Tắt thông báo' : 'Bật thông báo';
    testBtn.hidden = pushState.status !== 'enabled';

    // Rieng canh bao "chua ai nhan thong bao": doc so lien ket THAT tren D1
    // (khong chi trinh duyet nay) - im lang neu khong xac dinh duoc (null),
    // chi hien khi CHAC CHAN la 0 de tranh bao nham luc mat mang.
    if (zeroBanner) {
      const linkedCount = await window.MayapPush.getLinkedCount(device.id);
      if (myToken !== pushStatusToken) return;
      zeroBanner.classList.toggle('show', linkedCount === 0);
    }
  }

  function pushReasonText(reason, detail) {
    const map = {
      unsupported: 'Trình duyệt này không hỗ trợ thông báo đẩy.',
      'not-configured': 'Trang web chưa cấu hình máy chủ thông báo.',
      'ios-needs-install': 'Hãy thêm MAYAP vào Màn hình chính trước (xem hướng dẫn bên dưới).',
      denied: 'Bạn đã từ chối hoặc trình duyệt đang chặn thông báo.',
      'no-device': 'Hãy chọn hoặc thêm thiết bị trước.',
      error: `Lỗi: ${detail || 'không xác định'}`
    };
    return map[reason] || 'Không bật được thông báo, thử lại sau.';
  }

  async function onEnablePushClick() {
    const device = currentDevice();
    if (!device) return toast('Hãy thêm máy trước');
    if (!window.MayapPush) return toast('push.js chưa tải xong, thử lại sau vài giây');

    const btn = $('enablePushBtn');
    const turningOff = btn.textContent.trim() === 'Tắt thông báo';
    btn.disabled = true;
    btn.textContent = turningOff ? 'Đang tắt…' : 'Đang bật…';
    try {
      if (turningOff) {
        await window.MayapPush.disable();
        toast('Đã tắt thông báo trên trình duyệt này');
      } else {
        const allDeviceIds = state.devices.map((item) => item.id);
        const result = await window.MayapPush.enable(allDeviceIds);
        toast(result.ok ? '🔔 Đã bật thông báo cho tất cả thiết bị trên dashboard này' : pushReasonText(result.reason, result.error));
      }
    } finally {
      await renderPushStatus();
    }
  }

  async function onTestPushClick() {
    if (!window.MayapPush) return;
    const btn = $('testPushBtn');
    btn.disabled = true;
    btn.textContent = 'Đang gửi…';
    try {
      const result = await window.MayapPush.testNotification();
      if (result.ok) {
        toast('Đã gửi thông báo test - chờ vài giây trên điện thoại');
      } else if (result.pushStatus) {
        const detail = `Chưa gửi được (mã lỗi ${result.pushStatus}${result.pushError ? ': ' + result.pushError : ''})`;
        console.error('[push] test that bai:', detail);
        toast(detail, 9000);
      } else {
        toast('Chưa gửi được, thử bật lại thông báo');
      }
    } finally {
      btn.disabled = false;
      btn.textContent = 'Kiểm tra thông báo';
    }
  }

  function registerServiceWorker() {
    if (!('serviceWorker' in navigator) || location.protocol === 'file:') return;
    navigator.serviceWorker.register('./sw.js').catch(() => {});
  }

  // Mo tu link trong thong bao day (?device=MAP-XXXX) - tu chon dung thiet bi
  // thay vi luon mo theo thiet bi da chon truoc do/mac dinh, de bam vao thong
  // bao la thay dung may dang bao loi ngay, khong phai vao "trang trong".
  function applyDeepLinkDevice() {
    const requested = normalizeDeviceId(new URLSearchParams(window.location.search).get('device') || '');
    if (!requested) return;
    const exists = state.devices.some((item) => item.id === requested);
    if (exists && state.selectedId !== requested) {
      state.selectedId = requested;
      saveDevices();
    }
  }

  function accountDevices(rows) {
    if (!window.MayapAccount?.current) return;
    const ids = new Set(rows.map(row => row.device_id));
    const removed = state.devices.filter(device => !ids.has(device.id));
    if (removed.length) controlSessions.clear();
    state.devices = rows.map(row => {
      const device = state.devices.find(d => d.id === row.device_id) || createDevice(row.device_id, row.device_name || row.device_id, 'account-session');
      device.name = row.device_name || row.device_id;
      device.accountRole = row.role;
      if (row.role === 'viewer') controlSessions.delete(device.id);
      return device;
    });
    if (!ids.has(state.selectedId)) state.selectedId = state.devices[0]?.id || '';
  }
  window.addEventListener('mayap-logout', () => {
    controlSessions.clear(); state.devices = [];
  });
  async function init() {
    if (!window.MayapAccount) return; // Production always installs the account gate.
    const account = await window.MayapAccount.ready;
    if (!account) return;
    STORAGE = `mayap.account.${account.user.sub}`;
    RUNTIME_CACHE = `${STORAGE}.runtime.v1`;
    state.selectedId = localStorage.getItem(`${STORAGE}.selected`) || '';
    accountDevices(account.devices);
    window.addEventListener('mayap-account-devices', event => {
      accountDevices(event.detail); renderSelector();
      syncSelectedDevice();
    });
    // Goi showPage() thay vi chi dat dataset.page: truoc day tieu de va chu
    // thich luc moi mo trang lay tu chuoi VIET CUNG trong index.html (vi
    // showPage chi chay khi bam nut chuyen trang), nen moi lan doi chu trong
    // pageMeta ma quen sua index.html la nguoi dung van thay chuoi cu.
    showPage('device');
    applyTheme(getThemePreference());
    applyDeepLinkDevice();
    bindUi();
    notesUi = window.MayapNotes?.mount({
      confirm: confirmAction,
      getContext: () => {
        const device = currentDevice();
        return { deviceId: device?.id || '', deviceName: device?.name || '',
          batchRunning: Boolean(device?.snapshot?.runtime?.batchRunning),
          notesWritable: Boolean(device?.id && device.accountRole!=='viewer'),
          notesStatus: !device?.id?'Hãy chọn máy trước.':device.accountRole==='viewer'?'Tài khoản này chỉ có quyền xem.':'Lưu trực tiếp trên Cloud.' };
      },
      listNotes:listCloudNotes,
      saveNote:saveCloudNote,
      deleteNote:deleteCloudNote
    });
    renderSelector();
    document.documentElement.dataset.auth = 'ready';
    updateSettingSummaries();
    renderBatchLogs();
    if (!currentDevice()?.snapshot) setCurrentActivity('Đang kết nối', 'Đang chờ dữ liệu từ máy', 'idle');
    startTimers();
    registerServiceWorker();
    renderPushStatus();
    renderDevice();
  }

  window.addEventListener('pagehide', () => { persistRuntimeCache(currentDevice(), true); deactivateSession(); });
  window.addEventListener('resize', requestTemperatureChartRender, { passive: true });
  window.matchMedia?.('(prefers-color-scheme: dark)')?.addEventListener('change', () => {
    if (getThemePreference() === 'system') syncBrowserTheme();
  });
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) { persistRuntimeCache(currentDevice(), true); deactivateSession(); }
    else { renderDevice(); activateSelectedSession(true); }
  });

  let notesUi;
  init();
})();
