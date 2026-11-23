// 文件说明：浏览器停轨验收器的离线判据，不以这些对象代替真实Chromium执行。
'use strict';
const test=require('node:test'),assert=require('node:assert/strict');
const {verifyTracks,frame,browserArgs}=require('./browser_resource_release_live.cjs');
test('本机Chromium参数按CLI实际逗号合同分隔，真实媒体开关不能拼成单个参数',()=>{
  assert.deepEqual(browserArgs.split(','),['--no-sandbox','--use-fake-device-for-media-stream','--use-fake-ui-for-media-stream','--autoplay-policy=no-user-gesture-required']);
  assert.equal(browserArgs.includes(' '),false);
});
test('停轨证据必须是全部真实MediaStreamTrack，不能以Mock或仅部分ended通过',()=>{
  const fact={tracks:[{kind:'audio',state:'ended',native:true},{kind:'video',state:'ended',native:true}]};
  verifyTracks(fact,['audio','video'],'ended');
  for(const change of [{tracks:fact.tracks.slice(0,1)},{tracks:[{...fact.tracks[0],native:false},fact.tracks[1]]},{tracks:[{...fact.tracks[0],state:'live'},fact.tracks[1]]},{tracks:[fact.tracks[0],{...fact.tracks[1],kind:'audio'}]}])assert.throws(()=>verifyTracks(change,['audio','video'],'ended'));
});
test('本地桥只允许有界文本或关闭帧，不引入第三方服务',()=>{
  assert.deepEqual(frame('ok'),Buffer.from([0x81,2,111,107]));assert.deepEqual(frame('',8),Buffer.from([0x88,0]));
  assert.throws(()=>frame('a'.repeat(65536)));
});
