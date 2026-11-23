// 文件说明：独立视频页保持/恢复的离线媒体合同；VM不替代真实浏览器/RTP/扬声器验收。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

// fixture 模拟DOM与原发送轨，只观测生产页面调用，不猴补云端信令或播放器实现。
function fixture() {
  const messages = [], audioCommands = [], frames = [], elements = new Map();
  let node;
  const element = id => {
    if (!elements.has(id)) elements.set(id, {disabled: false, textContent: '',
      style: {}, paused: false, muted: false, listeners: {},
      addEventListener(kind, callback) { this.listeners[kind]=callback; },
      pause() { this.paused = true; }, play() { this.paused = false; return Promise.resolve(); }});
    return elements.get(id);
  };
  const sandbox = {document: {getElementById: element}, console, setTimeout, clearTimeout,
    setInterval() {}, WebSocket: {OPEN: 1}, window: {addEventListener() {}},
    requestAnimationFrame: callback => frames.push(callback),
    AudioContext: class {
      constructor() { this.sampleRate=48000;this.state='running';this.audioWorklet={addModule:async()=>{}}; }
      async resume() {} async close() {}
      createMediaStreamSource(stream) { return {stream,connect() {},disconnect() {}}; } },
    AudioWorkletNode: class {
      constructor() { node=this;this.port={postMessage:value=>audioCommands.push(value)}; } connect() {} },
    navigator: {}, location: {host: '127.0.0.1'},
    MediaStream: class { constructor(tracks) {this.tracks=tracks;}getAudioTracks(){return this.tracks.filter(value=>value.kind==='audio');} }};
  vm.createContext(sandbox);
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/video.js'), 'utf8'), sandbox);
  sandbox.record = value => messages.push(JSON.parse(value));
  vm.runInContext("socket={readyState:1,bufferedAmount:0,send:record,close(){}};state='connected';", sandbox);
  // mount 保留原两轨及Sender；resolve只有显式驱动完成时才更新原track。
  const mount = () => {
    const tracks = ['audio','video'].map(kind => ({kind,readyState:'live',enabled:kind!=='audio',stop(){this.readyState='ended';}}));
    const senders = tracks.map(track => ({track,targets:[],pending:[],
      replaceTrack(target) { this.targets.push(target);return new Promise(resolve=>this.pending.push(()=>{this.track=target;resolve();})); }}));
    sandbox.fixtureCapture={getTracks:()=>tracks,getAudioTracks:()=>[tracks[0]],getVideoTracks:()=>[tracks[1]]};
    sandbox.fixturePeer={getSenders:()=>senders,close(){this.closed=true;}};
    vm.runInContext("peer=fixturePeer;capture=fixtureCapture;peerEpoch='1';callSession='original-leg';",sandbox);
    return {tracks,senders,peer:sandbox.fixturePeer};
  };
  return {run: source => vm.runInContext(source, sandbox), messages, element, mount,
    click: id => element(id).listeners.click(),
    // configureRemote 执行原configure/ontrack；模拟网络端口不代替真实Chromium解码探针。
    configureRemote: async () => {
      const tracks=['audio','video'].map(kind=>({kind,readyState:'live',enabled:true,stop(){}}));
      sandbox.navigator.mediaDevices={getUserMedia:async()=>({getTracks:()=>tracks,getAudioTracks:()=>[tracks[0]],getVideoTracks:()=>[tracks[1]]})};
      const remotePeer={addTrack(){},iceGatheringState:'complete',createOffer:async()=>({type:'offer',sdp:'v=0\r\n'}),
        async setLocalDescription(value){this.localDescription=value;},close(){}};
      sandbox.RTCPeerConnection=class{constructor(){return remotePeer;}};
      await vm.runInContext("callAudioReady()",sandbox);
      await vm.runInContext("configure({kind:'callConfigure',peerEpoch:'1'})",sandbox);
      const track={kind:'audio',readyState:'live'};
      remotePeer.ontrack({track});await flush();return {track,remotePeer};
    },
    audioCommands, audio: value => node.port.onmessage({data:value}),
    paint: () => { const callbacks=frames.splice(0);for(const callback of callbacks) callback(); }};
}
// flush 仅排空纯Promise微任务，不制造媒体块、绘制回执或sleep。
async function flush() { for(let index=0;index<5;++index) await Promise.resolve(); }
// hold 原命令字段固定，不能通过测试eval替代实际设备状态修改。
const hold = (held, ticket='1') => `incoming({kind:'callHoldGate',sessionId:'original-leg',
  peerEpoch:'1',ticket:'${ticket}',held:${held},controlToken:'original-${held?'hold':'resume'}',revision:${held?1:2}})`;

