#!/usr/bin/env node
/**
 * test_host_config - Self-contained API server + test client.
 *
 * Starts a simple HTTP server with GET/POST endpoints, runs connectivity
 * tests from multiple addresses, then shuts down.  Zero dependencies —
 * only Node.js built-ins.  Useful for diagnosing host/network issues on
 * WSL, Docker, or any environment before running the full gtopt webservice.
 *
 * Usage:  node test/test_host_config.js [--port PORT] [--hostname HOST]
 */

const http = require('http');
const dns  = require('dns');
const os   = require('os');

// ---- helpers --------------------------------------------------------

let pass = 0, fail = 0;
const ok   = (m) => { console.log(`  [PASS] ${m}`); pass++; };
const bad  = (m) => { console.log(`  [FAIL] ${m}`); fail++; };
const info = (m) => { console.log(`  [INFO] ${m}`); };
const sec  = (t) => { console.log(`\n--- ${t} ---`); };

// ---- server ---------------------------------------------------------

function startServer(hostname, port) {
  return new Promise((resolve, reject) => {
    const server = http.createServer((req, res) => {
      const json = (code, obj) => {
        res.writeHead(code, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify(obj));
      };
      if (req.url === '/api' && req.method === 'GET') {
        return json(200, { status: 'ok', method: 'GET' });
      }
      if (req.url === '/api' && req.method === 'POST') {
        let body = '';
        req.on('data', (c) => { body += c; });
        return req.on('end', () => json(200, { status: 'ok', method: 'POST', echo: body }));
      }
      if (req.url === '/api/ping') {
        return json(200, { status: 'ok', service: 'test-host-config' });
      }
      json(404, { error: 'not found' });
    });
    server.on('error', reject);
    server.listen(port, hostname, () => resolve(server));
  });
}

// ---- client ---------------------------------------------------------

function request(method, url, opts, body) {
  return new Promise((resolve) => {
    const options = { timeout: 3000, ...opts };
    const cb = (res) => {
      let d = '';
      res.on('data', (c) => { d += c; });
      res.on('end', () => {
        try   { resolve({ status: res.statusCode, body: JSON.parse(d) }); }
        catch { resolve({ status: res.statusCode, raw: d }); }
      });
    };
    let req;
    if (method === 'GET') {
      req = http.get(url, options, cb);
    } else {
      const u = new URL(url);
      req = http.request({ ...options, hostname: u.hostname, port: u.port,
        path: u.pathname, method, headers: { 'Content-Type': 'application/json' } }, cb);
      if (body) req.write(body);
      req.end();
    }
    req.on('error',   (e) => resolve({ error: e.message }));
    req.on('timeout', ()  => { req.destroy(); resolve({ error: 'timeout' }); });
  });
}

// ---- test runner ----------------------------------------------------

async function check(label, method, url, opts, body, expect) {
  const t0 = Date.now();
  const r  = await request(method, url, opts || {}, body);
  const ms = Date.now() - t0;
  if (r.error)           return bad(`${label}: ${r.error} (${ms}ms)`);
  if (!expect(r))        return bad(`${label}: unexpected status=${r.status} (${ms}ms)`);
  ok(`${label} (${ms}ms)`);
}

async function dnsInfo(host) {
  return new Promise((resolve) => {
    dns.lookup(host, { all: true }, (err, addrs) => {
      if (err) { info(`dns("${host}"): ${err.message}`); return resolve(); }
      info(`dns("${host}"): ${addrs.map((a) => `${a.address} (IPv${a.family})`).join(', ')}`);
      resolve();
    });
  });
}

// ---- main -----------------------------------------------------------

async function main() {
  // parse args
  const args = process.argv.slice(2);
  let port = 3111, hostname = '0.0.0.0';
  for (let i = 0; i < args.length; i++) {
    if (args[i] === '--port')     port = parseInt(args[++i], 10);
    if (args[i] === '--hostname') hostname = args[++i];
    if (args[i] === '-h' || args[i] === '--help') {
      console.log('Usage: node test/test_host_config.js [--port PORT] [--hostname HOST]');
      return;
    }
  }

  console.log('\n' + '='.repeat(50));
  console.log('  gtopt host configuration test');
  console.log('='.repeat(50));

  // environment
  sec('Environment');
  info(`Node ${process.version}  ${os.type()} ${os.release()}  ${process.arch}`);
  info(`HOSTNAME=${process.env.HOSTNAME || '(not set)'}  GTOPT_HOSTNAME=${process.env.GTOPT_HOSTNAME || '(not set)'}`);
  const wsl = process.env.WSL_DISTRO_NAME || process.env.WSLENV;
  if (wsl) info(`WSL: ${process.env.WSL_DISTRO_NAME || 'yes'}`);

  // dns
  sec('DNS resolution');
  for (const h of ['localhost', '127.0.0.1', os.hostname()]) await dnsInfo(h);

  // start server
  sec(`Server (${hostname}:${port})`);
  let server;
  try {
    server = await startServer(hostname, port);
    const a = server.address();
    ok(`listening on ${a.address}:${a.port} (${a.family})`);
  } catch (e) {
    bad(`bind failed: ${e.message}`);
    return process.exit(1);
  }

  // GET tests
  sec('GET /api');
  const okGet = (r) => r.status === 200 && r.body && r.body.status === 'ok';
  await check('127.0.0.1',       'GET', `http://127.0.0.1:${port}/api`, {},           null, okGet);
  await check('localhost',        'GET', `http://localhost:${port}/api`,  {},           null, okGet);
  await check('localhost (IPv4)', 'GET', `http://localhost:${port}/api`,  { family: 4 }, null, okGet);

  // POST tests
  sec('POST /api');
  const body = '{"msg":"hello"}';
  const okPost = (r) => r.status === 200 && r.body && r.body.echo === body;
  await check('127.0.0.1',       'POST', `http://127.0.0.1:${port}/api`, {},           body, okPost);
  await check('localhost (IPv4)', 'POST', `http://localhost:${port}/api`,  { family: 4 }, body, okPost);

  // ping
  sec('GET /api/ping');
  await check('ping', 'GET', `http://127.0.0.1:${port}/api/ping`, {}, null,
    (r) => r.status === 200 && r.body && r.body.service === 'test-host-config');

  // 404
  sec('Error handling');
  await check('404', 'GET', `http://127.0.0.1:${port}/nonexistent`, {}, null,
    (r) => r.status === 404);

  // done
  server.close();
  console.log('\n' + '='.repeat(50));
  console.log(`  Results: ${pass} passed, ${fail} failed`);
  console.log('='.repeat(50));
  if (fail) {
    console.log('\nHints:');
    console.log('  localhost fails, 127.0.0.1 works → IPv6 issue; use --hostname 127.0.0.1');
    console.log('  All fail → firewall or port collision');
    console.log('  WSL → check Windows firewall / port forwarding');
  }
  process.exit(fail ? 1 : 0);
}

main().catch((e) => { console.error(e); process.exit(1); });
