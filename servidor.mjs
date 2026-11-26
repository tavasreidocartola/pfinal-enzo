import { createServer, request } from 'node:http';
import { readFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const WEB = new URL('./web/', import.meta.url);
const ASSETS = new Map([
  ['/', ['index.html', 'text/html; charset=utf-8']],
  ['/style.css', ['style.css', 'text/css; charset=utf-8']],
  ['/painel.js', ['painel.js', 'text/javascript; charset=utf-8']],
  ['/icon.svg', ['icon.svg', 'image/svg+xml']],
  ['/manifest.json', ['manifest.json', 'application/manifest+json']]
]);
const COMMAND = /^(?:m,[12],(?:-1|0|1)|v,(?:[3-9]\d|100))$/;
const ESP32 = '192.168.4.1';

function allowedCommand(command) {
  if (command === 'p' || command === 'x') return true;
  const parts = command.split(';');
  return parts.length <= 3 && parts.every(part => COMMAND.test(part)) &&
    new Set(parts.map(part => part.startsWith('v,') ? 'v' : part.split(',')[1])).size === parts.length;
}

function respond(response, status, type, content) {
  if (response.destroyed || response.writableEnded) return;
  response.writeHead(status, {
    'Content-Type': type,
    'Cache-Control': 'no-store',
    'X-Content-Type-Options': 'nosniff'
  });
  response.end(content);
}

function relay(command, response, hostname, port) {
  const upstream = request({
    hostname, port, agent: false, method: 'GET', path: '/cmd?c=' + encodeURIComponent(command)
  }, incoming => {
    if (incoming.statusCode !== 200 || !incoming.headers['content-type']?.startsWith('application/json')) {
      incoming.resume();
      respond(response, 502, 'text/plain; charset=utf-8', 'Resposta inválida do ESP32');
      return;
    }
    let body = '';
    incoming.setEncoding('utf8');
    incoming.on('data', chunk => {
      body += chunk;
      if (body.length > 4096) upstream.destroy(new Error('Resposta longa demais'));
    });
    incoming.on('end', () => respond(response, 200, 'application/json', body));
    incoming.on('error', error => upstream.destroy(error));
  });
  const timeout = setTimeout(() => upstream.destroy(new Error('Tempo limite de resposta')), command === 'p' ? 620 : 300);
  upstream.on('error', () => respond(response, 502, 'text/plain; charset=utf-8', 'ESP32 indisponível'));
  upstream.on('close', () => clearTimeout(timeout));
  upstream.end();
}

export function createPanelServer(hostname = ESP32, port = 80) {
  return createServer(async (incoming, response) => {
    if (!/^(?:localhost|127\.0\.0\.1)(?::\d{1,5})?$/i.test(incoming.headers.host ?? '')) {
      return respond(response, 403, 'text/plain; charset=utf-8', 'Use localhost');
    }
    if (incoming.method !== 'GET') return respond(response, 405, 'text/plain; charset=utf-8', 'Método indisponível');
    if (!incoming.url?.startsWith('/') || incoming.url.startsWith('//') || incoming.url.includes('\\')) {
      return respond(response, 400, 'text/plain; charset=utf-8', 'Endereço inválido');
    }
    const url = new URL(incoming.url, 'http://localhost');
    if (url.pathname === '/cmd') {
      if (incoming.headers['x-garra-painel'] !== '1' || incoming.headers['sec-fetch-site'] === 'cross-site') {
        return respond(response, 403, 'text/plain; charset=utf-8', 'Comando fora do painel');
      }
      const command = url.searchParams.get('c') ?? '';
      if (!allowedCommand(command)) return respond(response, 400, 'text/plain; charset=utf-8', 'Comando inválido');
      relay(command, response, hostname, port);
      return;
    }
    const asset = ASSETS.get(url.pathname);
    if (!asset) return respond(response, 404, 'text/plain; charset=utf-8', 'Página indisponível');
    try {
      respond(response, 200, asset[1], await readFile(new URL(asset[0], WEB)));
    } catch {
      respond(response, 500, 'text/plain; charset=utf-8', 'Arquivo do painel indisponível');
    }
  });
}

if (process.argv[1] && fileURLToPath(import.meta.url) === resolve(process.argv[1])) {
  const server = createPanelServer();
  server.on('error', error => {
    console.error(error.code === 'EADDRINUSE' ? 'A porta 8080 já está em uso.' : error.message);
    process.exitCode = 1;
  });
  server.listen(8080, '127.0.0.1', () => console.log('Abra http://localhost:8080 e conecte-se à rede do ESP32.'));
}
