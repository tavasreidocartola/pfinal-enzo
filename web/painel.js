'use strict';

const byId = id => document.getElementById(id);
const buttons = Array.from(document.querySelectorAll('.move'));
const connection = byId('connection');
const speed = byId('speed');

const labels = {
  1: { name: 'Ombro', '-1': 'Descer', 1: 'Subir' },
  2: { name: 'Cotovelo', '-1': 'Recolher', 1: 'Estender' }
};
const REPEAT_MS = 150;
const RETRY_MS = 1200;
const COMMAND_TIMEOUT_MS = 370;
const PING_TIMEOUT_MS = 750;

const held = new Map();
const released = new Set();
const newPresses = new Set();
let status = 'checking';
let snapshot = null;
let inFlight = false;
let mustStop = false;
let latched = false;
let stopConfirmed = false;
let movementDue = false;
let pendingSpeed = null;
let probeDue = true;
let retryAt = 0;
let lastDispatch = -Infinity;
let lastPing = -Infinity;
let sliderEditing = false;
let lastCommand = 'Nenhum comando enviado';

function setText(id, value) {
  const node = byId(id);
  if (node.textContent !== value) node.textContent = value;
}

function render() {
  const online = status === 'online';
  const available = online && !latched && !mustStop;
  const moving = snapshot?.j.some(motor => motor.d !== 0);
  const operating = latched ? 'locked' : !online || mustStop ? 'waiting' : moving ? 'moving' : 'ready';

  connection.dataset.status = status;
  setText('connection-text', {
    checking: 'Buscando ESP32', online: 'ESP32 conectado',
    offline: 'ESP32 desconectado', incompatible: 'Firmware incompatível'
  }[status]);
  setText('connect-text', online ? 'Reconectar' : 'Conectar');
  byId('connect').disabled = status === 'checking' && inFlight;
  byId('operation').dataset.mode = operating;
  setText('operation-text', {
    locked: 'Painel bloqueado', waiting: 'Controles indisponíveis',
    moving: 'Comando em execução', ready: 'Pronto para operar'
  }[operating]);

  const communication = {
    checking: 'Verificando resposta do ESP32',
    online: 'ESP32 respondendo',
    offline: 'Conecte-se à rede Wi-Fi do ESP32',
    incompatible: 'Grave o sketch deste projeto'
  }[status];
  setText('communication', mustStop ? 'Aguardando confirmação de parada'
    : latched && stopConfirmed && online ? 'Parada confirmada pelo ESP32' : communication);
  setText('last-command', lastCommand);

  byId('stop').dataset.latched = latched;
  byId('reset').hidden = !latched;
  byId('reset').disabled = !online || !stopConfirmed || mustStop;
  speed.disabled = !available;
  const shownSpeed = sliderEditing ? speed.value : pendingSpeed ?? snapshot?.v;
  if (!sliderEditing && shownSpeed !== undefined) speed.value = shownSpeed;
  setText('speed-value', shownSpeed === undefined ? '—' : shownSpeed + '%');

  for (const id of [1, 2]) {
    const motor = snapshot?.j[id - 1];
    const active = online && Boolean(motor?.d);
    const limited = online && Boolean(motor?.t);
    const joint = byId('joint-' + id);
    joint.dataset.active = active;
    joint.dataset.limited = limited;
    byId('drawing-' + id).classList.toggle('is-active', active);
    byId('callout-' + id).classList.toggle('is-active', active);
    setText('joint-state-' + id, !online ? 'Sem comunicação' : limited ? 'Solte para liberar'
      : active ? labels[id][motor.d] : held.has(id) ? 'Enviando…' : released.has(id) ? 'Parando…' : 'Parado');
  }
  for (const button of buttons) {
    const id = Number(button.dataset.motor);
    const current = held.get(id);
    const opposite = current && current.button !== button;
    button.disabled = !available;
    button.classList.toggle('is-held', current?.button === button);
    button.setAttribute('aria-disabled', String(Boolean(opposite)));
  }
}

function validStatus(value) {
  return Number.isInteger(value?.v) && value.v >= 30 && value.v <= 100 &&
    Array.isArray(value.j) && value.j.length === 2 &&
    value.j.every((motor, index) => motor?.id === index + 1 &&
      [-1, 0, 1].includes(motor.d) && typeof motor.t === 'boolean');
}

function clearInputs() {
  for (const { button, pointer } of held.values()) {
    if (typeof pointer === 'number' && button.hasPointerCapture(pointer)) {
      button.releasePointerCapture(pointer);
    }
  }
  held.clear();
  released.clear();
  newPresses.clear();
  movementDue = false;
  pendingSpeed = null;
  sliderEditing = false;
}

function requestStop(emergency = false) {
  clearInputs();
  mustStop = true;
  stopConfirmed = false;
  latched ||= emergency;
  retryAt = 0;
  render();
  dispatch();
}

// Mantém só a intenção atual. Uma liberação tem prioridade sobre outra direção do mesmo motor.
function nextCommand() {
  if (mustStop) return { kind: 'stop', value: 'x', label: latched ? 'Parada de emergência' : 'Parar motores' };
  if (status === 'online' && !latched && !document.hidden) {
    const commands = [];
    const descriptions = [];
    for (const id of released) {
      commands.push('m,' + id + ',0');
      descriptions.push(labels[id].name + ' parado');
    }
    if (movementDue) {
      for (const [id, { direction }] of held) {
        if (released.has(id)) continue;
        commands.push('m,' + id + ',' + direction);
        if (newPresses.delete(id)) descriptions.push(labels[id].name + ' · ' + labels[id][direction]);
      }
      if (![...released].some(id => held.has(id))) movementDue = false;
    }
    released.clear();
    if (pendingSpeed !== null) {
      commands.push('v,' + pendingSpeed);
      descriptions.push('Potência · ' + pendingSpeed + '%');
      pendingSpeed = null;
    }
    if (commands.length) {
      // As renovações não substituem o último comando do operador.
      return { kind: 'control', value: commands.join(';'), label: descriptions.join(' / ') };
    }
  }
  if (document.hidden || (!probeDue && performance.now() - lastPing < RETRY_MS)) return null;
  probeDue = false;
  return { kind: 'ping', value: 'p' };
}

