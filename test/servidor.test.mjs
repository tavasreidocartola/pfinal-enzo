import { createServer } from 'node:http';
import { once } from 'node:events';
import { after, before, test } from 'node:test';
import assert from 'node:assert/strict';
import { createPanelServer } from '../servidor.mjs';

const received = [];
const directions = [0, 0];
let power = 70;
let base;
let panel;
let esp;

before(async () => {
  esp = createServer((incoming, response) => {
    const command = new URL(incoming.url, 'http://esp').searchParams.get('c');
    received.push(command);
    for (const item of command.split(';')) {
      const [kind, value, direction] = item.split(',');
      if (kind === 'm') directions[Number(value) - 1] = Number(direction);
      if (kind === 'v') power = Number(value);
      if (kind === 'x') directions.fill(0);
    }
    response.setHeader('Content-Type', 'application/json');
    response.end(JSON.stringify({ v: power, j: directions.map((d, i) => ({ id: i + 1, d, t: false })) }));
  }).listen(0, '127.0.0.1');
  await once(esp, 'listening');
  panel = createPanelServer('127.0.0.1', esp.address().port).listen(0, '127.0.0.1');
  await once(panel, 'listening');
  base = 'http://127.0.0.1:' + panel.address().port;
});

after(async () => {
  if (panel) {
    panel.closeAllConnections();
    panel.close();
  }
  if (esp?.listening) esp.close();
});

function command(value, headers = { 'X-Garra-Painel': '1' }) {
  return fetch(base + '/cmd?c=' + encodeURIComponent(value), { headers });
}

test('serve todos os recursos da interface local', async () => {
  for (const path of ['/', '/style.css', '/painel.js', '/icon.svg', '/manifest.json']) {
    const response = await fetch(base + path);
    assert.equal(response.status, 200);
    assert.equal(response.headers.get('cache-control'), 'no-store');
    assert.ok((await response.text()).length);
  }
});

test('encaminha ping, os quatro sentidos, PWM e as duas formas de parar', async () => {
  const check = async (value, expected, percent = 70) => {
    const response = await command(value);
    assert.equal(response.status, 200);
    const state = await response.json();
    assert.equal(state.v, percent);
    assert.deepEqual(state.j.map(motor => motor.d), expected);
    assert.equal(received.at(-1), value);
  };
  await check('p', [0, 0]);
  await check('m,1,1;m,2,-1', [1, -1]);
  await check('m,1,0;m,2,0', [0, 0]);
  await check('m,1,-1;m,2,1;v,56', [-1, 1], 56);
  await check('x', [0, 0], 56);
});

test('recusa comandos externos, saídas inexistentes e direções contraditórias', async () => {
  const previous = received.length;
  assert.equal((await command('x', {})).status, 403);
  assert.equal((await command('x', { 'X-Garra-Painel': '1', 'Sec-Fetch-Site': 'cross-site' })).status, 403);
  for (const invalid of ['m,0,1', 'm,3,1', 'm,1,1;m,1,-1', 'v,29', 'x;m,1,1']) {
    assert.equal((await command(invalid)).status, 400);
  }
  assert.equal(received.length, previous);
});

test('sinaliza firmware indisponível após queda da conexão', async () => {
  esp.close();
  await once(esp, 'close');
  assert.equal((await command('p')).status, 502);
});
