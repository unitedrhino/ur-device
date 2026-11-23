// 文件说明：正式JS宿主分派与生命周期回归；媒体类/DOM为显式夹具，不冒称实际解码E2E。
const test = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/file-host.js'), 'utf8');

// fixture 记录原输出节点、长控制与独立关闭，不在模拟类构造时伪造实际完成。
function fixture() {
  const nodes = [], players = [], calls = [];
  let failed = 0, unblock;
  const document = {createElement() { return {setAttribute() {}, appendChild() {}, remove() {}}; },
    querySelector() { return {appendChild() {}}; }};
  class Lane {
    constructor(context, mount, send, lane) { this.mount = mount; this.lane = lane; this.node = {}; nodes.push(this); }
    async gate(value) { calls.push(value.kind); return !value.invalid; }
    destination() { return this.node; }
    release() { this.released = true; }
  }
  class Player {
    constructor(context, mount, send, token, destination) { this.destination = destination; players.push(this); }
    async initialize() { calls.push('bind'); }
    async control() { calls.push('control'); await new Promise(resolve => { unblock = resolve; }); }
    async close() { calls.push('close'); return true; }
    async outputGate() { return true; }
    release() { calls.push('release'); }
  }
  const scope = {document, UrFileLane: Lane, UrFilePlayer: Player};
  vm.runInNewContext(source, scope);
  const host = new scope.UrDesktopFiles({destination:{}}, ()=>{}, 'local-fixture', ()=>{ failed++; });
  return {host,nodes,players,calls,unblock:()=>unblock?.(),failed:()=>failed};
}

test('每种完整组只能接原固定出口，实际关闭前不换播放器', async () => {
  for (const [mime,audio,lane] of [['audio/wav',true,'audio'],['video/mp4',false,'video'],['video/webm',true,'audioVideo']]) {
    const f = fixture();
    await f.host.execute({kind:'fileLaneGate',lane,enabled:false});
    await f.host.execute({kind:'fileBind',mime,audio});
    assert.equal(f.players[0].destination, f.nodes[0].node);
    await assert.rejects(f.host.execute({kind:'fileBind',mime,audio}), /busy/);
    await f.host.execute({kind:'fileClose'});
    assert.equal(f.host.player,null);
    await f.host.execute({kind:'fileBind',mime,audio});
    f.host.close(); f.host.close();
    assert.equal(f.nodes[0].released,true);
    assert.equal(f.calls.filter(x=>x==='release').length,1);
  }
});

test('关闭立即中断生命周期，不排在长控制后且迟到任务不复活', async () => {
  const f = fixture();
  await f.host.execute({kind:'fileLaneGate',lane:'audio',enabled:false});
  await f.host.execute({kind:'fileBind',mime:'audio/wav',audio:true});
  const control = f.host.execute({kind:'fileControl'});
  await f.host.execute({kind:'fileClose'});
  assert.deepEqual(f.calls,['fileLaneGate','bind','control','close']);
  f.host.close();
  f.unblock(); await control;
  await f.host.execute({kind:'fileBind',mime:'audio/wav',audio:true});
  assert.equal(f.players.length,1);
});

test('未知出口不能绕过默认扬声器，坏路径失败且不接管AI消息', async () => {
  const f = fixture();
  assert.equal(f.host.handle({kind:'playbackFrame'}),false);
  await assert.rejects(f.host.execute({kind:'fileBind',mime:'audio/wav',audio:true}), /unknownLane/);
  assert.equal(f.players.length,0);
  assert.equal(f.host.handle({kind:'fileLaneGate',lane:'microphone'}),true);
  await new Promise(resolve=>setImmediate(resolve));
  assert.equal(f.failed(),1);
  assert.equal(f.host.closed,true);
});
