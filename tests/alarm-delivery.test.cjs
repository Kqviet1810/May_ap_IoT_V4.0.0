const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const {DatabaseSync}=require('node:sqlite');
const loaded=Promise.all([import('../cloudflare/src/alarm-delivery.js'),import('../cloudflare/src/index.js'),import('../cloudflare/src/auth.js')]);
async function setup(){
 const [delivery,worker,auth]=await loaded,sql=new DatabaseSync(':memory:');
 sql.exec(fs.readFileSync('cloudflare/schema.sql','utf8'));
 sql.exec(fs.readFileSync('cloudflare/migrations/0004_accounts.sql','utf8'));
 sql.exec(fs.readFileSync('cloudflare/migrations/0007_alarm_delivery.sql','utf8'));
 const DB={prepare(source){const stmt=sql.prepare(source);let args=[];return{bind(...a){args=a;return this;},
  async first(){return stmt.get(...args)||null;},async all(){return {results:stmt.all(...args)};},
  async run(){return {meta:stmt.run(...args)};}};},async batch(statements){sql.exec('BEGIN');try{const out=[];for(const s of statements)out.push(await s.run());sql.exec('COMMIT');return out;}catch(e){sql.exec('ROLLBACK');throw e;}}};
 const key=await crypto.subtle.generateKey({name:'ECDH',namedCurve:'P-256'},true,['deriveBits']);
 const pub=Buffer.from(await crypto.subtle.exportKey('raw',key.publicKey)).toString('base64url');
 const priv=await crypto.subtle.exportKey('jwk',key.privateKey);
 const env={DB,DEVICE_KEY_PEPPER:'test-pepper',ALLOWED_ORIGIN:'https://web.test',
  VAPID_SUBJECT:'mailto:tests@example.test',VAPID_PUBLIC_KEY:pub,VAPID_PRIVATE_KEY:priv.d};
 const deviceId='MAP-441BF6E051D0',secret='test-device-key';
 sql.prepare('INSERT INTO devices(device_id,device_key_hash,created_at) VALUES(?,?,?)').run(deviceId,await auth.hashDeviceKey(secret,env.DEVICE_KEY_PEPPER),0);
 sql.prepare("INSERT INTO users(google_sub,created_at,last_login_at) VALUES('owner',0,0)").run();
 sql.prepare("INSERT INTO user_devices(user_sub,device_id,role,created_at) VALUES('owner',?,'owner',0)").run(deviceId);
 function subscribe(endpoint){sql.prepare(`INSERT INTO push_subscriptions(device_id,endpoint,p256dh,auth,user_sub,created_at,updated_at)
  VALUES(?,?,?,?,'owner',0,0)`).run(deviceId,endpoint,pub,Buffer.alloc(16,1).toString('base64url'));}
 subscribe('https://push.test/phone1');
 const notification={title:'alarm',body:'off',data:{deviceId,alarmType:'FAULT_130',severity:'critical',state:'active'}};
 const jobs=[];const ctx={waitUntil(p){jobs.push(p);}};
 async function alarm(eventId='boot-0001',state='active',message='off'){
  return worker.default.fetch(new Request('https://worker.test/api/device/alarm',{method:'POST',body:JSON.stringify({device_id:deviceId,device_key:secret,event_id:eventId,alarm_type:'FAULT_130',state,message,severity:'critical'})}),env,ctx);
 }
 async function batch(events,key=secret){
  return worker.default.fetch(new Request('https://worker.test/api/device/alarms',{method:'POST',body:JSON.stringify({device_id:deviceId,device_key:key,events})}),env,ctx);
 }
 return{delivery,env,sql,deviceId,notification,subscribe,alarm,batch,jobs};
}
test('durable HTTP receipt, response-loss duplicate and event conflict',async()=>{
 const h=await setup(),original=global.fetch;global.fetch=async()=>new Response('',{status:201});
 try{
  const first=await (await h.alarm()).json();assert.equal(first.durable,true);assert.equal(first.event_id,'boot-0001');
  const duplicate=await (await h.alarm()).json();assert.equal(duplicate.duplicate,true);
  assert.equal(h.sql.prepare('SELECT COUNT(*) n FROM alarm_events').get().n,1);
  assert.equal((await h.alarm('boot-0001','resolved','on')).status,409);
  await Promise.all(h.jobs);
  assert.equal(h.sql.prepare('SELECT status FROM alarm_deliveries').get().status,'sent');
 }finally{global.fetch=original;}
});
test('atomic persistence failure never returns a durable receipt',async()=>{
 const h=await setup();h.env.DB.batch=async()=>{throw Error('injected storage failure');};
 assert.equal((await h.alarm()).status,500);
 assert.equal(h.sql.prepare('SELECT COUNT(*) n FROM alarm_events').get().n,0);
});
test('partial delivery, 503 retry, per-phone ordering, and Worker restart resume',async()=>{
 const h=await setup();h.subscribe('https://push.test/phone2');const original=global.fetch,calls=[];
 global.fetch=async(endpoint)=>{calls.push(endpoint);return new Response('',{status:endpoint.endsWith('phone1')?201:503});};
 try{
  await h.delivery.queueAlarmEvent(h.env,{deviceId:h.deviceId,eventId:'active',alarmType:'FAULT_130',notification:h.notification});
  await h.delivery.queueAlarmEvent(h.env,{deviceId:h.deviceId,eventId:'resolved',alarmType:'FAULT_130',notification:{...h.notification,data:{...h.notification.data,state:'resolved'}}});
  await h.delivery.drainAlarmDeliveries(h.env);
  assert.equal(calls.length,2);assert.equal(h.sql.prepare("SELECT COUNT(*) n FROM alarm_deliveries WHERE status='pending'").get().n,3);
  calls.length=0;await h.delivery.drainAlarmDeliveries(h.env);
  assert.deepEqual(calls,['https://push.test/phone1']); // phone2 RESOLVED is blocked behind ACTIVE
  h.sql.exec("UPDATE alarm_deliveries SET next_attempt_at=0,lease_until=0");
  global.fetch=async endpoint=>{calls.push(endpoint);return new Response('',{status:201});};
  await h.delivery.drainAlarmDeliveries(h.env);await h.delivery.drainAlarmDeliveries(h.env);
  assert.equal(h.sql.prepare("SELECT COUNT(*) n FROM alarm_deliveries WHERE status='pending'").get().n,0);
 }finally{global.fetch=original;}
});
test('concurrent Workers claim each recipient once',async()=>{
 const h=await setup(),original=global.fetch;let calls=0;
 global.fetch=async()=>{calls++;await new Promise(r=>setTimeout(r,10));return new Response('',{status:201});};
 try{
  await h.delivery.queueAlarmEvent(h.env,{deviceId:h.deviceId,eventId:'one',alarmType:'FAULT_130',notification:h.notification});
  assert.equal(h.sql.prepare('SELECT COUNT(*) n FROM alarm_events').get().n,1);
  await Promise.all([h.delivery.drainAlarmDeliveries(h.env),h.delivery.drainAlarmDeliveries(h.env)]);
  assert.equal(calls,1);
 }finally{global.fetch=original;}
});
test('404/410 subscriptions are removed; revoked owners do not receive pending work',async()=>{
 const h=await setup(),original=global.fetch;
 global.fetch=async()=>new Response('',{status:410});
 try{
  await h.delivery.queueAlarmEvent(h.env,{deviceId:h.deviceId,eventId:'gone',alarmType:'FAULT_130',notification:h.notification});
  await h.delivery.drainAlarmDeliveries(h.env);
  assert.equal(h.sql.prepare('SELECT COUNT(*) n FROM push_subscriptions').get().n,0);
  h.subscribe('https://push.test/phone1');
  await h.delivery.queueAlarmEvent(h.env,{deviceId:h.deviceId,eventId:'revoked',alarmType:'FAULT_130',notification:h.notification});
  h.sql.exec('DELETE FROM user_devices');global.fetch=async()=>{throw Error('must not send');};
  await h.delivery.drainAlarmDeliveries(h.env);
  assert.equal(h.sql.prepare("SELECT status FROM alarm_deliveries ORDER BY event_seq DESC LIMIT 1").get().status,'gone');
 }finally{global.fetch=original;}
});
test('push requests have bounded timeout, critical urgency and Retry-After',async()=>{
 const h=await setup(),original=global.fetch;let request;
 global.fetch=async(_,options)=>{request=options;return new Response('',{status:429,headers:{'Retry-After':'60'}});};
 try{
  await h.delivery.queueAlarmEvent(h.env,{deviceId:h.deviceId,eventId:'rate',alarmType:'FAULT_130',notification:h.notification});
  await h.delivery.drainAlarmDeliveries(h.env);
  const headers=new Headers(request.headers);assert.equal(headers.get('Urgency'),'high');assert.ok(request.signal);
  const row=h.sql.prepare('SELECT * FROM alarm_deliveries').get();assert.equal(row.status,'pending');
  assert.ok(row.next_attempt_at>Date.now()+55000);
 }finally{global.fetch=original;}
});
test('expired jobs stop retrying while durable receipts remain for deduplication',async()=>{
 const h=await setup();
 await h.delivery.queueAlarmEvent(h.env,{deviceId:h.deviceId,eventId:'expired',alarmType:'FAULT_130',notification:h.notification,now:Date.now()-3600001});
 await h.delivery.maintainAlarmDeliveries(h.env);
 assert.equal(h.sql.prepare('SELECT status FROM alarm_deliveries').get().status,'expired');
 assert.ok(await h.delivery.findAlarmEvent(h.env,h.deviceId,'expired'));
});
test('ACTIVE and RESOLVED notifications use separate tags',async()=>{
 const handlers={},notifications=[];const self={location:{href:'https://web.test/sw.js'},addEventListener(k,v){handlers[k]=v;},registration:{showNotification(t,o){notifications.push(o);return Promise.resolve();}}};
 vm.runInNewContext(fs.readFileSync('sw.js','utf8'),{self,URL,console});
 for(const state of ['active','resolved'])handlers.push({data:{json:()=>({data:{deviceId:'MAP-test',alarmType:'FAULT_130',state}})},waitUntil(){}});
 assert.notEqual(notifications[0].tag,notifications[1].tag);assert.equal(notifications[0].renotify,true);
});

