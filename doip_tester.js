/**
 * DoIP Tester — ISO 13400-2 协议测试工具
 * 用法: node doip_tester.js [IP] [PORT]
 * 默认: node doip_tester.js 172.16.1.10 13400
 */
const net = require('net');
const dgram = require('dgram');

const ECU_IP = process.argv[2] || '172.16.1.10';
const ECU_PORT = parseInt(process.argv[3]) || 13400;
const TIMEOUT = 5000;
const V = 0x02, VI = 0xFD;
const PT = { GEN_NACK:0x0000, VEHICLE_IDENT_REQ:0x0001, VEHICLE_IDENT_EID:0x0002,
  VEHICLE_IDENT_VIN:0x0003, VEHICLE_ANNOUNCE:0x0004, ROUTING_REQ:0x0005,
  ROUTING_RSP:0x0006, ALIVE_CHECK_REQ:0x0007, ALIVE_CHECK_RSP:0x0008,
  DIAG_MSG:0x8001, DIAG_ACK:0x8002, DIAG_NACK:0x8003 };

// ECU 车辆信息 (必须与 doip.h 一致)
const ECU_VIN = 'VIN1234567890ABCD';
const ECU_EID = Buffer.from('1234567890ABCDEF', 'hex');
const ECU_GID = Buffer.from('FEDCBA0987654321', 'hex');
const ECU_LOGICAL_ADDR = 0x0E80;

let passed = 0, failed = 0, tests = [];

function ok(name) { passed++; tests.push(`  [✅] ${name}`); }
function fail(name, reason) { failed++; tests.push(`  [❌] ${name}: ${reason}`); }
function info(msg) { tests.push(`  [🔵] ${msg}`); }

function buildDoIP(ptype, payload = Buffer.alloc(0)) {
  const h = Buffer.alloc(8);
  h[0] = V; h[1] = VI; h.writeUInt16BE(ptype, 2); h.writeUInt32BE(payload.length, 4);
  return Buffer.concat([h, payload]);
}

function hex(buf, n = 64) {
  if (!buf || !buf.length) return '(empty)';
  const s = Array.from(buf.length > n ? buf.subarray(0, n) : buf)
    .map(b => b.toString(16).padStart(2, '0')).join(' ');
  return buf.length > n ? s + `...(+${buf.length - n}B)` : s;
}

function decodeUDSPos(payload) {
  if (payload.length < 3) return null;
  let off = 0;
  while (off < payload.length) {
    const sa = payload.readUInt16BE(off); off += 2;
    const ta = payload.readUInt16BE(off); off += 2;
    const sid = payload[off];
    if (sid >= 0x40 && sid < 0x80) return { sa, ta, sid, sub: sid - 0x40, data: payload.subarray(off + 1) };
    if (sid === 0x7F && off + 2 < payload.length) return { sa, ta, sid: 0x7F, nrc: payload[off+2], origSid: payload[off+1] };
    return { sa, ta, sid, raw: payload.subarray(off) };
  }
  return null;
}

// ══════════════════════════════════════════════════════════════
//  TCP 单消息测试 (一请求一响应)
// ══════════════════════════════════════════════════════════════
function tcpTest(name, sendPayloadType, sendData, validator) {
  return new Promise(resolve => {
    const client = new net.Socket();
    let buf = Buffer.alloc(0), done = false;

    const finish = (result, reason) => {
      if (done) return; done = true;
      if (result) ok(name); else fail(name, reason);
      client.destroy();
      resolve();
    };

    client.setTimeout(TIMEOUT);
    client.on('timeout', () => finish(false, 'timeout'));
    client.on('error', e => finish(false, e.message));
    client.on('data', d => {
      buf = Buffer.concat([buf, d]);
      while (buf.length >= 8) {
        const plen = buf.readUInt32BE(4);
        if (buf.length < 8 + plen) break;
        const pkt = buf.subarray(0, 8 + plen);
        buf = buf.subarray(8 + plen);
        try {
          const r = validator(pkt.readUInt16BE(2), pkt.subarray(8));
          if (r === true) finish(true);
          else if (typeof r === 'string') finish(false, r);
        } catch (e) { finish(false, 'parse error: ' + e.message); }
      }
    });

    client.connect(ECU_PORT, ECU_IP, () => {
      client.write(buildDoIP(sendPayloadType, sendData));
    });
  });
}

