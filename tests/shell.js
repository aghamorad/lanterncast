// Run the shipped TV shell without a browser or network service.
const fs = require('fs'), vm = require('vm'), assert = require('assert');
const html=fs.readFileSync(__dirname+'/../index.html','utf8');
const script=html.match(/<script>\s*([\s\S]*?)<\/script>/)[1];
const events={},messages=[],timers=[],intervals=[];
let writes=0;
const nodes={state:{textContent:''},ui:{style:{}},report:{set textContent(v){this.value=v;writes++;}},
 nacl:{postMessage(m){messages.push(m);},addEventListener(n,f){events[n]=f;}}};
const docEvents={};
const document={readyState:'complete',hidden:false,getElementById(id){return nodes[id];},addEventListener(n,f){docEvents[n]=f;}};
const context={document,navigator:{userAgent:'fixture'},ArrayBuffer,
 webapis:{network:{getIp(){return '192.0.2.1';},getMac(){return '00:11:22:33:44:55';}}},
 setTimeout(f,ms){timers.push({f,ms});return timers.length;},setInterval(f,ms){intervals.push({f,ms});}};
context.window=context;context.innerWidth=1920;context.innerHeight=1080;
vm.runInNewContext(script,context);
timers.find(t=>t.ms===500).f();
assert(messages.includes('network|192.0.2.1|00:11:22:33:44:55'));
const baseline=writes;
for(let i=0;i<1000;i++)events.message({data:'DIAG fixture '+i});
assert.equal(writes,baseline,'diagnostics must not render per packet');
assert.equal(timers.filter(t=>t.ms===1000).length,1);
timers.find(t=>t.ms===1000).f();
assert.equal(writes,baseline+1);
assert.equal(nodes.report.value.split('\n').length,500,'bounded history');
assert(nodes.report.value.endsWith('999'));
events.message({data:'DIAG tcp peer connected'});
assert.notEqual(nodes.state.textContent,'A device connected');
events.message({data:'DIAG video playback advancing'});assert.equal(nodes.ui.style.display,'none');
events.message({data:'DIAG session stopped'});assert.equal(nodes.ui.style.display,'flex');
document.hidden=true;docEvents.visibilitychange();assert.equal(messages.pop(),'suspend');
document.hidden=false;docEvents.visibilitychange();assert.equal(messages.pop(),'report');
events.crash();assert.equal(nodes.ui.style.display,'flex');assert(nodes.state.textContent.includes('Reopen'));
console.log('PASS shell: 1000 diagnostics batched into one DOM write, bounded history, session UI, suspend/resume, crash state');
