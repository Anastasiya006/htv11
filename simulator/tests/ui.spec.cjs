const {test,expect}=require('@playwright/test');
const idle={protocol:2,session:null,connected:false,animation:'disconnected',screenshots:[],telemetry:{timeline:[],tools:0,errors:0}};
test('device inspector, menu, and demo response/image flow',async({page})=>{
 const errors=[];page.on('pageerror',error=>errors.push(error.message));
 await page.route('**/state',route=>route.fulfill({json:idle}));await page.goto('/');
 await expect(page.locator('#device-signal')).toHaveText('○ OFFLINE');
 const logical=await page.locator('#screen').evaluate(el=>({width:el.offsetWidth,height:el.offsetHeight}));expect(logical).toEqual({width:240,height:320});
 await page.locator('#device-status').click();await expect(page.locator('#inspect-body')).toContainText('No tool calls yet.');
 await page.locator('[data-panel="timeline"]').click();await expect(page.locator('#inspect-body')).toContainText('No events yet.');await page.locator('#inspect-back').click();
 await page.evaluate(()=>{
  socket={readyState:1,send:raw=>{window.testSent=JSON.parse(raw);}};
  terminal.reset();terminal.write('╭───────────────╮\r\nAllow execution?\r\n❯ 1. Allow once\r\n  2. Deny\r\n');
 });
 await expect(page.locator('#input-picker')).toBeVisible();await expect(page.locator('#picker-context')).not.toContainText('─');
 await page.locator('#picker-options button').nth(1).click();await page.locator('#picker-confirm').click();
 expect(await page.evaluate(()=>window.testSent.data)).toBe('\x1b[B\r');await page.evaluate(()=>{socket=null;});
 await page.locator('#replay-demo').click();await expect(page.locator('#demo-banner')).toBeVisible();
 await expect(page.locator('#response-preview')).toBeVisible({timeout:22000});
 await page.locator('#device-status').click();await expect(page.locator('#inspect-body')).toContainText('4');
 await page.locator('[data-panel="timeline"]').click();await expect(page.locator('.device-event')).toHaveCount(10);
 await page.locator('[data-panel="health"]').click();await expect(page.locator('#inspect-body')).toContainText('DEMO REPLAY');
 await page.locator('#inspect-back').click();
 await page.locator('#response-preview').click();await expect(page.locator('#response-body')).toContainText('Layout updated.');
 const pet=await page.locator('#response-view .reader-pet img').boundingBox();const view=await page.locator('#screen').boundingBox();
 expect(pet.y).toBeGreaterThanOrEqual(view.y);expect(pet.y+pet.height).toBeLessThanOrEqual(view.y+208);
 await page.locator('#reader-image').click();await expect(page.locator('#image-view')).toBeVisible();
 await expect.poll(()=>page.locator('#screenshot').evaluate(img=>img.naturalWidth)).toBeGreaterThan(0);
 await page.locator('#image-zoom').click();await expect(page.locator('#screenshot')).toHaveClass('zoomed');await page.locator('#image-back').click();
 await page.screenshot({path:'/tmp/gemmi-companion.png'});
 await page.locator('#replay-demo').click();await expect(page.locator('#demo-banner')).toBeHidden();await expect(page.locator('#image-card')).toBeHidden();expect(errors).toEqual([]);
});
test('new turn and disconnect clear response and viewer',async({page})=>{
 let state={...idle,connected:true,animation:'success',turnAt:10,eventId:'first',response:'A complete answer. '.repeat(20),responseAt:20};
 await page.addInitScript(()=>window.addEventListener('DOMContentLoaded',()=>window.dispatchEvent(new CustomEvent('gemmi-connection',{detail:{connected:true}}))));
 await page.route('**/state',route=>route.fulfill({json:state}));await page.goto('/');
 await expect(page.locator('#response-preview')).toBeVisible();await page.locator('#response-preview').click();
 state={...state,animation:'thinking',turnAt:30,eventId:'second',response:'',responseAt:0};
 await expect(page.locator('#response-preview')).toBeHidden();await expect(page.locator('#response-view')).toBeHidden();
 state={...state,connected:false,animation:'disconnected'};
 await expect(page.locator('.pet img')).toHaveAttribute('src','../animations/disconnected.gif');await expect(page.locator('#activity')).toHaveText('CLI disconnected.');
});
test('mobile layout fits and response uses text, not executable HTML',async({page})=>{
 await page.setViewportSize({width:390,height:844});
 await page.addInitScript(()=>window.addEventListener('DOMContentLoaded',()=>window.dispatchEvent(new CustomEvent('gemmi-connection',{detail:{connected:true}}))));
 await page.route('**/state',route=>route.fulfill({json:{...idle,connected:true,animation:'success',turnAt:1,eventId:'safe',response:'<img src=x onerror="alert(1)">'.repeat(10),responseAt:2}}));
 await page.goto('/');await page.locator('#response-preview').click();await expect(page.locator('#response-body')).toHaveText('<img src=x onerror="alert(1)">'.repeat(10));
 expect(await page.locator('#response-body img').count()).toBe(0);expect(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth)).toBe(true);
});

test('idle Gemmi sleeps, wakes on tap, and a new turn restores thinking',async({page})=>{
 let state={...idle,connected:true,animation:'idle',eventId:'idle',turnAt:1};
 await page.clock.install();
 await page.addInitScript(()=>window.addEventListener('DOMContentLoaded',()=>window.dispatchEvent(new CustomEvent('gemmi-connection',{detail:{connected:true}}))));
 await page.route('**/state',route=>route.fulfill({json:state}));await page.goto('/');
 await expect(page.locator('#device-signal')).toContainText('IDLE');
 await page.clock.fastForward(62000);
 await expect(page.locator('.pet img')).toHaveAttribute('src','../animations/sleeping.gif');
 await page.locator('.pet').click();await expect(page.locator('.pet img')).toHaveAttribute('src','../animations/waking-up.gif');
 state={...state,animation:'thinking',eventId:'new-turn',turnAt:2};
 await page.clock.fastForward(3000);
 await expect(page.locator('.pet img')).toHaveAttribute('src','../animations/thinking.gif');
});
