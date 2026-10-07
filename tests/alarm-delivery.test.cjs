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