test('expired delivery lease is reclaimed after an interrupted Worker',async()=>{
 const h=await setup(),original=global.fetch;let calls=0;
 global.fetch=async()=>{calls++;return new Response('',{status:201});};
 try{
  await h.delivery.queueAlarmEvent(h.env,{deviceId:h.deviceId,eventId:'lease',alarmType:'FAULT_130',notification:h.notification});
  h.sql.prepare("UPDATE alarm_deliveries SET lease_token='terminated-worker',lease_until=?").run(Date.now()+15000);
  await h.delivery.drainAlarmDeliveries(h.env);assert.equal(calls,0);
  h.sql.exec('UPDATE alarm_deliveries SET lease_until=0');
  await h.delivery.drainAlarmDeliveries(h.env);assert.equal(calls,1);
  assert.equal(h.sql.prepare('SELECT status FROM alarm_deliveries').get().status,'sent');
 }finally{global.fetch=original;}
});
test('a hung Push endpoint aborts and remains durably pending',async()=>{
 const h=await setup(),original=global.fetch;
 global.fetch=async(_,options)=>new Promise((resolve,reject)=>{
  const fallback=setTimeout(()=>reject(Error('deadline missing')),6000);
  options.signal.addEventListener('abort',()=>{clearTimeout(fallback);reject(options.signal.reason);},{once:true});
 });
 try{
  await h.delivery.queueAlarmEvent(h.env,{deviceId:h.deviceId,eventId:'timeout',alarmType:'FAULT_130',notification:h.notification});
  const started=Date.now();await h.delivery.drainAlarmDeliveries(h.env);
  assert.ok(Date.now()-started<5500);
  const row=h.sql.prepare('SELECT status,attempts,lease_until FROM alarm_deliveries').get();
  assert.equal(row.status,'pending');assert.equal(row.attempts,1);assert.equal(row.lease_until,0);
 }finally{global.fetch=original;}
});

