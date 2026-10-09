// Real browser cross-origin fetch + DOM; isolated GIS/API fixtures, no Google/customer I/O.
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const {chromium}=require(process.env.MAYAP_PLAYWRIGHT || 'playwright');
async function openTab(page,name){const tab=page.locator(`[data-page="${name}"]`);if(!(await tab.isVisible()))await page.locator('#navToggle').click();await tab.click();}
const out=path.resolve(process.argv[2] || '../../outputs/account-browser');fs.mkdirSync(out,{recursive:true});
const home='http://127.0.0.1:8765',api='https://mayap-push-worker.vietk-mayaptrung.workers.dev';
const token='ab'.repeat(32);
const gis=`window.google={accounts:{id:{initialize(options){window.gisOptions=options;},renderButton(root,options){
 const button=document.createElement('button');button.textContent='Đăng nhập với Google';button.style.cssText='border:1px solid #dadce0;border-radius:24px;background:white;height:44px;width:'+options.width+'px';
 button.onclick=()=>window.gisOptions.callback({credential:'isolated-google-id-token'});root.append(button);}}}};`;
async function fixture(context,{gate=Promise.resolve(),guest=false,picture='https://lh3.googleusercontent.com/avatar'}={}){
  let loggedOut=false;
  await context.route('https://accounts.google.com/gsi/client',r=>r.fulfill({contentType:'application/javascript',body:gis}));
  await context.route('https://lh3.googleusercontent.com/**',r=>r.fulfill({contentType:'image/svg+xml',body:'<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64"><rect width="64" height="64" fill="#087d85"/><circle cx="32" cy="24" r="12" fill="#fff"/><path d="M10 64v-9a22 22 0 0 1 44 0v9" fill="#fff"/></svg>'}));
  await context.route(api+'/api/**',async route=>{
    const request=route.request(),pathname=new URL(request.url()).pathname;
    const headers={'Access-Control-Allow-Origin':home,'Access-Control-Allow-Headers':'authorization,content-type','Access-Control-Allow-Methods':'GET,POST,DELETE'};
    if(request.method()==='OPTIONS')return route.fulfill({status:204,headers});
    let status=200,data={success:true};
    if(pathname==='/api/account/google/challenge')data={success:true,challenge:'cd'.repeat(32),nonce:'server-nonce',clientId:'public-client'};
    else if(pathname==='/api/account/google/login'){
      assert.equal(request.postDataJSON().challenge,'cd'.repeat(32));assert.equal(request.postDataJSON().credential,'isolated-google-id-token');
      data={success:true,token,expiresAt:Date.now()+86400000};guest=false;picture='https://lh3.googleusercontent.com/avatar';
    }else if(pathname==='/api/account/session'){
      await gate;assert.equal(request.headers().authorization,'Bearer '+token);
      if(loggedOut || guest){status=401;data={success:false};}
      else data={success:true,user:{sub:'returning-sub',name:'Khách quay lại',email:'user@example.test',picture},devices:[],expiresAt:Date.now()+86400000};
    }else if(pathname==='/api/account/logout'){
      assert.equal(request.headers().authorization,'Bearer '+token);loggedOut=true;
    }
    return route.fulfill({status,headers,contentType:'application/json',body:JSON.stringify(data)});
  });
}
async function main(){
  const browser=await chromium.launch({executablePath:process.env.MAYAP_CHROME || 'C:/Program Files/Google/Chrome/Application/chrome.exe',headless:true});
  const results=[];
  try {
    for(const width of [320,360,390,768,1440]){
      const context=await browser.newContext({viewport:{width,height:900},serviceWorkers:'block'});await fixture(context,{guest:true});
      const page=await context.newPage(),errors=[];page.on('pageerror',e=>errors.push(e.message));
      await page.goto(home,{waitUntil:'networkidle'});
      assert.equal(await page.locator('.appShell').isVisible(),false);assert.equal(await page.locator('.landing').isVisible(),true);
      assert.equal(await page.locator('.landing .googleLogin button').count(),2);
      assert.equal(await page.evaluate(()=>window.gisOptions.nonce),'server-nonce');
      assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),true);
      await page.screenshot({path:path.join(out,`landing-${width}.png`),fullPage:true});
      // Durable token restores server-verified identity even after all tabs close.
      await page.locator('.googleLogin button').first().click();
      await page.waitForFunction(()=>document.documentElement.dataset.auth==='ready');
      assert.equal(await page.evaluate(()=>sessionStorage.getItem('mayap.account.session.v1')),null);
      assert.equal(await page.evaluate(()=>localStorage.getItem('mayap.account.session.v1')),token);
      await page.close();
      const reopened=await context.newPage();await reopened.goto(home,{waitUntil:'networkidle'});
      await reopened.waitForFunction(()=>document.documentElement.dataset.auth==='ready');
      await reopened.evaluate(()=>{
        document.getElementById('liveTemp').textContent='32,4°C';
        document.getElementById('liveHumidity').textContent='80%';
        document.getElementById('liveState').textContent='Đang ấp';
      });
      assert.equal(await reopened.locator(width<=800?'.mobileIdentity [data-account-name]':'.brand [data-account-name]').textContent(),'Khách quay lại');
      if(width<=800)assert.equal(await reopened.locator('.sidebar > .brand').isVisible(),false);
      if(width<=800){
        const profile=await reopened.locator('.mobileIdentity').boundingBox();
        const readings=await reopened.locator('.headerReadings').boundingBox();
        assert.ok(profile.x+profile.width<=readings.x);
        assert.ok(profile.y<readings.y+readings.height && readings.y<profile.y+profile.height);
        const fonts=await reopened.locator('.live>strong').evaluateAll(nodes=>nodes.map(node=>parseFloat(getComputedStyle(node).fontSize)));
        assert.ok(fonts[0]>=(width<=360?14:17) && fonts[1]>=(width<=360?15:17) && fonts[2]>=(width<=360?12:13));
        assert.equal(await reopened.locator('.live').evaluateAll(nodes=>nodes.every(node=>node.scrollWidth<=node.clientWidth)),true);
      }
      assert.equal(await reopened.locator(width<=800?'.mobileIdentity img':'.brand img').evaluate(img=>img.complete && img.naturalWidth>0),true);
      assert.equal(await reopened.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),true);
      await openTab(reopened,'settings');
      assert.equal(await reopened.locator('#page-settings > :last-child').getAttribute('id'),'accountPanel');
      assert.equal(await reopened.locator('#revokeOtherSessions').count(),0);
      await reopened.screenshot({path:path.join(out,`account-settings-${width}.png`),fullPage:true});
      assert.deepEqual(errors,[]);results.push({width,guestHome:true,coldGoogleLogin:true,reopenKeepsLogin:true,identityVisible:true,noOverflow:true,errors});await context.close();
    }
    const photoContext=await browser.newContext({viewport:{width:320,height:844},serviceWorkers:'block'});
    await fixture(photoContext,{picture:''});
    await photoContext.addInitScript(token=>localStorage.setItem('mayap.account.session.v1',token),token);
    const photoPage=await photoContext.newPage();await photoPage.goto(home,{waitUntil:'networkidle'});
    await photoPage.locator('.mobileIdentity .avatarRefresh').click();
    await photoPage.locator('#profilePhotoDialog .googleLogin button').click();
    await photoPage.waitForFunction(()=>document.querySelector('.mobileIdentity img')?.naturalWidth>0);
    assert.equal(await photoPage.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),true);
    results.push({missingGooglePhotoVerification:true});await photoContext.close();
    const context=await browser.newContext({viewport:{width:390,height:844},serviceWorkers:'block'});
    let release;const gate=new Promise(r=>{release=r;});await fixture(context,{gate});
    await context.addInitScript(token=>{sessionStorage.setItem('mayap.account.session.v1',token);
      window.authStates=[];new MutationObserver(()=>window.authStates.push(document.documentElement?.dataset.auth))
        .observe(document,{subtree:true,attributes:true,attributeFilter:['data-auth']});},token);
    const page=await context.newPage();const returningErrors=[];page.on('pageerror',e=>returningErrors.push(e.message));
    await page.goto(home,{waitUntil:'domcontentloaded'});
    assert.equal(await page.locator('.landing').isVisible(),false);assert.equal(await page.locator('.appShell').isVisible(),false);
    release();
    try {await page.waitForFunction(()=>document.documentElement.dataset.auth==='ready');}
    catch(error){console.error({returningErrors,state:await page.evaluate(()=>({auth:document.documentElement.dataset.auth,
      token:sessionStorage.getItem('mayap.account.session.v1'),account:window.MayapAccount?.current,message:document.getElementById('authMessage').textContent}))});throw error;}
    assert.ok(!(await page.evaluate(()=>window.authStates)).includes('guest'));
    await openTab(page,'settings');await page.locator('#logoutBtn').click();
    // Fixture returns 401 even if context init script re-seeds on navigation.
    await page.waitForFunction(()=>document.documentElement.dataset.auth==='guest');
    assert.equal(await page.locator('.landing').isVisible(),true);assert.equal(await page.locator('.appShell').isVisible(),false);
    assert.equal(await page.evaluate(()=>sessionStorage.getItem('mayap.account.session.v1')),null);
    assert.equal(await page.evaluate(()=>localStorage.getItem('mayap.account.session.v1')),null);
    results.push({returningLoginNoLandingFlash:true,logoutReturnsHome:true,logoutClearsToken:true,crossOriginBearer:true});await context.close();
    fs.writeFileSync(path.join(out,'account-browser-qa.json'),JSON.stringify({passed:true,results},null,2));console.log(JSON.stringify(results));
  }finally{await browser.close();}
}
main().catch(e=>{console.error(e);process.exitCode=1;});