// transferReceipt 仅构造宿主固定只读字段，候选ID/目标/版本不能由界面自授。
function transferReceipt(overrides={}) {
  return {kind:'callTransferReceipt',sessionId:'original-leg',controlToken:'original-transfer',
    state:'transferring',code:200,ackRevision:1,resultRevision:0,reason:'',candidatePresent:true,
    cancelToken:'',cancelState:'idle',cancelCode:0,cancelReason:'',...overrides};
}

test('production transfer receipt changes only controls, never original Peer or actual media', () => {
  const value=fixture(), original=value.mount();
  assert.doesNotThrow(()=>value.run(`incoming(${JSON.stringify(transferReceipt())})`));
  assert.equal(original.peer.closed,undefined);
  assert.deepEqual(original.senders.map(sender=>sender.targets),[[],[]]);
  assert.equal(value.audioCommands.length,0);assert.equal(value.messages.length,0);
  assert.equal(value.element('cancelTransfer').disabled,false);
});

test('real registered transfer clicks submit only target intent once and cancellation has no UI candidate', () => {
  const value=fixture();value.mount();value.run('buttons()');
  value.element('transferTarget').value='p1:camera-03';value.click('transfer');value.click('transfer');
  assert.deepEqual(value.messages,[{kind:'callControl',operation:'transfer',target:'p1:camera-03'}]);
  value.run(`incoming(${JSON.stringify(transferReceipt())})`);
  value.click('cancelTransfer');value.click('cancelTransfer');
  assert.deepEqual(value.messages.at(-1),{kind:'callControl',operation:'cancelTransfer'});
  assert.equal(value.messages.length,2);
  value.run(`incoming(${JSON.stringify(transferReceipt())})`);
  value.click('cancelTransfer');assert.equal(value.messages.length,2,'重复受理不能解除原撤候选等待');
});

test('old token terminal receipt cannot unlock a new operation and local refusal never retires original Peer', () => {
  const value=fixture(), original=value.mount();
  value.run(`incoming(${JSON.stringify(transferReceipt({state:'failed'}))})`);
  value.run(`incoming(${JSON.stringify(transferReceipt({controlToken:'second-transfer',state:'pending',code:0,candidatePresent:false}))})`);
  value.run(`incoming(${JSON.stringify(transferReceipt({state:'cancelled'}))})`);
  assert.equal(value.element('transfer').disabled,true);assert.equal(value.element('hold').disabled,true);
  value.run("incoming({kind:'callTransferRejected',operation:'transfer'})");
  assert.equal(value.element('transfer').disabled,true,'本地拒绝不能覆盖原已记录pending');
  assert.equal(value.run('controlMetrics().transferReceipts.length'),2);
  value.run("incoming({kind:'callStop'})");
  value.run(`incoming(${JSON.stringify(transferReceipt({controlToken:'second-transfer',state:'transferred'}))})`);
  assert.equal(original.peer.closed,true);assert.equal(value.run('latestTransfer'),null);
  assert.equal(value.messages.length,0);assert.equal(value.audioCommands.length,0);
});

test('explicit new leg can submit a new transfer after local refusal without reviving old leg', () => {
  const value=fixture();value.mount();value.run('buttons()');
  value.element('transferTarget').value='p1:camera-03';value.click('transfer');
  value.run("incoming({kind:'callTransferRejected',operation:'transfer'})");
  assert.equal(value.element('transfer').disabled,false);
  assert.equal(value.run('closed'),false);
  value.run("releasePeer()");value.mount();value.run("callSession='new-leg';buttons()");
  value.run(`incoming(${JSON.stringify(transferReceipt())})`);
  value.click('transfer');assert.equal(value.messages.length,2);
  assert.equal(value.element('transfer').disabled,true);
});

test('transfer rejects invalid device target and candidate cannot be cancelled without original accepted fact', () => {
  for(const target of ['bad','p:','p:x/y','p:x#','p:x\n','p:'+ 'x'.repeat(128)]) {
    const value=fixture();value.mount();value.run('buttons()');
    value.element('transferTarget').value=target;value.click('transfer');value.click('cancelTransfer');
    assert.equal(value.messages.length,0);
  }
});

test('cancel ACK rejection is independent, and transfer terminal states never release original media', () => {
  for(const state of ['transferred','failed','cancelled','rejected','unknown']) {
    const value=fixture(), original=value.mount();
    value.run(`incoming(${JSON.stringify(transferReceipt({state}))})`);
    assert.equal(original.peer.closed,undefined);assert.equal(value.run('outputEnabled'),false);
    assert.deepEqual(original.senders.map(sender=>sender.targets),[[],[]]);
    assert.equal(value.messages.length,0);assert.equal(value.audioCommands.length,0);
    if(state==='transferred'||state==='unknown') assert.equal(value.element('hold').disabled,true);
    value.run("incoming({kind:'callStop'})");assert.equal(original.peer.closed,true);
  }
  const value=fixture();value.mount();
  value.run(`incoming(${JSON.stringify(transferReceipt({cancelToken:'original-cancel',cancelState:'rejected',cancelCode:409,cancelReason:'invalidState'}))})`);
  assert.equal(value.element('transfer').disabled,true);assert.equal(value.element('hold').disabled,true);
  assert.equal(value.run('controlMetrics().transferReceipts[0].cancelState'),'rejected');
});

