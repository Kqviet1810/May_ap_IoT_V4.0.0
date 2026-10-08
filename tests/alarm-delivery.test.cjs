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
  // Broker queue form: per-event verdicts. Cooldown = `throttled` with the remaining wait (not a failure), a reused id with other
  // content = `rejected`, a replay = `duplicate`, an unknown device = `rejected` for every event (permanent, not an outage).
  const batch=async(events,device=h.deviceId)=>(await call({device_id:device,kind:'alarms',events})).json();
  const mk=(id,extra={})=>({event_id:id,alarm_type:'FAULT_140',state:'active',message:'m',severity:'critical',...extra});
  const q1=await batch([mk('q-1')]);assert.deepEqual(q1.results.map(r=>r.outcome),['stored']);
  const q2=await batch([mk('q-1'),mk('q-2'),mk('q-3',{alarm_type:'FAULT_141'})]);
  assert.deepEqual(q2.results.map(r=>r.outcome),['duplicate','throttled','stored']);
  assert.ok(q2.results[1].retry_after_ms>=1000&&q2.results[1].retry_after_ms<=15000);
  const q3=await batch([mk('q-1',{message:'other'}),{event_id:'q-5',alarm_type:'X'}]);
  assert.deepEqual(q3.results.map(r=>[r.outcome,r.error]),[['rejected','EVENT_ID_CONFLICT'],['rejected','BAD_EVENT']]);
  const q4=await batch([mk('q-6',{state:'resolved'}),mk('q-7',{state:'active'})]);          // order inside one batch is kept
  assert.deepEqual(q4.results.map(r=>r.outcome),['stored','stored']);
  assert.deepEqual((await batch([mk('q-8')],'MAP-AAAAAAAAAAAA')).results.map(r=>[r.outcome,r.error]),[['rejected','DEVICE_NOT_REGISTERED']]);
  assert.equal((await call({device_id:h.deviceId,kind:'alarms',events:[]})).status,400);
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