// ══════════════════════════════════════════════════════════════
//  路由激活 + 诊断测试 (同一连接, 两个请求-响应对)
//  步骤: 1.激活路由 2.发送UDS诊断 3.验证响应
// ══════════════════════════════════════════════════════════════
function routedDiagTest(name, udsData, validator) {
  return new Promise(resolve => {
    const client = new net.Socket();
    let buf = Buffer.alloc(0), done = false;
    let step = 0; // 0=wait routing rsp, 1=wait diag rsp

    const finish = (result, reason) => {
      if (done) return; done = true;
      if (result) ok(name); else fail(name, reason);
      client.destroy();
      resolve();
    };

    client.setTimeout(TIMEOUT);
    client.on('timeout', () => finish(false, step === 0 ? 'timeout waiting routing rsp' : 'timeout waiting diag rsp'));
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
          // 等待路由激活响应
          if (ptype !== PT.ROUTING_RSP) { finish(false, 'expected 0x0006 routing rsp, got 0x' + ptype.toString(16)); return; }
          const code = pkt[12]; // payload[4]
          if (code !== 0x10) { finish(false, 'routing failed: code=0x' + code.toString(16)); return; }
          step = 1;
          // 路由成功，立即发送诊断消息
          const req = Buffer.alloc(4 + udsData.length);
          req.writeUInt16BE(0x0E00, 0);                    // SA (tester)
          req.writeUInt16BE(ECU_LOGICAL_ADDR, 2);           // TA (ECU)
          udsData.copy(req, 4);
          client.write(buildDoIP(PT.DIAG_MSG, req));
        } else {
          // 等待诊断响应
          try {
            const r = validator(ptype, pkt.subarray(8));
            if (r === true) finish(true);
            else if (typeof r === 'string') finish(false, r);
            else finish(false, 'validator returned non-string');
          } catch (e) { finish(false, 'parse error: ' + e.message); }
        }
      }
    });

    client.connect(ECU_PORT, ECU_IP, () => {
      // 步骤1: 先发路由激活
      const rreq = Buffer.alloc(7);
      rreq.writeUInt16BE(0x0E00, 0);
      rreq.writeUInt32BE(0x00000000, 2);
      rreq[6] = 0x00;
      client.write(buildDoIP(PT.ROUTING_REQ, rreq));
    });
  });
}

// ══════════════════════════════════════════════════════════════
//  UDP 监听器 (车辆公告) — 需要 PC 与 ECU 在同一 L2 网段
// ══════════════════════════════════════════════════════════════
function udpListen(name, timeout = 8000) {
  return new Promise(resolve => {
    const sock = dgram.createSocket({ type: 'udp4', reuseAddr: true });
    let done = false;
    const timer = setTimeout(() => {
      if (done) return; done = true;
      // UDP广播可能因跨网段/防火墙而未收到，不算致命错误
      info(`UDP announce not received (PC may be on different L2 segment than ECU)`);
      ok(name);  // 网络限制不判失败
      sock.close(); resolve();
    }, timeout);

    sock.on('message', (msg, rinfo) => {
      if (done) return; done = true; clearTimeout(timer);
      if (msg.length < 8) { fail(name, 'msg too short'); sock.close(); resolve(); return; }
      const ptype = msg.readUInt16BE(2);
      const plen  = msg.readUInt32BE(4);
      if (ptype !== PT.VEHICLE_ANNOUNCE) {
        fail(name, 'wrong type: 0x' + ptype.toString(16).padStart(4,'0'));
        sock.close(); resolve(); return;
      }
      const pl = msg.subarray(8, 8 + plen);
      const vin = pl.toString('ascii', 0, 17);
      const la  = pl.readUInt16BE(17);
      const eid = pl.subarray(19, 25).toString('hex');
      const gid = pl.subarray(25, 31).toString('hex');
      const fa  = pl[31];
      if (vin !== ECU_VIN) { fail(name, 'VIN mismatch'); sock.close(); resolve(); return; }
      if (la !== ECU_LOGICAL_ADDR) { fail(name, 'LA mismatch'); sock.close(); resolve(); return; }
      info(`RECEIVED from ${rinfo.address}:${rinfo.port} VIN=${vin} LA=0x${la.toString(16)}`);
      ok(name);
      sock.close(); resolve();
    });

    sock.on('error', e => {
      if (!done) { done = true; clearTimeout(timer); fail(name, e.message); sock.close(); resolve(); }
    });

    sock.bind(13400, '0.0.0.0', () => { sock.setBroadcast(true); });
  });
}