test('batch endpoint: one auth, per-event durable receipts, idempotent resend, ordering after a failure',async()=>{
 const h=await setup(),original=global.fetch;global.fetch=async()=>new Response('',{status:201});
 const ev=(id,type,state='active',message='m')=>({event_id:id,alarm_type:type,state,message,severity:'warning'});
 try{
  const ok=await h.batch([ev('b-1','LIGHT_ON_DURING_BATCH'),ev('b-2','FAULT_130','active','off'),ev('b-3','FAULT_130','resolved','on')]);
  assert.equal(ok.status,200);const body=await ok.json();
  assert.deepEqual(body.results.map(r=>[r.event_id,r.durable,r.status]),[['b-1',true,200],['b-2',true,200],['b-3',true,200]]);
  assert.equal(h.sql.prepare('SELECT COUNT(*) n FROM alarm_events').get().n,3);
  // Response lost on the device: the identical batch is a duplicate, not a second write.
  const again=await (await h.batch([ev('b-1','LIGHT_ON_DURING_BATCH'),ev('b-2','FAULT_130','active','off')])).json();
  assert.deepEqual(again.results.map(r=>[r.durable,r.duplicate]),[[true,true],[true,true]]);
  assert.equal(h.sql.prepare('SELECT COUNT(*) n FROM alarm_events').get().n,3);
  // Conflict on one event does not block an unrelated alarm type, but later events of the same type are skipped.
  const mixed=await (await h.batch([ev('b-2','FAULT_130','resolved','DIFFERENT'),ev('b-9','FAULT_130','resolved','on'),ev('b-4','OTHER')])).json();
  assert.deepEqual(mixed.results.map(r=>[r.event_id,r.durable,r.status]),[['b-2',false,409],['b-9',false,409],['b-4',true,200]]);
  assert.equal(mixed.results[1].error,'SKIPPED_AFTER_FAILURE');
  await Promise.all(h.jobs);
 }finally{global.fetch=original;}
});
test('batch endpoint: bad key, empty, oversized and malformed events',async()=>{
 const h=await setup();
 const ev=(id)=>({event_id:id,alarm_type:'X',state:'active',message:'m'});
 assert.equal((await h.batch([ev('c-1')],'wrong-key')).status,401);
 assert.equal((await h.batch([])).status,400);
 assert.equal((await h.batch(Array.from({length:9},(_,i)=>ev('d-'+i)))).status,400);
 const body=await (await h.batch([{event_id:'e-1',alarm_type:'X'},{alarm_type:'X',message:'m'},ev('e-3')])).json();
 assert.deepEqual(body.results.map(r=>[r.durable,r.status]),[[false,400],[false,400],[true,200]]);
 assert.equal(h.sql.prepare('SELECT COUNT(*) n FROM alarm_events').get().n,1);
});