test('transfer projections update the same token and are bounded, with stale session and malformed fields isolated', () => {
  const value=fixture();value.mount();
  value.run(`incoming(${JSON.stringify(transferReceipt())})`);
  value.run(`incoming(${JSON.stringify(transferReceipt({state:'failed',reason:'transferRejected'}))})`);
  assert.equal(value.run('controlMetrics().transferReceipts.length'),1);
  assert.equal(value.run('controlMetrics().transferReceipts[0].state'),'failed');
  value.run(`incoming(${JSON.stringify(transferReceipt({sessionId:'old-leg',state:'transferred'}))})`);
  assert.equal(value.run('controlMetrics().transferReceipts[0].state'),'failed');
  for(let index=1;index<25;++index) value.run(`incoming(${JSON.stringify(transferReceipt({controlToken:'transfer-'+index,state:'failed'}))})`);
  assert.equal(value.run('controlMetrics().transferReceipts.length'),24);
  assert.equal(value.run('controlMetrics().transferOverflow'),true);
  for(const patch of [{candidateSessionId:'not-authorized'},{code:201},{ackRevision:-1},{resultRevision:1.5},
    {cancelState:'applied'},{reason:'free-platform-text'},{cancelReason:'free-platform-text'},
    {controlToken:'bad\n'},{candidatePresent:'true'}]) {
    assert.throws(()=>value.run(`incoming(${JSON.stringify(transferReceipt(patch))})`),/invalid_transfer_receipt/);
  }
  assert.equal(value.messages.length,0);
});

test('remote audio decoder is driven by muted native element, while all audible output remains gated', async () => {
  const value=fixture(), {track}=await value.configureRemote();
  const element=value.element('remoteAudio');
  assert.equal(element.srcObject?.getAudioTracks()[0],track);
  assert.equal(element.muted,true,'原生播放元素不能绕过末端worklet出声');
  assert.equal(element.paused,false,'远端WebRTC音频需要原生播放元素驱动解码');
  assert.equal(value.run('outputEnabled'),false,'解码启动不意味着输出已获许可');
  assert.equal(value.audioCommands.length,0,'没有伪造音频门控回执');
  value.run("releasePeer()");
  assert.equal(element.srcObject,null);assert.equal(element.paused,true);
});

test('remote decoder play rejection terminates current media without an applied receipt', async () => {
  const value=fixture();value.element('remoteAudio').play=()=>Promise.reject(Error('decoder_unavailable'));
  await value.configureRemote();await flush();
  assert.equal(value.run('closed'),true);assert.equal(value.run('outputEnabled'),false);
  assert.equal(value.messages.filter(value=>value.kind==='callOutputApplied').length,0);
});

test('released Peer late remote track cannot bind or reopen native decoder', async () => {
  const value=fixture(), {remotePeer}=await value.configureRemote();
  value.run('releasePeer()');
  remotePeer.ontrack({track:{kind:'audio',readyState:'live'}});await flush();
  assert.equal(value.element('remoteAudio').srcObject,null);
  assert.equal(value.element('remoteAudio').paused,true);
  assert.equal(value.run('remoteAudioSource'),null);
  assert.equal(value.run('closed'),false,'旧轨不把可接受后继呼叫的设备变为终态');
});

test('old decoder play rejection after Peer release does not terminate device', async () => {
  const value=fixture();let reject;
  value.element('remoteAudio').play=()=>new Promise((_,failure)=>{reject=failure;});
  await value.configureRemote();value.run('releasePeer()');reject(Error('old_decoder_unavailable'));await flush();
  assert.equal(value.run('closed'),false);assert.equal(value.run('outputEnabled'),false);
  assert.equal(value.messages.filter(value=>value.kind==='callOutputApplied').length,0);
});

test('production page recognizes actual hold command without treating it as unknown fatal event', () => {
  const fixtureValue = fixture();
  assert.doesNotThrow(() => fixtureValue.run(`incoming({kind:'callHoldGate',sessionId:'original-leg',
    peerEpoch:'1',ticket:'1',held:true,controlToken:'original-hold',revision:1})`));
  assert.equal(fixtureValue.run('closed'), false);
});