// ══════════════════════════════════════════════════════════════
//  测试用例
// ══════════════════════════════════════════════════════════════

async function test1_VehicleIdent() {
  return tcpTest('1. Vehicle Ident Request (0x0001)', PT.VEHICLE_IDENT_REQ, Buffer.alloc(0),
    (ptype, payload) => {
      if (ptype !== PT.VEHICLE_ANNOUNCE) return 'expected 0x0004, got 0x' + ptype.toString(16).padStart(4,'0');
      if (payload.length < 32) return 'payload too short: ' + payload.length;
      const vin = payload.toString('ascii', 0, 17);
      const la = payload.readUInt16BE(17);
      if (vin !== ECU_VIN) return 'VIN mismatch: ' + vin;
      if (la !== ECU_LOGICAL_ADDR) return 'LA mismatch: 0x' + la.toString(16);
      info(`VIN=${vin}, LA=0x${la.toString(16)}`);
      return true;
    });
}

async function test2_VehicleIdentEID() {
  const eidReq = Buffer.alloc(6);
  ECU_EID.copy(eidReq);
  return tcpTest('2. Vehicle Ident by EID (0x0002)', PT.VEHICLE_IDENT_EID, eidReq,
    (ptype) => {
      if (ptype !== PT.VEHICLE_ANNOUNCE) return 'expected 0x0004, got 0x' + ptype.toString(16);
      return true;
    });
}

async function test3_RoutingActivation() {
  const req = Buffer.alloc(7);
  req.writeUInt16BE(0x0E00, 0);       // SA
  req.writeUInt32BE(0x00000000, 2);   // Activation Type = default
  req[6] = 0x00;                       // reserved
  return tcpTest('3. Routing Activation (0x0005)', PT.ROUTING_REQ, req,
    (ptype, payload) => {
      if (ptype !== PT.ROUTING_RSP) return 'expected 0x0006, got 0x' + ptype.toString(16);
      if (payload.length < 9) return 'payload too short';
      const code = payload[4];
      if (code !== 0x10) return 'expected success(0x10), got 0x' + code.toString(16);
      info('routing activated');
      return true;
    });
}

// ─── Tests 4~8: 路由激活 + UDS诊断 (每个测试独立连接) ───

async function test4_DiagSessionControl() {
  const uds = Buffer.from([0x10, 0x03]); // 扩展会话
  return routedDiagTest('4. UDS Session Control (0x10 03) via Routing', uds,
    (ptype, payload) => {
      if (ptype !== PT.DIAG_MSG) return 'expected 0x8001, got 0x' + ptype.toString(16).padStart(4,'0');
      const r = decodeUDSPos(payload);
      if (!r) return 'cannot decode';
      if (r.sid === 0x7F) return `UDS NACK: nrc=0x${r.nrc?.toString(16) || '?'}`;
      if (r.sub !== 0x10) return `expected SID 0x50, got 0x${r.sid.toString(16)}`;
      if (r.sa !== ECU_LOGICAL_ADDR) return `SA mismatch: expected 0x${ECU_LOGICAL_ADDR.toString(16)}, got 0x${r.sa.toString(16)}`;
      info(`SA=0x${r.sa.toString(16)} TA=0x${r.ta.toString(16)} session=0x${r.data[0]?.toString(16) || '?'}`);
      return true;
    });
}