// ---- End-to-end: firmware-shaped MQTT publish -> real broker DO (durable queue) -> real Worker ingest -> D1 ----
// Contract: a PUBACK on `alarm` means BROKER_STORED (a durable row in the Durable Object). D1_STORED follows when the DO alarm
// drains the queue into the Worker. The two are different facts and this test keeps them apart.
const NativeResponse = globalThis.Response;     // captured before the broker harness swaps in its 101-tolerant stub
test('uplink end to end: PUBACK = BROKER_STORED, D1_STORED follows from the queue, retries are idempotent',async()=>{
 const crypto=require('node:crypto'),wire=require('./fixtures/mqtt-wire.cjs'),harness=require('./fixtures/mqtt-broker-harness.cjs');
 const h=await setup(),originalFetch=global.fetch;global.fetch=async()=>new NativeResponse('',{status:201});
 const secret='uplink-secret';h.env.MQTT_DEVICE_SECRET=secret;
 const worker=(await import('../cloudflare/src/index.js')).default,ctx={waitUntil(p){h.jobs.push(p);}};
 const ingestCalls=[];let workerDown=false;
 const env={BROKER_DEVICE_SECRET:secret,BROKER_WEB_TOKEN_SECRET:'web-secret',CLOUD_INGEST:{async fetch(url,init){
  const parsed=JSON.parse(init.body);ingestCalls.push(parsed.kind+(parsed.events?':'+parsed.events.map(e=>e.event_id).join(','):''));
  if(workerDown) throw new Error('worker unreachable');
  const stub=globalThis.Response;globalThis.Response=NativeResponse;     // the Worker answers with real Responses
  try{return await worker.fetch(new Request(url,init),h.env,ctx);}finally{globalThis.Response=stub;}
 }}};
 const id=h.deviceId,password=crypto.createHmac('sha256',secret).update(`mayap-mqtt-device:v1\n${id}`).digest('hex');
 try{
  const b=await harness.makeBroker({env});
  let clock=Date.now();b.broker._now=()=>clock;
  const {client,server}=await harness.openWebSocket(b.broker,id);
  await harness.feed(b.broker,server,wire.connect({clientId:`esp-${id}`,username:id,password,will:{topic:`mayap/v1/${id}/presence`,qos:1,retain:true,payload:'{"online":false}'}}));
  assert.equal(wire.parseConnack((await harness.clientReceive(client))[0]).returnCode,0);
  // Exactly the JSON the firmware builds in sendAlarmsUplink()/sendHeartbeat().
  const event=(eventId,state,message)=>JSON.stringify({event_id:eventId,alarm_type:'FAULT_130',severity:'critical',state,message,temperature:29.9,humidity:63.7,detected_uptime_ms:129825});
  const publish=async(packetId,topic,payload)=>{await harness.feed(b.broker,server,wire.publish({topic:`mayap/v1/${id}/${topic}`,qos:1,packetId,payload}));
   return (await harness.clientReceive(client)).map(f=>wire.parsePuback(f).packetId);};
  const count=()=>h.sql.prepare('SELECT COUNT(*) n FROM alarm_events').get().n;
  const drain=async(advanceMs=0)=>{clock+=advanceMs;await b.broker.alarm();};
  // BROKER_STORED: PUBACK at once, nothing in D1 yet, and the PUBACK never waited for the Worker.
  assert.deepEqual(await publish(11,'alarm',event('fe6e-00000001','active','off')),[11]);
  assert.equal(count(),0);assert.deepEqual(ingestCalls,[]);
  assert.deepEqual(await publish(11,'alarm',event('fe6e-00000001','active','off')),[11]);         // device resends (link blip): acknowledged, queued once
  assert.deepEqual(await publish(12,'alarm',event('fe6e-00000002','resolved','on')),[12]);        // recovery behind the alarm
  await drain();
  assert.equal(count(),2);                                                                        // D1_STORED, in order
  assert.deepEqual(h.sql.prepare('SELECT event_id FROM alarm_events ORDER BY order_seq').all().map(r=>r.event_id),['fe6e-00000001','fe6e-00000002']);
  assert.deepEqual(ingestCalls,['alarms:fe6e-00000001,fe6e-00000002']);                           // one batched request
  // A conflicting event id is stored by the broker (PUBACK) but the Worker rejects it permanently: visible, never retried.
  assert.deepEqual(await publish(13,'alarm',event('fe6e-00000001','active','DIFFERENT')),[13]);
  await drain();
  assert.equal(count(),2);
  const afterConflict=await b.broker.uplink.status();
  assert.equal(afterConflict.rejected,1);assert.equal(afterConflict.pending,0);
  assert.equal(afterConflict.recent_rejected[0].err,'EVENT_ID_CONFLICT');
  assert.deepEqual(await publish(14,'heartbeat',JSON.stringify({batch_running:true})),[14]);
  await drain();
  const row=h.sql.prepare('SELECT batch_running,status,last_seen FROM devices WHERE device_id=?').get(id);
  assert.equal(row.batch_running,1);assert.equal(row.status,'online');assert.ok(row.last_seen>0);
  // HTTPS fallback carrying an alarm MQTT already stored: same event_id -> duplicate, still one row.
  const https=async(events)=>{const stub=globalThis.Response;globalThis.Response=NativeResponse;try{return await (await h.batch(events)).json();}finally{globalThis.Response=stub;}};
  const viaHttps=await https([{event_id:'fe6e-00000001',alarm_type:'FAULT_130',severity:'critical',state:'active',message:'off'}]);
  assert.deepEqual([viaHttps.results[0].durable,viaHttps.results[0].duplicate],[true,true]);
  assert.equal(count(),2);
  // HTTPS stored it first, then the device's MQTT resend arrives: acknowledged (BROKER_STORED) and forwarded as a duplicate.
  const fresh=await https([{event_id:'fe6e-00000009',alarm_type:'FAULT_131',severity:'critical',state:'active',message:'x'}]);
  assert.equal(fresh.results[0].durable,true);assert.equal(count(),3);
  assert.deepEqual(await publish(16,'alarm',JSON.stringify({event_id:'fe6e-00000009',alarm_type:'FAULT_131',severity:'critical',state:'active',message:'x'})),[16]);
  await drain();assert.equal(count(),3);
  // Worker unreachable (or misconfigured): the device still gets its PUBACK, the event waits in the broker, and the
  // backlog is observable. When the Worker recovers the queue drains by itself - nothing was lost, nothing duplicated.
  workerDown=true;
  assert.deepEqual(await publish(20,'alarm',event('fe6e-00000020','active','off')),[20]);
  assert.deepEqual(await publish(21,'alarm',JSON.stringify({event_id:'fe6e-00000021',alarm_type:'FAULT_131',severity:'critical',state:'resolved',message:'ok'})),[21]);
  await drain(); await drain(3000); await drain(6000);
  assert.equal(count(),3);
  const stuck=await b.broker.uplink.status();
  assert.equal(stuck.pending,2);assert.equal(stuck.retrying,2);assert.match(stuck.last_error,/FETCH_ERROR/);assert.ok(stuck.oldest_age_ms>=0);
  workerDown=false;
  await drain(10000); await drain(60000);
  assert.equal(count(),5);
  const healed=await b.broker.uplink.status();
  assert.equal(healed.pending,0);assert.equal(healed.last_error,'');
  await Promise.all(h.jobs);
 }finally{global.fetch=originalFetch;}
});

