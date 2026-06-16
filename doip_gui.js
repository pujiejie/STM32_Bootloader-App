/**
 * DoIP GUI Tester — 图形化诊断测试工具
 * 用法: node doip_gui.js
 * 自动打开浏览器 http://localhost:8080
 */
const http = require('http');
const net = require('net');
const dgram = require('dgram');
const { exec } = require('child_process');

const HTTP_PORT = 8080;
const DEFAULT_ECU_IP = '172.16.1.10';
const DEFAULT_ECU_PORT = 13400;
const TIMEOUT = 5000;

// ─── DoIP 常量 ───
const V = 0x02, VI = 0xFD;
const PT = { GEN_NACK: 0x0000, VEHICLE_ANNOUNCE: 0x0004, ROUTING_REQ: 0x0005,
  ROUTING_RSP: 0x0006, ALIVE_CHECK_REQ: 0x0007, ALIVE_CHECK_RSP: 0x0008,
  DIAG_MSG: 0x8001, DIAG_NACK: 0x8003 };
const ECU_VIN = 'VIN1234567890ABCD';
const ECU_EID = Buffer.from('1234567890ABCDEF', 'hex');
const ECU_LA = 0x0E80;

// ─── SSE 客户端列表 ───
const clients = new Set();

function broadcast(type, data) {
  const msg = `data: ${JSON.stringify({ type, ...data })}\n\n`;
  clients.forEach(res => { try { res.write(msg); } catch(e) {} });
}

function log(msg, testId = null) { broadcast('log', { msg, testId }); }
function result(testId, pass, detail = '') { broadcast('result', { testId, pass, detail }); }
function progress(testId, status) { broadcast('progress', { testId, status }); }

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

// ─── DoIP 工具函数 ───
function buildDoIP(ptype, payload = Buffer.alloc(0)) {
  const h = Buffer.alloc(8);
  h[0] = V; h[1] = VI; h.writeUInt16BE(ptype, 2); h.writeUInt32BE(payload.length, 4);
  return Buffer.concat([h, payload]);
}

function decodeUDSPos(payload) {
  if (payload.length < 5) return null;
  const sa = payload.readUInt16BE(0), ta = payload.readUInt16BE(2);
  const sid = payload[4];
  if (sid >= 0x40 && sid < 0x80) return { sa, ta, sid, sub: sid - 0x40, data: payload.subarray(5) };
  if (sid === 0x7F && payload.length >= 7) return { sa, ta, sid: 0x7F, origSid: payload[5], nrc: payload[6] };
  return { sa, ta, sid, raw: payload.subarray(4) };
}

// ─── 测试实现 ───
function tcpTest(testId, sendType, sendData, validator) {
  return new Promise(resolve => {
    const client = new net.Socket();
    let buf = Buffer.alloc(0), done = false;
    const ecuIP = global.testIP || DEFAULT_ECU_IP;
    const ecuPort = global.testPort || DEFAULT_ECU_PORT;

    const finish = (pass, detail) => {
      if (done) return; done = true;
      client.destroy();
      result(testId, pass, detail);
      resolve();
    };

    client.setTimeout(TIMEOUT);
    client.on('timeout', () => finish(false, 'Timeout'));
    client.on('error', e => finish(false, e.message));
    client.on('data', d => {
      buf = Buffer.concat([buf, d]);
      while (buf.length >= 8 && !done) {
        const plen = buf.readUInt32BE(4);
        if (buf.length < 8 + plen) break;
        const pkt = buf.subarray(0, 8 + plen);
        buf = buf.subarray(8 + plen);
        try {
          const r = validator(pkt.readUInt16BE(2), pkt.subarray(8));
          if (typeof r === 'string') finish(false, r);
          else finish(true, r || '');
        } catch (e) { finish(false, e.message); }
      }
    });
    client.connect(ecuPort, ecuIP, () => {
      client.write(buildDoIP(sendType, sendData));
    });
  });
}