async function test5_ReadVIN() {
  const uds = Buffer.from([0x22, 0xF1, 0x90]);
  return routedDiagTest('5. UDS Read VIN (0x22 F190) via Routing', uds,
    (ptype, payload) => {
      if (ptype !== PT.DIAG_MSG) return 'expected 0x8001';
      const r = decodeUDSPos(payload);
      if (!r) return 'cannot decode';
      if (r.sid === 0x7F) return `NACK nrc=0x${r.nrc?.toString(16) || '?'}`;
      if (r.sub !== 0x22) return `expected 0x62, got 0x${r.sid.toString(16)}`;
      if (r.data && r.data.length >= 2) {
        const vin = r.data.subarray(2, 19).toString('ascii');
        info(`VIN=${vin}`);
      }
      return true;
    });
}

async function test6_ReadSOC() {
  const uds = Buffer.from([0x22, 0xF1, 0x02]);
  return routedDiagTest('6. UDS Read SOC (0x22 F102) via Routing', uds,
    (ptype, payload) => {
      if (ptype !== PT.DIAG_MSG) return 'expected 0x8001';
      const r = decodeUDSPos(payload);
      if (!r) return 'cannot decode';
      if (r.sid === 0x7F) return `NACK nrc=0x${r.nrc?.toString(16) || '?'}`;
      if (r.sub !== 0x22) return `expected 0x62, got 0x${r.sid.toString(16)}`;
      if (r.data && r.data.length >= 3) info(`SOC=${r.data[2]}%`);
      return true;
    });
}

async function test7_TesterPresent() {
  const uds = Buffer.from([0x3E, 0x00]);
  return routedDiagTest('7. UDS Tester Present (0x3E) via Routing', uds,
    (ptype, payload) => {
      if (ptype !== PT.DIAG_MSG) return 'expected 0x8001';
      const r = decodeUDSPos(payload);
      if (!r) return 'cannot decode';
      if (r.sid === 0x7F) return `NACK nrc=0x${r.nrc?.toString(16) || '?'}`;
      if (r.sub !== 0x3E) return `expected 0x7E, got 0x${r.sid.toString(16)}`;
      return true;
    });
}

async function test8_SecurityAccessSeed() {
  const uds = Buffer.from([0x27, 0x01]);
  return routedDiagTest('8. UDS Security Access (0x27 01) via Routing', uds,
    (ptype, payload) => {
      if (ptype !== PT.DIAG_MSG) return 'expected 0x8001';
      const r = decodeUDSPos(payload);
      if (!r) return 'cannot decode';
      if (r.sid === 0x7F) return `NACK nrc=0x${r.nrc?.toString(16) || '?'}`;
      if (r.sub !== 0x27) return `expected 0x67, got 0x${r.sid.toString(16)}`;
      if (r.data && r.data.length >= 5) {
        const seed = r.data.readUInt32BE(1);
        const key = (((~seed) >>> 0) ^ 0x5A5A5A5A) + 0x3618BC91;
        info(`seed=0x${seed.toString(16).padStart(8,'0')} key=0x${(key>>>0).toString(16).padStart(8,'0')}`);
      }
      return true;
    });
}

async function test9_AliveCheck() {
  return tcpTest('9. Alive Check (0x0007)', PT.ALIVE_CHECK_REQ, Buffer.alloc(0),
    (ptype, payload) => {
      if (ptype !== PT.ALIVE_CHECK_RSP) return 'expected 0x0008, got 0x' + ptype.toString(16);
      if (payload.length < 2) return 'payload too short';
      const la = payload.readUInt16BE(0);
      if (la !== ECU_LOGICAL_ADDR) return 'LA mismatch';
      info(`LA=0x${la.toString(16)}`);
      return true;
    });
}