async function dispatch() {
  if (inFlight || performance.now() < retryAt) return;
  const command = nextCommand();
  if (!command) return;
  inFlight = true;
  lastDispatch = lastPing = performance.now();

  if (command.label) lastCommand = command.label;
  render();
  const abort = new AbortController();
  const timeout = setTimeout(() => abort.abort(), command.kind === 'ping' ? PING_TIMEOUT_MS : COMMAND_TIMEOUT_MS);
  try {
    const reply = await fetch('/cmd?c=' + encodeURIComponent(command.value), {
      cache: 'no-store', signal: abort.signal, headers: { 'X-Garra-Painel': '1' },
      keepalive: command.kind === 'stop'
    });
    if (!reply.ok) throw new Error('ESP32 indisponível');
    const state = await reply.json();
    if (!validStatus(state)) {
      status = 'incompatible';
      throw new Error('Formato incompatível');
    }
    snapshot = state;
    status = 'online';
    if (command.kind === 'stop') {
      stopConfirmed = state.j.every(motor => motor.d === 0);
      mustStop = !stopConfirmed;
      if (mustStop) retryAt = performance.now() + RETRY_MS;
    }
    // Se uma reconexão encontrar saídas já acionadas, envie uma parada antes de continuar.
    if (command.kind === 'ping' && state.j.some(motor => motor.d !== 0)) requestStop();
  } catch {
    const mightBeMoving = command.kind === 'control' || held.size || snapshot?.j.some(motor => motor.d);
    clearInputs();
    snapshot = null;
    if (status !== 'incompatible') status = 'offline';
    stopConfirmed = false;
    mustStop ||= Boolean(mightBeMoving) || latched;
    retryAt = performance.now() + (command.kind === 'control' ? 0 : RETRY_MS);
  } finally {
    clearTimeout(timeout);
    inFlight = false;
    render();
    dispatch();
  }
}

function start(button, pointer) {
  const id = Number(button.dataset.motor);
  if (button.disabled || held.has(id) || document.hidden || !document.hasFocus()) return false;
  held.set(id, { button, pointer, direction: Number(button.dataset.dir) });
  newPresses.add(id);
  movementDue = true;
  render();
  dispatch();
  return true;
}

function release(button, pointer) {
  const id = Number(button.dataset.motor);
  const current = held.get(id);
  if (current?.button !== button || current.pointer !== pointer) return;
  held.delete(id);
  newPresses.delete(id);
  released.add(id);
  movementDue = held.size > 0;
  render();
  dispatch();
}

function stopOnLeave() {
  if (held.size || released.size || snapshot?.j.some(motor => motor.d)) requestStop();
}

for (const button of buttons) {
  button.addEventListener('pointerdown', event => {
    if (event.button !== 0) return;
    event.preventDefault();
    if (start(button, event.pointerId)) button.setPointerCapture(event.pointerId);
  });
  for (const eventName of ['pointerup', 'pointercancel', 'lostpointercapture']) {
    button.addEventListener(eventName, event => release(button, event.pointerId));
  }
  button.addEventListener('contextmenu', event => event.preventDefault());
  button.addEventListener('keydown', event => {
    if (event.key !== ' ' && event.key !== 'Enter') return;
    event.preventDefault();
    if (!event.repeat) start(button, event.key);
  });
  button.addEventListener('keyup', event => {
    if (event.key !== ' ' && event.key !== 'Enter') return;
    event.preventDefault();
    release(button, event.key);
  });
  button.addEventListener('blur', () => {
    const current = held.get(Number(button.dataset.motor));
    if (typeof current?.pointer === 'string') release(button, current.pointer);
  });
}

byId('connect').addEventListener('click', () => {
  if (held.size || released.size || snapshot?.j.some(motor => motor.d)) requestStop();
  status = 'checking';
  retryAt = 0;
  probeDue = true;
  render();
  dispatch();
});
byId('stop').addEventListener('click', () => requestStop(true));
byId('reset').addEventListener('click', () => {
  if (!stopConfirmed || status !== 'online' || mustStop) return;
  latched = false;
  render();
});

speed.addEventListener('input', () => {
  sliderEditing = true;
  setText('speed-value', speed.value + '%');
});
speed.addEventListener('change', () => {
  sliderEditing = false;
  pendingSpeed = Number(speed.value);
  dispatch();
});

window.addEventListener('blur', stopOnLeave);
window.addEventListener('pagehide', stopOnLeave);
window.addEventListener('offline', stopOnLeave);
window.addEventListener('focus', () => dispatch());
document.addEventListener('visibilitychange', () => {
  if (document.hidden) stopOnLeave();
  else dispatch();
});
setInterval(() => {
  if (held.size && !document.hasFocus()) stopOnLeave();
  if (held.size && performance.now() - lastDispatch >= REPEAT_MS) movementDue = true;
  dispatch();
}, 30);

render();
dispatch();
