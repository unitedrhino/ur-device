// 文件说明：历史专用宿主页面，固定桥配对和只读状态；没有自由MQTT/URL/摄像头开关。
'use strict';
(async () => {
  // status/state只写textContent，不把远端文字或私有标识解释成HTML。
  const status = document.getElementById('record-status'), state = document.getElementById('record-session');
  let socket = null, source = null, closing = false;
  // send只交本地白名单事实，不含设备认证；连接关闭不补发假回执。
  const send = value => {
    if (!socket || socket.readyState !== WebSocket.OPEN) return false;
    socket.send(JSON.stringify(value)); return true;
  };
  // stop页面失联也主动切断本源；本地实际关闭不等于平台收到released。
  const stop = async () => {
    if (closing) return;
    closing = true;
    if (source?.binding) {
      try { await source.close({kind: 'recordClose', sessionId: source.binding.sessionId,
        playbackId: source.binding.playbackId, releaseId: '1'}); } catch { /* 关闭失败保持未知，不伪造成功。 */ }
    }
    status.textContent = '本地连接已关闭';
  };
  try {
    const response = await fetch('/bootstrap', {cache: 'no-store'});
    if (!response.ok) throw Error('pairingFailed');
    const value = await response.json();
    if (Object.keys(value).length !== 1 || typeof value.token !== 'string' || !/^[0-9a-f]{64}$/.test(value.token))
      throw Error('pairingFailed');
    socket = new WebSocket(`ws://${location.host}/bridge?token=${value.token}`);
    source = new UrRecordSource({send, token: value.token, mount: document.getElementById('record-media')});
    globalThis.recordSource = source; // 仅本回环页面供中央实测读取实际Peer/轨/解码事实，不代表后端收录。
    globalThis.recordHostState = null; // 原核心只读投影，与源实际媒体状态分开。
    socket.onopen = () => { status.textContent = '本地桥已配对，等待能力协商'; send({kind: 'ready'}); };
    socket.onmessage = async event => {
      try {
        const message = JSON.parse(event.data);
        if (message.kind === 'recordAvailable') status.textContent = '历史能力已确认，等待平台邀请';
        else if (message.kind === 'recordSessionState') {
          const phases = ['prepared', 'accepting', 'rejecting', 'ready', 'negotiating', 'active', 'seeking', 'ended'];
          const states = ['', 'loaded', 'playing', 'paused', 'seeking', 'stopped', 'ended', 'failed'];
          if (Object.keys(message).length !== 7 || !phases.includes(message.phase) || !states.includes(message.state) ||
              !Number.isSafeInteger(message.revision) || message.revision < 0 || !Number.isSafeInteger(message.mediaRevision) ||
              message.mediaRevision < 1 || typeof message.released !== 'boolean' || typeof message.releaseSubmitted !== 'boolean')
            throw Error('invalidProjection');
          globalThis.recordHostState = Object.freeze({...message});
          state.textContent = `核心阶段：${message.phase}；实际状态：${message.state || '未加载'}；媒体代次：${message.mediaRevision}；实际释放：${message.released ? '是' : '待确认'}`;
        } else if (!(await source.incoming(message))) {
          // 原源状态机已报告脱敏失败事实；不由UI自行播放、续期或补Connected。
          status.textContent = '档案驱动拒绝本地操作';
        }
      } catch { status.textContent = '本地消息处理失败'; await stop(); }
    };
    socket.onclose = () => { void stop(); };
    socket.onerror = () => { status.textContent = '本地桥连接失败'; void stop(); };
    document.getElementById('record-close').addEventListener('click', () => { send({kind: 'shutdown'}); });
    globalThis.addEventListener('pagehide', () => { void stop(); });
  } catch { status.textContent = '本地桥配对失败'; await stop(); }
})();