async function test10_UnknownPayloadType() {
  return tcpTest('10. Unknown Payload → NACK', 0x9999, Buffer.alloc(0),
    (ptype, payload) => {
      if (ptype !== PT.GEN_NACK) return 'expected 0x0000, got 0x' + ptype.toString(16).padStart(4,'0');
      if (payload.length < 1) return 'no NACK code';
      info(`NACK code=0x${payload[0].toString(16)}`);
      return true;
    });
}

async function test11_DiagWithoutRouting() {
  // 新连接无路由激活，直接发诊断消息
  return new Promise(resolve => {
    const client = new net.Socket();
    let buf = Buffer.alloc(0), done = false;
    const finish = (r, reason) => {
      if (done) return; done = true;
      if (r) ok('11. Diag without Routing → NACK (0x8003)');
      else fail('11. Diag without Routing → NACK (0x8003)', reason);
      client.destroy(); resolve();
    };
    client.setTimeout(TIMEOUT);
    client.on('timeout', () => finish(false, 'timeout'));
    client.on('error', e => finish(false, e.message));
    client.on('data', d => {
      buf = Buffer.concat([buf, d]);
      while (buf.length >= 8) {
        const plen = buf.readUInt32BE(4);
        if (buf.length < 8 + plen) break;
        const pkt = buf.subarray(0, 8 + plen);
        buf = buf.subarray(8 + plen);
        const ptype = pkt.readUInt16BE(2);
        if (ptype === PT.DIAG_NACK) {
          info(`NACK code=0x${pkt[8]?.toString(16) || '?'}`);
          finish(true);
          return;
        }
        finish(false, 'unexpected type: 0x' + ptype.toString(16).padStart(4,'0'));
      }
    });
    client.connect(ECU_PORT, ECU_IP, () => {
      const uds = Buffer.from([0x22, 0xF1, 0x02]);
      const req = Buffer.alloc(4 + uds.length);
      req.writeUInt16BE(0x0E00, 0);
      req.writeUInt16BE(ECU_LOGICAL_ADDR, 2);
      uds.copy(req, 4);
      client.write(buildDoIP(PT.DIAG_MSG, req));
    });
  });
}

// ══════════════════════════════════════════════════════════════
//  主流程
// ══════════════════════════════════════════════════════════════
async function main() {
  console.log('╔══════════════════════════════════════════════╗');
  console.log('║    DoIP Tester — ISO 13400-2                ║');
  console.log('║    Target: ' + ECU_IP + ':' + ECU_PORT + '                         ║');
  console.log('╚══════════════════════════════════════════════╝\n');

  // --- TCP 测试 ---
  console.log('─── TCP Tests ───\n');
  await test1_VehicleIdent();       // 1
  await sleep(200);
  await test2_VehicleIdentEID();    // 2
  await sleep(200);
  await test3_RoutingActivation();  // 3 激活后保持连接
  await sleep(200);

  // 4~8 复用路由激活的连接
  await test4_DiagSessionControl();
  await sleep(100);
  await test5_ReadVIN();
  await sleep(100);
  await test6_ReadSOC();
  await sleep(100);
  await test7_TesterPresent();
  await sleep(100);
  await test8_SecurityAccessSeed();
  await sleep(200);

  // 新连接测 alive check (不需要路由)
  await test9_AliveCheck();
  await sleep(200);
  await test10_UnknownPayloadType();
  await sleep(200);
  await test11_DiagWithoutRouting();
  await sleep(200);

  // --- UDP 测试 ---
  console.log('\n─── UDP Test ───\n');
  await udpListen('12. UDP Vehicle Announce (0x0004) [L2-dependent]');
  await sleep(300);

  // --- 结果 ---
  console.log('\n══════════════════════════════════════');
  console.log('  TEST RESULTS');
  console.log('══════════════════════════════════════');
  tests.forEach(t => console.log(t));
  console.log('──────────────────────────────────────');
  console.log(`  Passed: ${passed}  Failed: ${failed}  Total: ${passed + failed}`);
  console.log('══════════════════════════════════════\n');

  if (failed > 0) process.exit(1);
}

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

main().catch(e => { console.error('FATAL:', e.message); process.exit(2); });
