// 文件说明：真实三C++接续驱动的门禁与解码/停流判据；离线用例不宣称真实后端通过。
'use strict';
const test=require('node:test');
const assert=require('node:assert/strict');
const {configuration,decodedMedia,stoppedMedia,finishDevice,deviceCompletion}=require('./native_endpoint_handoff_live.cjs');
// fixture 使用公开占位值及假文件只校验配置，不创建进程或网络。
function fixture(){return {UR_NATIVE_ENDPOINT_HANDOFF_LIVE:'1',UR_IPC_HUMAN_CALL:'1',UR_IPC_HUMAN_RECEIVE_VIDEO:'1',
  UR_MQTT_HOST:'127.0.0.1',UR_IPC_DEVICE_BIN:'/placeholder/device',UR_DEVICE_PRODUCT:'p',UR_DEVICE_NAME:'source',UR_DEVICE_SECRET:'placeholder',UR_IPC_DEVICE_PORT:'19001',UR_IPC_AUDIO_FIXTURE:'/placeholder/a.wav',
  UR_NATIVE_ENDPOINT_RETAINED_PRODUCT:'p',UR_NATIVE_ENDPOINT_RETAINED_NAME:'retained',UR_NATIVE_ENDPOINT_RETAINED_SECRET:'placeholder',UR_NATIVE_ENDPOINT_RETAINED_PORT:'19002',UR_NATIVE_ENDPOINT_RETAINED_AUDIO_FIXTURE:'/placeholder/b.wav',
  UR_NATIVE_ENDPOINT_TARGET_PRODUCT:'p',UR_NATIVE_ENDPOINT_TARGET_NAME:'target',UR_NATIVE_ENDPOINT_TARGET_SECRET:'placeholder',UR_NATIVE_ENDPOINT_TARGET_PORT:'19003',UR_NATIVE_ENDPOINT_TARGET_AUDIO_FIXTURE:'/placeholder/c.wav'};}
test('explicit three real identities and unique loopback ports required',()=>{
  const stat=()=>({isFile:()=>true});assert.equal(configuration(fixture(),stat).devices.length,3);
  for(const patch of [{UR_NATIVE_ENDPOINT_HANDOFF_LIVE:'0'},{UR_IPC_HUMAN_RECEIVE_VIDEO:'0'},{UR_MQTT_HOST:'production.example.invalid'},
    {UR_NATIVE_ENDPOINT_TARGET_NAME:'source'},{UR_NATIVE_ENDPOINT_TARGET_PORT:'19001'},{UR_NATIVE_ENDPOINT_TARGET_SECRET:''},
    {UR_NATIVE_ENDPOINT_HANDOFF_OPERATION:'force'}])assert.throws(()=>configuration({...fixture(),...patch},stat));
});
test('RTP alone or local encoded video never satisfies actual remote decode',()=>{
  const value={connected:true,active:true,audioPackets:5,audioSamples:1920,energy:0.01,audioPeak:0.02,videoFrames:4,videoPackets:8,pixels:100};
  assert.equal(decodedMedia(value),true);
  for(const patch of [{active:false},{audioSamples:0},{audioPeak:0},{energy:0},{videoFrames:0},{pixels:0},{connected:false}])assert.equal(decodedMedia({...value,...patch}),false);
});
test('deleting view without actual capture and remote stop is not cleanup',()=>{
  const value={present:false,peer:'closed',tracks:2,tracksEnded:true,remoteAudioEnded:true,remoteVideoEnded:true};
  assert.equal(stoppedMedia(value),true);
  for(const patch of [{present:true},{peer:'connected'},{tracks:0},{tracksEnded:false},{remoteAudioEnded:false},{remoteVideoEnded:false}])assert.equal(stoppedMedia({...value,...patch}),false);
});
test('cleanup failure preserves the original business failure and reports both',()=>{
  // 直接执行维护驱动现有最终清理分支，防止验收首因被进程退出错误覆盖。
  const source=require('node:fs').readFileSync(require.resolve('./native_endpoint_handoff_live.cjs'),'utf8');
  const start=source.indexOf('if(cleanupFailures.length)');
  const end=source.indexOf('\n  }',start);
  assert.ok(start>0&&end>start);
  const finish=new Function('report','cleanupFailures',source.slice(start,end)+';return report;');
  const result=finish({result:'fail',stage:'original_call',reason:'native_state_timeout'},['device_exit_nonzero']);
  assert.equal(result.result,'fail');assert.equal(result.stage,'original_call');
  assert.equal(result.reason,'native_state_timeout');assert.deepEqual(result.cleanupFailures,['device_exit_nonzero']);
});
test('正常shutdown先等实际退出，不能立刻TERM掩盖正常收尾',async()=>{
  let exited=false;const signals=[];const child={kill:signal=>signals.push(signal)};
  const completion=new Promise(resolve=>setImmediate(()=>{exited=true;resolve();}));
  await finishDevice(child,completion,()=>exited,100);
  assert.deepEqual(signals,[]);
});
test('shutdown无响应才精确终止原进程，不能跳过清理',async()=>{
  let exited=false,resolve;const signals=[];
  const completion=new Promise(done=>{resolve=done;});
  const child={kill:signal=>{signals.push(signal);exited=true;resolve();}};
  await finishDevice(child,completion,()=>exited,5);
  assert.deepEqual(signals,['SIGTERM']);
});
test('实际exit尚未排空stderr不能被误记为close完成',async()=>{
  const {EventEmitter}=require('node:events');
  const device={child:new EventEmitter(),exited:false};let completed=false;
  const promise=deviceCompletion(device).then(()=>{completed=true;});
  device.child.emit('exit',0);await Promise.resolve();
  assert.equal(completed,false);assert.equal(device.exited,false);
  device.child.emit('close',0);await promise;
  assert.equal(completed,true);assert.equal(device.exited,true);assert.equal(device.exitCode,0);
});