test('both actual sender detachments are required, explicit resume restores same tracks and manual mute', async () => {
  const value=fixture(), {tracks,senders,peer}=value.mount();
  value.run(hold(true));
  assert.deepEqual(senders.map(sender=>sender.targets),[[null],[null]]);
  assert.equal(value.messages.length,0);
  senders[0].pending.shift()();await flush();assert.equal(value.messages.length,0);
  senders[1].pending.shift()();await flush();
  assert.equal(value.messages.at(-1).kind,'callHoldApplied');
  assert.equal(value.messages.at(-1).applied,true);assert.equal(value.messages.at(-1).held,true);
  value.run(hold(false,'2'));assert.equal(value.messages.length,1);
  assert.equal(senders[0].targets[1],tracks[0]);assert.equal(senders[1].targets[1],tracks[1]);
  senders.forEach(sender=>sender.pending.shift()());await flush();
  assert.equal(value.messages.at(-1).held,false);assert.equal(value.messages.at(-1).applied,true);
  assert.equal(tracks[0].enabled,false);assert.equal(tracks[1].enabled,true);
  assert.equal(peer.closed,undefined);assert.equal(value.run('holdEffects'),2);
});

test('actual output requires original audio block and both real paint boundaries', async () => {
  const value=fixture();value.mount();await value.run('callAudioReady()');
  value.run("incoming({kind:'callOutputGate',sessionId:'original-leg',peerEpoch:'1',gateTicket:'1',enabled:true})");
  assert.equal(value.audioCommands.length,1);assert.equal(value.messages.length,0);
  assert.equal(value.element('hold').disabled,true,'实际音频和画面等待未完成前保持按钮仍禁用');
  value.audio({kind:'callAudioGated',ticket:'wrong',peerEpoch:'1',enabled:true});
  value.paint();value.paint();await flush();assert.equal(value.messages.length,0);
  value.audio({kind:'callAudioGated',ticket:'1',peerEpoch:'1',enabled:true});
  assert.equal(value.messages.at(-1).kind,'callOutputApplied');
  assert.equal(value.messages.at(-1).applied,true);
  assert.equal(value.element('remote').style.visibility,'visible');
  assert.equal(value.element('hold').disabled,false,'实际输出就绪必须解除本地保持按钮等待，不能等不存在的下一条callState');
  value.run("incoming({kind:'callOutputGate',sessionId:'original-leg',peerEpoch:'1',gateTicket:'2',enabled:false})");
  value.audio({kind:'callAudioGated',ticket:'2',peerEpoch:'1',enabled:false});
  assert.equal(value.messages.length,1);value.paint();value.paint();await flush();
  assert.equal(value.messages.at(-1).enabled,false);assert.equal(value.element('remote').paused,true);
});

test('close cancels pending actual receipts, old ticket and old Peer cannot reopen', async () => {
  const value=fixture(), {senders}=value.mount();await value.run('callAudioReady()');
  value.run(hold(true));
  value.run("incoming({kind:'callOutputGate',sessionId:'original-leg',peerEpoch:'1',gateTicket:'1',enabled:true})");
  value.run("stop('fixed-close')");
  senders.forEach(sender=>sender.pending.shift()());
  value.paint();value.paint();await flush();
  value.audio({kind:'callAudioGated',ticket:'1',peerEpoch:'1',enabled:true});
  assert.equal(value.messages.length,0);assert.equal(value.run('outputEnabled'),false);
  assert.equal(value.run('holdEffects'),0);assert.equal(value.run('closed'),true);
  assert.throws(()=>value.run(hold(false,'2')),/invalid_hold_gate/);
});

test('failed restore detachment silences original output and never reports applied success', async () => {
  const value=fixture(), {senders}=value.mount();await value.run('callAudioReady()');
  value.run(hold(true));senders.forEach(sender=>sender.pending.shift()());await flush();
  value.run("incoming({kind:'callOutputGate',sessionId:'original-leg',peerEpoch:'1',gateTicket:'1',enabled:true})");
  value.audio({kind:'callAudioGated',ticket:'1',peerEpoch:'1',enabled:true});
  value.paint();value.paint();await flush();
  senders[0].replaceTrack=()=>Promise.reject(Error('fixed_driver_failure'));
  value.run(hold(false,'2'));await flush();
  assert.equal(value.messages.at(-1).kind,'callHoldApplied');
  assert.equal(value.messages.at(-1).applied,false);
  assert.equal(value.run('outputEnabled'),false);
  assert.equal(value.audioCommands.at(-1).kind,'callAudioAbort');
  assert.equal(value.audioCommands.at(-1).ticket,'1');
  assert.equal(value.element('remote').paused,true);
  assert.equal(value.run('holdEffects'),1);
});