test('MQTT uplink ingest: signed broker calls write alarms/heartbeats exactly like the HTTPS endpoints',async()=>{
 const crypto=require('node:crypto'),h=await setup(),original=global.fetch;global.fetch=async()=>new Response('',{status:201});
 h.env.MQTT_DEVICE_SECRET='uplink-secret';
 const call=async(message,{secret='uplink-secret',ts=Date.now(),tamper=false}={})=>{
  const body=JSON.stringify(message);
  const sig=crypto.createHmac('sha256',secret).update(`mayap-uplink-ingest:v1\n${ts}\n${tamper?body+' ':body}`).digest('hex');
  const worker=(await import('../cloudflare/src/index.js')).default;
  return worker.fetch(new Request('https://worker.test/api/internal/uplink',{method:'POST',headers:{'x-mayap-ts':String(ts),'x-mayap-sig':sig},body}),h.env,{waitUntil(p){h.jobs.push(p);}});
 };
 const event=(id,extra={})=>({device_id:h.deviceId,kind:'alarm',data:{event_id:id,alarm_type:'FAULT_130',state:'active',message:'off',severity:'critical',...extra}});
 try{
  const first=await (await call(event('u-1'))).json();assert.deepEqual([first.success,first.durable,first.status],[true,true,200]);
  assert.equal(h.sql.prepare('SELECT COUNT(*) n FROM alarm_events').get().n,1);
  const again=await (await call(event('u-1'))).json();assert.equal(again.durable,true);       // response-loss resend
  assert.equal(h.sql.prepare('SELECT COUNT(*) n FROM alarm_events').get().n,1);
  const conflict=await (await call(event('u-1',{message:'different'}))).json();assert.equal(conflict.durable,false);
  assert.equal((await call(event('u-2',{state:'active'}))).status,200);                       // cooldown: unchanged state is not durable
  const throttled=await (await call(event('u-3'))).json();assert.equal(throttled.durable,false);
  const bad=await (await call({device_id:h.deviceId,kind:'alarm',data:{event_id:'u-4',alarm_type:'X'}})).json();assert.equal(bad.durable,false);
  const hb=await (await call({device_id:h.deviceId,kind:'heartbeat',data:{batch_running:true}})).json();assert.equal(hb.durable,true);
  const row=h.sql.prepare('SELECT last_seen,batch_running,status FROM devices WHERE device_id=?').get(h.deviceId);
  assert.equal(row.batch_running,1);assert.equal(row.status,'online');assert.ok(row.last_seen>0);
  const unknown=await (await call({device_id:'MAP-AAAAAAAAAAAA',kind:'heartbeat',data:{}})).json();assert.equal(unknown.durable,false);
  // Authentication: wrong secret, tampered body, stale or missing timestamp, unknown kind.
  assert.equal((await call(event('u-5'),{secret:'other'})).status,401);
  assert.equal((await call(event('u-5'),{tamper:true})).status,401);
  assert.equal((await call(event('u-5'),{ts:Date.now()-10*60*1000})).status,401);
  assert.equal((await call({device_id:h.deviceId,kind:'nope',data:{}})).status,400);
  h.env.MQTT_DEVICE_SECRET='';
  assert.equal((await call(event('u-6'))).status,401);                                       // fails closed without the secret
  await Promise.all(h.jobs);
 }finally{global.fetch=original;}
});