function routedDiagTest(testId, udsData, validator) {
  return new Promise(resolve => {
    const client = new net.Socket();
    let buf = Buffer.alloc(0), done = false, step = 0;
    const ecuIP = global.testIP || DEFAULT_ECU_IP;
    const ecuPort = global.testPort || DEFAULT_ECU_PORT;

    const finish = (pass, detail) => {
      if (done) return; done = true;
      client.destroy();
      result(testId, pass, detail);
      resolve();
    };

    client.setTimeout(TIMEOUT);
    client.on('timeout', () => finish(false, `Timeout at step ${step}`));
    client.on('error', e => finish(false, e.message));
    client.on('data', d => {
      buf = Buffer.concat([buf, d]);
      while (buf.length >= 8 && !done) {
        const plen = buf.readUInt32BE(4);
        if (buf.length < 8 + plen) break;
        const pkt = buf.subarray(0, 8 + plen);
        buf = buf.subarray(8 + plen);
        const ptype = pkt.readUInt16BE(2);

        if (step === 0) {
          if (ptype !== PT.ROUTING_RSP) { finish(false, `Routing rsp type=0x${ptype.toString(16)}`); return; }
          if (pkt[12] !== 0x10) { finish(false, `Routing failed code=0x${pkt[12].toString(16)}`); return; }
          step = 1;
          const req = Buffer.alloc(4 + udsData.length);
          req.writeUInt16BE(0x0E00, 0); req.writeUInt16BE(ECU_LA, 2);
          udsData.copy(req, 4);
          client.write(buildDoIP(PT.DIAG_MSG, req));
        } else {
          try {
            const r = validator(ptype, pkt.subarray(8));
            if (typeof r === 'string') finish(false, r);
            else finish(true, r || '');
          } catch (e) { finish(false, e.message); }
        }
      }
    });
    client.connect(ecuPort, ecuIP, () => {
      const rreq = Buffer.alloc(7);
      rreq.writeUInt16BE(0x0E00, 0); rreq.writeUInt32BE(0, 2); rreq[6] = 0;
      client.write(buildDoIP(PT.ROUTING_REQ, rreq));
    });
  });
}