test('stored is not pushed: receipt says durable, status shows push progress, age and channel are kept',async()=>{
 const crypto=require('node:crypto'),h=await setup(),original=global.fetch,logs=[],log=console.log,stubResponse=globalThis.Response;
 globalThis.Response=NativeResponse;     // the Worker answers with real Responses (an earlier harness swapped in a stub)
 console.log=(...a)=>{logs.push(a.join(' '));};
 global.fetch=async()=>new Response('',{status:503});                      // the push service is down
 try{
  const worker0=(await import('../cloudflare/src/index.js')).default;
  const resp=await worker0.fetch(new Request('https://worker.test/api/device/alarm',{method:'POST',body:JSON.stringify({device_id:h.deviceId,device_key:'test-device-key',
   event_id:'age-1',alarm_type:'FAULT_130',state:'active',message:'off',severity:'critical',age_ms:4200,detected_uptime_ms:1000})}),h.env,{waitUntil(p){h.jobs.push(p);}});
  const res=await resp.json();
  assert.deepEqual([res.success,res.durable,res.stored],[true,true,true]);   // stored in D1 ...
  await new Promise(r=>setTimeout(r,150));                                    // the Worker's immediate push attempt (503) has run
  let snap=await h.delivery.alarmStatusSnapshot(h.env,h.deviceId);
  assert.equal(snap.events.length,1);
  assert.deepEqual(snap.events[0].push,{recipients:1,sent:0,pending:1,gone:0,expired:0});   // ... but nothing was pushed yet
  assert.equal(snap.events[0].device_age_ms,4200);assert.equal(snap.events[0].via,'https');
  assert.equal(snap.events[0].state,'active');assert.equal(snap.events[0].severity,'critical');
  assert.deepEqual(snap.alarms.map(a=>a.alarm_type),['FAULT_130']);
  assert.ok(snap.server_time>0);
  assert.ok(logs.some(l=>/result=retry/.test(l)&&/device_age_ms=4200 via=https/.test(l)&&/since_received_ms=\d+/.test(l)),logs.join('\n'));
  // The push service recovers: the same event is now "sent" (accepted by the push service).
  global.fetch=async()=>new Response('',{status:201});h.sql.exec('UPDATE alarm_deliveries SET next_attempt_at=0,lease_until=0');
  await h.delivery.drainAlarmDeliveries(h.env);
  snap=await h.delivery.alarmStatusSnapshot(h.env,h.deviceId);
  assert.deepEqual(snap.events[0].push,{recipients:1,sent:1,pending:0,gone:0,expired:0});
  // MQTT channel: tagged "mqtt"; a recovery clears the active alarm from the snapshot.
  h.env.MQTT_DEVICE_SECRET='uplink-secret';
  const worker=(await import('../cloudflare/src/index.js')).default;
  const send=async data=>{const body=JSON.stringify({device_id:h.deviceId,kind:'alarm',data}),ts=Date.now();
   const sig=crypto.createHmac('sha256','uplink-secret').update(`mayap-uplink-ingest:v1\n${ts}\n${body}`).digest('hex');
   return (await worker.fetch(new Request('https://worker.test/api/internal/uplink',{method:'POST',headers:{'x-mayap-ts':String(ts),'x-mayap-sig':sig},body}),h.env,{waitUntil(p){h.jobs.push(p);}})).json();};
  h.sql.exec('UPDATE alarm_state SET last_sent_at=0');                         // past the cooldown
  assert.equal((await send({event_id:'age-2',alarm_type:'FAULT_130',state:'resolved',message:'on',severity:'critical',age_ms:90})).durable,true);
  snap=await h.delivery.alarmStatusSnapshot(h.env,h.deviceId);
  assert.equal(snap.events[0].event_id,'age-2');assert.equal(snap.events[0].via,'mqtt');assert.equal(snap.events[0].device_age_ms,90);
  assert.deepEqual(snap.alarms,[]);
  // Negative / absurd ages are ignored rather than trusted.
  h.sql.exec('UPDATE alarm_state SET last_sent_at=0');
  assert.equal((await send({event_id:'age-3',alarm_type:'FAULT_130',state:'active',message:'off',severity:'critical',age_ms:-5})).durable,true);
  snap=await h.delivery.alarmStatusSnapshot(h.env,h.deviceId);assert.equal(snap.events[0].device_age_ms,null);
  await Promise.all(h.jobs);
 }finally{global.fetch=original;console.log=log;globalThis.Response=stubResponse;}
});
test('D1 fault: a failed write never produces a durable receipt on any channel',async()=>{
 const crypto=require('node:crypto'),h=await setup(),stubResponse=globalThis.Response;h.env.MQTT_DEVICE_SECRET='uplink-secret';globalThis.Response=NativeResponse;
 h.env.DB.batch=async()=>{throw Error('injected D1 outage');};
 const worker=(await import('../cloudflare/src/index.js')).default;
 const body=JSON.stringify({device_id:h.deviceId,kind:'alarm',data:{event_id:'d1-1',alarm_type:'FAULT_130',state:'active',message:'off',severity:'critical'}}),ts=Date.now();
 const sig=crypto.createHmac('sha256','uplink-secret').update(`mayap-uplink-ingest:v1\n${ts}\n${body}`).digest('hex');
 let durable=null,status=0;
 try{const r=await worker.fetch(new Request('https://worker.test/api/internal/uplink',{method:'POST',headers:{'x-mayap-ts':String(ts),'x-mayap-sig':sig},body}),h.env,{waitUntil(){}});
  status=r.status;durable=(await r.json()).durable;}catch{status=500;}
 assert.ok(status>=500||durable===false);assert.notEqual(durable,true);              // broker would not PUBACK -> device keeps the event
 assert.equal((await h.alarm()).status,500);                                         // HTTPS fallback: 500, no receipt
 assert.equal(h.sql.prepare('SELECT COUNT(*) n FROM alarm_events').get().n,0);
});