// ---- End-to-end simulation of plan B: firmware-shaped MQTT publish -> real broker DO -> real Worker ingest -> D1 ----
const NativeResponse = globalThis.Response;     // captured before the broker harness swaps in its 101-tolerant stub
test('uplink end to end: device alarm/heartbeat over MQTT is PUBACKed only once it is durable in D1',async()=>{
 const crypto=require('node:crypto'),wire=require('./fixtures/mqtt-wire.cjs'),harness=require('./fixtures/mqtt-broker-harness.cjs');
 const h=await setup(),originalFetch=global.fetch;global.fetch=async()=>new NativeResponse('',{status:201});
 const secret='uplink-secret';h.env.MQTT_DEVICE_SECRET=secret;
 const worker=(await import('../cloudflare/src/index.js')).default,ctx={waitUntil(p){h.jobs.push(p);}};
 const ingestCalls=[];
 const env={BROKER_DEVICE_SECRET:secret,BROKER_WEB_TOKEN_SECRET:'web-secret',CLOUD_INGEST:{async fetch(url,init){
  ingestCalls.push(JSON.parse(init.body).kind);
  const stub=globalThis.Response;globalThis.Response=NativeResponse;     // the Worker answers with real Responses
  try{return await worker.fetch(new Request(url,init),h.env,ctx);}finally{globalThis.Response=stub;}
 }}};
 const id=h.deviceId,password=crypto.createHmac('sha256',secret).update(`mayap-mqtt-device:v1\n${id}`).digest('hex');
 try{
  const b=await harness.makeBroker({env});
  const {client,server}=await harness.openWebSocket(b.broker,id);
  await harness.feed(b.broker,server,wire.connect({clientId:`esp-${id}`,username:id,password,will:{topic:`mayap/v1/${id}/presence`,qos:1,retain:true,payload:'{"online":false}'}}));
  assert.equal(wire.parseConnack((await harness.clientReceive(client))[0]).returnCode,0);
  // Exactly the JSON the firmware builds in sendAlarmsUplink()/sendHeartbeat().
  const event=(eventId,state,message)=>JSON.stringify({event_id:eventId,alarm_type:'FAULT_130',severity:'critical',state,message,temperature:29.9,humidity:63.7,detected_uptime_ms:129825});
  const publish=async(packetId,topic,payload)=>{await harness.feed(b.broker,server,wire.publish({topic:`mayap/v1/${id}/${topic}`,qos:1,packetId,payload}));
   return (await harness.clientReceive(client)).map(f=>wire.parsePuback(f).packetId);};
  const count=()=>h.sql.prepare('SELECT COUNT(*) n FROM alarm_events').get().n;
  assert.deepEqual(await publish(11,'alarm',event('fe6e-00000001','active','off')),[11]);         // durable -> PUBACK
  assert.equal(count(),1);
  assert.deepEqual(await publish(11,'alarm',event('fe6e-00000001','active','off')),[11]);         // response lost, device resends: still one row
  assert.equal(count(),1);
  assert.deepEqual(await publish(12,'alarm',event('fe6e-00000002','resolved','on')),[12]);        // recovery stored after the alarm
  assert.equal(count(),2);
  assert.deepEqual(await publish(13,'alarm',event('fe6e-00000001','active','DIFFERENT')),[]);     // conflicting id: not durable -> no PUBACK
  assert.equal(count(),2);
  assert.deepEqual(await publish(14,'heartbeat',JSON.stringify({batch_running:true})),[14]);
  const row=h.sql.prepare('SELECT batch_running,status,last_seen FROM devices WHERE device_id=?').get(id);
  assert.equal(row.batch_running,1);assert.equal(row.status,'online');assert.ok(row.last_seen>0);
  assert.deepEqual(ingestCalls,['alarm','alarm','alarm','alarm','heartbeat']);
  // Wrong secret on the Worker side (misconfigured deploy): nothing is stored and the device gets no PUBACK.
  h.env.MQTT_DEVICE_SECRET='rotated-elsewhere';
  assert.deepEqual(await publish(15,'alarm',event('fe6e-00000003','active','off')),[]);
  assert.equal(count(),2);
  await Promise.all(h.jobs);
 }finally{global.fetch=originalFetch;}
});