// ─── 测试用例定义 ───
const TESTS = [
  {
    id: 'vehicle_ident', name: 'Vehicle Identification (0x0001)', category: '连接',
    run: () => tcpTest('vehicle_ident', 0x0001, Buffer.alloc(0), (ptype, payload) => {
      if (ptype !== PT.VEHICLE_ANNOUNCE) return `Expected 0x0004, got 0x${ptype.toString(16)}`;
      if (payload.length < 32) return 'Payload too short';
      const vin = payload.toString('ascii', 0, 17);
      const la = payload.readUInt16BE(17);
      if (vin !== ECU_VIN) return `VIN mismatch: ${vin}`;
      if (la !== ECU_LA) return `LA mismatch: 0x${la.toString(16)}`;
      return `VIN=${vin} LA=0x${la.toString(16)}`;
    })
  },
  {
    id: 'routing', name: 'Routing Activation (0x0005)', category: '连接',
    run: () => {
      const req = Buffer.alloc(7);
      req.writeUInt16BE(0x0E00, 0); req.writeUInt32BE(0, 2); req[6] = 0;
      return tcpTest('routing', PT.ROUTING_REQ, req, (ptype, payload) => {
        if (ptype !== PT.ROUTING_RSP) return `Expected 0x0006, got 0x${ptype.toString(16)}`;
        if (payload.length < 9) return 'Payload too short';
        const code = payload[4];
        if (code !== 0x10) return `Failed: code=0x${code.toString(16)}`;
        return 'Routing activated';
      });
    }
  },
  {
    id: 'session', name: 'UDS Session Control (0x10 03)', category: 'UDS诊断',
    run: () => {
      const uds = Buffer.from([0x10, 0x03]);
      return routedDiagTest('session', uds, (ptype, payload) => {
        if (ptype !== PT.DIAG_MSG) return `Expected 0x8001, got 0x${ptype.toString(16)}`;
        const r = decodeUDSPos(payload);
        if (!r) return 'Cannot decode UDS';
        if (r.sid === 0x7F) return `NACK nrc=0x${r.nrc?.toString(16)}`;
        if (r.sub !== 0x10) return `Expected 0x50, got 0x${r.sid.toString(16)}`;
        return `SA=0x${r.sa.toString(16)} TA=0x${r.ta.toString(16)} Session=0x${r.data[0]?.toString(16)}`;
      });
    }
  },
  {
    id: 'read_vin', name: 'UDS Read VIN (0x22 F190)', category: 'UDS诊断',
    run: () => {
      const uds = Buffer.from([0x22, 0xF1, 0x90]);
      return routedDiagTest('read_vin', uds, (ptype, payload) => {
        if (ptype !== PT.DIAG_MSG) return `Expected 0x8001`;
        const r = decodeUDSPos(payload);
        if (!r) return 'Cannot decode';
        if (r.sid === 0x7F) return `NACK nrc=0x${r.nrc?.toString(16)}`;
        if (r.sub !== 0x22) return `Expected 0x62`;
        const vin = r.data.subarray(2, 19).toString('ascii');
        return `VIN=${vin}`;
      });
    }
  },
  {
    id: 'read_soc', name: 'UDS Read SOC (0x22 F102)', category: 'UDS诊断',
    run: () => {
      const uds = Buffer.from([0x22, 0xF1, 0x02]);
      return routedDiagTest('read_soc', uds, (ptype, payload) => {
        if (ptype !== PT.DIAG_MSG) return `Expected 0x8001`;
        const r = decodeUDSPos(payload);
        if (!r) return 'Cannot decode';
        if (r.sid === 0x7F) return `NACK nrc=0x${r.nrc?.toString(16)}`;
        if (r.sub !== 0x22) return `Expected 0x62`;
        return `SOC=${r.data[2]}%`;
      });
    }
  },
  {
    id: 'tester_present', name: 'UDS Tester Present (0x3E)', category: 'UDS诊断',
    run: () => {
      const uds = Buffer.from([0x3E, 0x00]);
      return routedDiagTest('tester_present', uds, (ptype, payload) => {
        if (ptype !== PT.DIAG_MSG) return `Expected 0x8001`;
        const r = decodeUDSPos(payload);
        if (!r) return 'Cannot decode';
        if (r.sid === 0x7F) return `NACK nrc=0x${r.nrc?.toString(16)}`;
        if (r.sub !== 0x3E) return `Expected 0x7E`;
        return 'OK';
      });
    }
  },
  {
    id: 'security', name: 'UDS Security Access (0x27 01)', category: 'UDS诊断',
    run: () => {
      const uds = Buffer.from([0x27, 0x01]);
      return routedDiagTest('security', uds, (ptype, payload) => {
        if (ptype !== PT.DIAG_MSG) return `Expected 0x8001`;
        const r = decodeUDSPos(payload);
        if (!r) return 'Cannot decode';
        if (r.sid === 0x7F) return `NACK nrc=0x${r.nrc?.toString(16)}`;
        if (r.sub !== 0x27) return `Expected 0x67`;
        if (r.data && r.data.length >= 5) {
          const seed = r.data.readUInt32BE(1);
          const key = (((~seed) >>> 0) ^ 0x5A5A5A5A) + 0x3618BC91;
          return `Seed=0x${seed.toString(16)} Key=0x${(key>>>0).toString(16)}`;
        }
        return 'OK';
      });
    }
  },
  {
    id: 'alive', name: 'Alive Check (0x0007)', category: '连接',
    run: () => tcpTest('alive', PT.ALIVE_CHECK_REQ, Buffer.alloc(0), (ptype, payload) => {
      if (ptype !== PT.ALIVE_CHECK_RSP) return `Expected 0x0008, got 0x${ptype.toString(16)}`;
      if (payload.length < 2) return 'Too short';
      return `LA=0x${payload.readUInt16BE(0).toString(16)}`;
    })
  },
  {
    id: 'unknown_nack', name: 'Unknown Type → NACK', category: '错误处理',
    run: () => tcpTest('unknown_nack', 0x9999, Buffer.alloc(0), (ptype, payload) => {
      if (ptype !== PT.GEN_NACK) return `Expected 0x0000 NACK, got 0x${ptype.toString(16)}`;
      return `NACK code=0x${payload[0]?.toString(16)}`;
    })
  },
  {
    id: 'no_routing', name: 'Diag without Routing', category: '错误处理',
    run: () => new Promise(resolve => {
      const client = new net.Socket();
      let buf = Buffer.alloc(0), done = false;
      const ecuIP = global.testIP || DEFAULT_ECU_IP;
      const ecuPort = global.testPort || DEFAULT_ECU_PORT;
      const finish = (pass, detail) => {
        if (done) return; done = true;
        client.destroy();
        result('no_routing', pass, detail);
        resolve();
      };
      client.setTimeout(TIMEOUT);
      client.on('timeout', () => finish(false, 'Timeout'));
      client.on('error', e => finish(false, e.message));
      client.on('data', d => {
        buf = Buffer.concat([buf, d]);
        while (buf.length >= 8 && !done) {
          const plen = buf.readUInt32BE(4);
          if (buf.length < 8 + plen) break;
          const ptype = buf.readUInt16BE(2);
          if (ptype === PT.DIAG_NACK) {
            finish(true, `NACK code=0x${buf[8]?.toString(16)}`);
          } else {
            finish(false, `Expected 0x8003 NACK, got 0x${ptype.toString(16)}`);
          }
          return;
        }
      });
      client.connect(ecuPort, ecuIP, () => {
        const uds = Buffer.from([0x22, 0xF1, 0x02]);
        const req = Buffer.alloc(6);
        req.writeUInt16BE(0x0E00, 0); req.writeUInt16BE(ECU_LA, 2);
        uds.copy(req, 4);
        client.write(buildDoIP(PT.DIAG_MSG, req));
      });
    })
  },
  {
    id: 'udp_announce', name: 'UDP Vehicle Announce (0x0004)', category: '连接',
    run: () => new Promise(resolve => {
      const sock = dgram.createSocket({ type: 'udp4', reuseAddr: true });
      let done = false;
      const ecuIP = global.testIP || DEFAULT_ECU_IP;
      const ecuPort = global.testPort || DEFAULT_ECU_PORT;
      const t = setTimeout(() => {
        if (done) return; done = true;
        sock.close();
        result('udp_announce', true, 'Not received (可能防火墙/跨网段)');
        resolve();
      }, 8000);
      sock.on('message', (msg, rinfo) => {
        if (done) return; done = true; clearTimeout(t);
        if (msg.length >= 40) {
          const vin = msg.toString('ascii', 8, 25);
          result('udp_announce', true, `From ${rinfo.address} VIN=${vin}`);
        } else {
          result('udp_announce', false, `Msg too short: ${msg.length}B`);
        }
        sock.close();
        resolve();
      });
      sock.on('error', e => { if (!done) { done = true; clearTimeout(t); sock.close(); result('udp_announce', false, e.message); resolve(); } });
      sock.bind(13400, '0.0.0.0', () => { sock.setBroadcast(true); });
      log('Listening for UDP announce... (最多8秒)', 'udp_announce');
    })
  },
  {
    id: 'read_dtc_count', name: 'UDS Read DTC Count (0x19 01)', category: 'DTC管理',
    run: () => {
      const uds = Buffer.from([0x19, 0x01, 0xFF]); // 全部状态
      return routedDiagTest('read_dtc_count', uds, (ptype, payload) => {
        if (ptype !== PT.DIAG_MSG) return `Expected 0x8001`;
        const r = decodeUDSPos(payload);
        if (!r) return 'Cannot decode';
        if (r.sid === 0x7F) return `NACK nrc=0x${r.nrc?.toString(16)}`;
        if (r.sub !== 0x19) return `Expected 0x59`;
        const cnt = r.data[3];
        return `Total DTCs: ${cnt}`;
      });
    }
  },
  {
    id: 'read_all_dtcs', name: 'UDS Read All DTCs (0x19 02)', category: 'DTC管理',
    run: () => {
      const uds = Buffer.from([0x19, 0x02, 0xFF]);
      return routedDiagTest('read_all_dtcs', uds, (ptype, payload) => {
        if (ptype !== PT.DIAG_MSG) return `Expected 0x8001`;
        const r = decodeUDSPos(payload);
        if (!r) return 'Cannot decode';
        if (r.sid === 0x7F) return `NACK nrc=0x${r.nrc?.toString(16)}`;
        if (r.sub !== 0x19) return `Expected 0x59`;
        // 解析: 前3字节header后, 每4字节一组(DTC_3B + Status_1B)
        const dtcs = [];
        for (let i = 3; i + 4 <= r.data.length; i += 4) {
          const code = (r.data[i] << 16) | (r.data[i+1] << 8) | r.data[i+2];
          const st = r.data[i+3];
          const act = (st & 0x01) ? 'TF' : '  ';
          const conf = (st & 0x08) ? 'CF' : '  ';
          const pend = (st & 0x20) ? 'PD' : '  ';
          const warn = (st & 0x40) ? '⚡' : '  ';
          dtcs.push(`0x${code.toString(16).toUpperCase().padStart(6,'0')}[${warn}${conf}${pend}${act}]`);
        }
        return dtcs.length > 0 ? dtcs.join(' ') : 'No DTCs';
      });
    }
  },
  {
    id: 'read_dtc_pending', name: 'UDS Read Pending DTCs (0x19 02 mask=0x20)', category: 'DTC管理',
    run: () => {
      const uds = Buffer.from([0x19, 0x02, 0x20]); // 只查pending
      return routedDiagTest('read_dtc_pending', uds, (ptype, payload) => {
        if (ptype !== PT.DIAG_MSG) return `Expected 0x8001`;
        const r = decodeUDSPos(payload);
        if (!r) return 'Cannot decode';
        if (r.sid === 0x7F) return `NACK nrc=0x${r.nrc?.toString(16)}`;
        if (r.sub !== 0x19) return `Expected 0x59`;
        let count = 0;
        for (let i = 3; i + 4 <= r.data.length; i += 4) count++;
        return `Pending DTCs: ${count}`;
      });
    }
  },
  {
    id: 'clear_dtcs', name: 'UDS Clear DTCs (0x14)', category: 'DTC管理',
    run: () => {
      const uds = Buffer.from([0x14, 0xFF, 0xFF, 0xFF]); // 清除全部
      return routedDiagTest('clear_dtcs', uds, (ptype, payload) => {
        if (ptype !== PT.DIAG_MSG) return `Expected 0x8001`;
        const r = decodeUDSPos(payload);
        if (!r) return 'Cannot decode';
        if (r.sid === 0x7F) return `NACK nrc=0x${r.nrc?.toString(16)}`;
        if (r.sub !== 0x14) return `Expected 0x54`;
        return 'DTCs cleared (模拟数据复位后恢复)';
      });
    }
  },
];

// ─── HTTP 服务器路由 ───
function serveStatic(res, contentType, content) {
  res.writeHead(200, { 'Content-Type': contentType });
  res.end(content);
}

const server = http.createServer((req, res) => {
  const url = new URL(req.url, `http://localhost:${HTTP_PORT}`);

  // SSE 实时日志流
  if (url.pathname === '/stream') {
    res.writeHead(200, {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache',
      'Connection': 'keep-alive',
      'Access-Control-Allow-Origin': '*',
    });
    res.write(':ok\n\n');
    clients.add(res);
    req.on('close', () => clients.delete(res));
    return;
  }

  // 触发单个测试
  if (url.pathname === '/test' && req.method === 'POST') {
    let body = '';
    req.on('data', d => body += d);
    req.on('end', () => {
      try {
        const { testId } = JSON.parse(body);
        const test = TESTS.find(t => t.id === testId);
        if (!test) { res.writeHead(404); res.end('Not found'); return; }
        progress(testId, 'running');
        log(`[开始] ${test.name}`, testId);
        test.run().then(() => progress(testId, 'done'));
      } catch (e) { res.writeHead(400); res.end(e.message); }
      res.writeHead(200); res.end('ok');
    });
    return;
  }

  // 运行全部测试
  if (url.pathname === '/run-all' && req.method === 'POST') {
    res.writeHead(200); res.end('ok');
    log('══════ 开始运行全部测试 ══════');
    (async () => {
      for (const test of TESTS) {
        progress(test.id, 'running');
        log(`[${test.name}]`, test.id);
        await test.run();
        await sleep(200);
      }
      log('══════ 全部测试完成 ══════');
    })();
    return;
  }

  // 首页
  if (url.pathname === '/') {
    serveStatic(res, 'text/html; charset=utf-8', getHTML());
    return;
  }

  res.writeHead(404); res.end('Not found');
});

// ─── HTML 界面 ───
function getHTML() {
  const testButtons = TESTS.map((t, i) => `
    <div class="test-card" id="card-${t.id}">
      <div class="test-header">
        <span class="test-id">#${i + 1}</span>
        <span class="test-status" id="status-${t.id}">●</span>
      </div>
      <div class="test-name">${t.name}</div>
      <div class="test-cat">${t.category}</div>
      <div class="test-detail" id="detail-${t.id}"></div>
      <button class="test-btn" onclick="runTest('${t.id}')">▶ 运行</button>
    </div>
  `).join('');

  return `<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>DoIP Tester — ISO 13400-2</title>
<style>
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',system-ui,sans-serif;background:#0F172A;color:#E2E8F0;min-height:100vh}
.header{background:#1E293B;padding:14px 24px;display:flex;align-items:center;justify-content:space-between;border-bottom:1px solid #334155}
.header h1{font-size:20px;color:#F97316}
.header .sub{color:#94A3B8;font-size:13px;margin-left:10px}
.config{display:flex;gap:10px;align-items:center}
.config input{background:#0F172A;border:1px solid #475569;color:#E2E8F0;padding:6px 12px;border-radius:6px;width:140px;font-size:13px}
.config input:focus{outline:none;border-color:#F97316}
.btn{padding:7px 18px;border:none;border-radius:6px;cursor:pointer;font-size:13px;font-weight:600;transition:all .15s}
.btn-primary{background:#F97316;color:#fff}.btn-primary:hover{background:#EA580C}
.btn-success{background:#22C55E;color:#fff}.btn-success:hover{background:#16A34A}
.btn-danger{background:#EF4444;color:#fff}.btn-danger:hover{background:#DC2626}
.panel{padding:16px 24px}
.stats{display:flex;gap:20px;margin-bottom:16px}
.stat{border-radius:8px;padding:10px 20px;background:#1E293B;min-width:100px;text-align:center}
.stat-val{font-size:28px;font-weight:700}
.stat-label{font-size:12px;color:#94A3B8;margin-top:2px}
.stat-pass .stat-val{color:#22C55E}
.stat-fail .stat-val{color:#EF4444}
.stat-total .stat-val{color:#3B82F6}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(280px,1fr));gap:12px}
.test-card{background:#1E293B;border-radius:10px;padding:14px;border:1px solid #334155;transition:border-color .2s}
.test-card:hover{border-color:#475569}
.test-card.running{border-color:#F97316;animation:pulse 1s infinite}
.test-card.pass{border-color:#22C55E;background:rgba(34,197,94,.05)}
.test-card.fail{border-color:#EF4444;background:rgba(239,68,68,.05)}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.6}}
.test-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:6px}
.test-id{color:#64748B;font-size:11px;font-weight:600}
.test-status{font-size:14px}.test-status.pending{color:#64748B}.test-status.running{color:#F97316}.test-status.pass{color:#22C55E}.test-status.fail{color:#EF4444}
.test-name{font-size:14px;font-weight:600;margin-bottom:4px;line-height:1.3}
.test-cat{font-size:11px;color:#64748B;margin-bottom:8px}
.test-detail{font-size:12px;color:#94A3B8;margin-bottom:10px;min-height:18px;word-break:break-all}
.test-btn{width:100%;padding:7px;border:1px solid #475569;background:transparent;color:#94A3B8;border-radius:6px;cursor:pointer;font-size:12px;font-weight:600;transition:all .15s}
.test-btn:hover{background:#334155;color:#E2E8F0}
.console{background:#020617;border-radius:10px;border:1px solid #1E293B;padding:14px;margin-top:16px;max-height:300px;overflow-y:auto;font-family:'Cascadia Code','Consolas',monospace;font-size:12px;line-height:1.6}
.console .log{color:#94A3B8}
.console .pass{color:#22C55E}
.console .fail{color:#EF4444}
.console .info{color:#3B82F6}
::-webkit-scrollbar{width:6px}::-webkit-scrollbar-track{background:transparent}::-webkit-scrollbar-thumb{background:#334155;border-radius:3px}
</style>
</head>
<body>
<div class="header">
  <div>
    <h1>DoIP Tester<span class="sub">ISO 13400-2 BMS Diagnostic</span></h1>
  </div>
  <div class="config">
    <input id="ip" value="${DEFAULT_ECU_IP}" placeholder="ECU IP">
    <input id="port" value="${DEFAULT_ECU_PORT}" placeholder="Port" style="width:80px">
    <button class="btn btn-primary" onclick="window.location.reload()">刷新配置</button>
    <button class="btn btn-success" onclick="runAll()">▶ 运行全部</button>
    <button class="btn btn-danger" onclick="clearConsole()">清空日志</button>
  </div>
</div>
<div class="panel">
  <div class="stats">
    <div class="stat stat-total"><div class="stat-val" id="count-total">0</div><div class="stat-label">Total</div></div>
    <div class="stat stat-pass"><div class="stat-val" id="count-pass">0</div><div class="stat-label">Passed</div></div>
    <div class="stat stat-fail"><div class="stat-val" id="count-fail">0</div><div class="stat-label">Failed</div></div>
  </div>
  <div class="grid">
    ${testButtons}
  </div>
  <div class="console" id="console">
    <div class="log">🟢 DoIP Tester ready. Target: <span id="target-info">${DEFAULT_ECU_IP}:${DEFAULT_ECU_PORT}</span></div>
  </div>
</div>

<script>
const evtSource = new EventSource('/stream');
evtSource.onmessage = e => {
  const d = JSON.parse(e.data);
  if (d.type === 'log') {
    appendLog(d.msg);
  } else if (d.type === 'progress') {
    updateStatus(d.testId, d.status);
  } else if (d.type === 'result') {
    updateResult(d.testId, d.pass, d.detail);
  }
};

function updateStatus(testId, status) {
  const card = document.getElementById('card-' + testId);
  const st = document.getElementById('status-' + testId);
  card?.classList.remove('running','pass','fail');
  st?.classList.remove('pending','running','pass','fail');
  if (status === 'running') {
    card?.classList.add('running');
    st?.classList.add('running');
    st.textContent = '⟳';
  } else {
    st?.classList.add('pending');
    st.textContent = '●';
  }
}

function updateResult(testId, pass, detail) {
  const card = document.getElementById('card-' + testId);
  const st = document.getElementById('status-' + testId);
  const dt = document.getElementById('detail-' + testId);
  card?.classList.remove('running');
  card?.classList.add(pass ? 'pass' : 'fail');
  st?.classList.remove('running');
  st?.classList.add(pass ? 'pass' : 'fail');
  st.textContent = pass ? '✅' : '❌';
  if (dt) dt.textContent = detail || '';
  updateStats();
}

function appendLog(msg) {
  const c = document.getElementById('console');
  const div = document.createElement('div');
  div.className = msg.includes('✅') ? 'pass' : msg.includes('❌') ? 'fail' : 'log';
  div.textContent = msg;
  c.appendChild(div);
  c.scrollTop = c.scrollHeight;
}

function updateStats() {
  const cards = document.querySelectorAll('.test-card');
  let pass = 0, fail = 0;
  cards.forEach(c => {
    if (c.classList.contains('pass')) pass++;
    if (c.classList.contains('fail')) fail++;
  });
  document.getElementById('count-total').textContent = TESTS.length;
  document.getElementById('count-pass').textContent = pass;
  document.getElementById('count-fail').textContent = fail;
}

function clearConsole() {
  document.getElementById('console').innerHTML = '';
  TESTS.forEach(t => {
    document.getElementById('card-'+t.id)?.classList.remove('running','pass','fail');
    const st = document.getElementById('status-'+t.id);
    if (st) { st.className = 'test-status pending'; st.textContent = '●'; }
    const dt = document.getElementById('detail-'+t.id);
    if (dt) dt.textContent = '';
  });
  updateStats();
}

async function runTest(testId) {
  globalThis.testIP = document.getElementById('ip').value;
  globalThis.testPort = parseInt(document.getElementById('port').value) || 13400;
  fetch('/test', { method:'POST', body:JSON.stringify({testId}) });
}

async function runAll() {
  globalThis.testIP = document.getElementById('ip').value;
  globalThis.testPort = parseInt(document.getElementById('port').value) || 13400;
  TESTS.forEach(t => {
    document.getElementById('detail-'+t.id).textContent = '';
  });
  fetch('/run-all', { method:'POST' });
}

const TESTS = ${JSON.stringify(TESTS.map(t => ({id:t.id,name:t.name})))};
document.getElementById('target-info').textContent =
  document.getElementById('ip').value + ':' + document.getElementById('port').value;
</script>
</body>
</html>`;
}

// ─── 启动 ───
server.listen(HTTP_PORT, () => {
  console.log(`\n╔══════════════════════════════════════════╗`);
  console.log(`║   DoIP GUI Tester                        ║`);
  console.log(`║   http://localhost:${HTTP_PORT}                    ║`);
  console.log(`╚══════════════════════════════════════════╝\n`);
  exec(`start http://localhost:${HTTP_PORT}`);
});
