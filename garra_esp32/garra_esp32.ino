// ============================================================
//  Garra hidráulica (seringas + água) com MOTORES DC + PONTE H
//  controlada pelo celular.
//
//  NÃO PRECISA DE NENHUMA BIBLIOTECA EXTRA: usa só o que já vem
//  com o suporte ao ESP32 na Arduino IDE (WiFi, WebServer e LEDC).
//  Tudo está neste arquivo único: a configuração fica logo abaixo.
//
//  Funciona com ponte H L298N, L9110S, TB6612 ou parecidas
//  (2 entradas por motor). No L298N, deixe os jumpers ENA/ENB.
//
//  Comandos em http://192.168.4.1/cmd?c=<comando>
//    m,<motor>,<dir>   dir = 1 (botão +), -1 (botão −), 0 (parar)
//                      o celular repete enquanto o botão está apertado
//    v,<0-100>         velocidade em %
//    x                 PARAR tudo
//    p                 ping
//  Vários comandos podem ir juntos, separados por ';'  ex: m,1,1;m,2,-1
//  Toda resposta é um JSON com o estado atual.
// ============================================================
#include <WiFi.h>
#include <WebServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif

// ============================================================
//  CONFIGURAÇÃO  (edite aqui: pinos, nomes, rede, velocidade)
// ============================================================

// ---------- Rede Wi-Fi criada pelo ESP32 (modo Access Point) ----------
// O celular conecta nesta rede e abre http://192.168.4.1
#define WIFI_SSID   "Garra-ESP-Enzo"
#define WIFI_SENHA  "garra1234"

// ---------- Motores (cada um = 1 motor DC + 1 par de seringas) ----------
// Ombro e cotovelo são motorizados. Base e garra são operadas manualmente.
//
// pinoA / pinoB : vão nas entradas da ponte H (IN1/IN2 ou IN3/IN4).
// botaoMenos / botaoMais : texto dos botões no celular.
// invertido     : true se o motor gira ao contrário do botão.
//
//  GPIO 25/26: Ombro. GPIO 32/33: Cotovelo (mesmas saídas da antiga Base).
struct Motor {
  const char* nome;
  const char* botaoMenos;
  const char* botaoMais;
  uint8_t     pinoA;
  uint8_t     pinoB;
  bool        invertido;
};

#define NUM_MOTORES 2
// Preserva os IDs 1 (ombro) e 2 (cotovelo) no protocolo /cmd.
constexpr int PRIMEIRO_ID_MOTOR = 1;

static const Motor MOTORES[NUM_MOTORES] = {
  // nome        botão −      botão +     pinoA pinoB invertido
  { "Ombro",    "Desce",     "Sobe",       25,   26,   false },
  { "Cotovelo", "Recolhe",   "Estica",     32,   33,   false },
};

// ---------- Velocidade (PWM nas entradas da ponte H) ----------
#define VELOCIDADE_PADRAO  70    // % (ajustável pelo celular)
#define VELOCIDADE_MIN     30    // abaixo disso o motor costuma nem girar
#define PWM_FREQ         1000    // Hz
#define PWM_BITS            8    // 0–255
#define RAMPA_MS          150    // partida suave (evita tranco na hidráulica)

// ---------- Segurança ----------
// O celular manda o comando repetidamente enquanto o botão está apertado.
// Se parar de chegar por este tempo, o motor para (botão solto / sinal caiu).
#define TEMPO_SEM_COMANDO_MS  400
// Tempo máximo girando sem soltar o botão (protege o fim de curso da seringa).
#define TEMPO_MAX_MS         6000

// Recursos embutidos que o ESP32 envia ao navegador. As cópias estão em web/.
// INÍCIO DOS RECURSOS GERADOS
const char PAGINA_HTML[] PROGMEM = R"panel(<!doctype html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
  <meta name="theme-color" content="#061426">
  <title>Garra Hidráulica | Central de comando</title>
  <link rel="icon" href="/icon.svg" type="image/svg+xml">
  <link rel="manifest" href="/manifest.json">
  <link rel="stylesheet" href="/style.css">
  <script src="/painel.js" defer></script>
</head>
<body>
  <svg class="symbols" aria-hidden="true" width="0" height="0">
    <symbol id="i-arm" viewBox="0 0 24 24"><path d="M3 21h13M8 21v-5l5-7m3-2 4-3m0 0 2 2m-2-2-1 4"/><circle cx="8" cy="15" r="2"/><circle cx="14.3" cy="8" r="2"/></symbol>
    <symbol id="i-link" viewBox="0 0 24 24"><path d="M9 15l6-6m-6 4-2 2a3.5 3.5 0 0 0 5 5l3-3a3.5 3.5 0 0 0 0-5m0-1 2-2a3.5 3.5 0 0 0-5-5L9 7a3.5 3.5 0 0 0 0 5"/></symbol>
    <symbol id="i-up" viewBox="0 0 24 24"><path d="M12 19V5m-5 5 5-5 5 5"/></symbol>
    <symbol id="i-down" viewBox="0 0 24 24"><path d="M12 5v14m-5-5 5 5 5-5"/></symbol>
    <symbol id="i-out" viewBox="0 0 24 24"><path d="M10 12H3m4-4-4 4 4 4m7-4h7m-4-4 4 4-4 4"/></symbol>
    <symbol id="i-in" viewBox="0 0 24 24"><path d="M3 12h7m-4-4 4 4-4 4m15-4h-7m4-4-4 4 4 4"/></symbol>
    <symbol id="i-stop" viewBox="0 0 24 24"><rect x="7" y="7" width="10" height="10" rx="1" fill="currentColor" stroke="none"/></symbol>
    <symbol id="i-reset" viewBox="0 0 24 24"><path d="M4 11a8 8 0 1 1 2 6m-2-6V5m0 6h6"/></symbol>
  </svg>
  <div class="shell">
    <header class="topbar">
      <a class="brand" href="/" aria-label="Garra Hidráulica, início">
        <span class="brand-mark"><svg class="icon" aria-hidden="true"><use href="#i-arm"/></svg></span>
        <span><strong>Garra Hidráulica</strong><small>Protótipo de robótica</small></span>
      </a>
      <div class="link-controls">
        <div class="connection" id="connection" data-status="checking" role="status">
          <span class="signal" aria-hidden="true"></span><span id="connection-text">Buscando ESP32</span>
        </div>
        <button class="connect" id="connect" type="button">
          <svg class="icon" aria-hidden="true"><use href="#i-link"/></svg>
          <span id="connect-text">Conectar</span>
        </button>
      </div>
    </header>

    <main>
      <div class="intro">
        <h1>Central de comando<span>.</span></h1>
        <div class="operation" id="operation" data-mode="waiting">
          <span class="operation-icon" aria-hidden="true"></span><span id="operation-text">Aguardando conexão</span>
        </div>
      </div>

      <div class="console">
        <figure class="schematic">
          <div class="figure-head">
            <span><i aria-hidden="true"></i> Braço hidráulico</span>
            <span class="view-label">Esquema lateral</span>
          </div>
          <svg class="diagram" viewBox="0 25 720 342" role="img" aria-labelledby="arm-title arm-desc">
            <title id="arm-title">Esquema do braço hidráulico</title>
            <desc id="arm-desc">Vista lateral conceitual com as seringas, o ombro e o cotovelo. As articulações se iluminam quando o ESP32 indica um comando ativo.</desc>
            <defs>
              <pattern id="hatch" width="7" height="7" patternUnits="userSpaceOnUse" patternTransform="rotate(45)"><path d="M0 0v7" stroke="#2d5576" stroke-width="2"/></pattern>
              <linearGradient id="metal" x1="0" x2="0" y1="0" y2="1" gradientUnits="objectBoundingBox"><stop stop-color="#1f4766"/><stop offset=".48" stop-color="#142e47"/><stop offset="1" stop-color="#0b2136"/></linearGradient>
              <linearGradient id="gold" x1="0" x2="0" y1="0" y2="1" gradientUnits="objectBoundingBox"><stop stop-color="#f5d78e"/><stop offset="1" stop-color="#9c7528"/></linearGradient>
              <g id="bolt"><circle r="3" fill="#081c30" stroke="#6890a8"/><path d="M-1 0h2" stroke="#6890a8"/></g>
              <g id="actuator">
                <path d="M0 0h145" stroke="#10293e" stroke-width="20" stroke-linecap="round"/>
                <path d="M1 0h145" stroke="#85bddd" stroke-width="4"/>
                <path d="M19-14h72l7 5V9l-7 5H19l-5-5V-9Z" fill="#123752" stroke="#5a9aba" stroke-width="1.6"/>
                <path d="M25-9h63V9H25Z" fill="#1768ac" fill-opacity=".38"/>
                <path d="M25-9v18M91-11v22M37-9v6m10-6v4m10-4v6m10-6v4m10-4v6" fill="none" stroke="#78adc8" stroke-width="1.4"/>
                <path d="M10-8v16m4-20v24m86-19v14m4-5v10" stroke="#bb984e" stroke-width="2"/>
                <circle cx="0" cy="0" r="5.5" fill="#0e263e" stroke="#82afc8"/><circle cx="145" cy="0" r="5.5" fill="#0e263e" stroke="#82afc8"/>
              </g>
            </defs>

            <g class="construction" fill="none">
              <path d="M84 339h545M191 53v310M391 52v127M578 113v131" stroke-dasharray="3 6"/>
              <path d="M99 348h183m-183-6v12m183-12v12M63 67v245m-6-245h12m-12 245h12"/>
              <path d="M93 65h14m-7-7v14m475 241h14m-7-7v14M630 94h14m-7-7v14"/>
              <circle cx="191" cy="237" r="93" stroke-dasharray="2 7"/>
              <path d="M297 96a94 94 0 0 1 132-22M341 204a83 83 0 0 0 115-23" stroke-dasharray="2 6"/>
            </g>

            <ellipse cx="195" cy="327" rx="141" ry="15" fill="#030c19" opacity=".72"/>
            <g stroke="#4a7898" stroke-width="1.7" stroke-linejoin="round">
              <path d="M89 315l28-21h146l30 21v17H89Z" fill="url(#metal)"/>
              <path d="M89 315h204m-195 7h186m-159-20h128" fill="none"/>
              <path d="M132 285l20-15h74l22 15v13H132Z" fill="#14334b"/>
              <path d="M133 285h114m-100 6h87" fill="none"/>
              <path d="M156 269l10-43 18-16h21l17 16 9 43-15 11h-44Z" fill="url(#metal)"/>
              <path d="M167 268l10-32h34l9 32-9 6h-35Z" fill="url(#hatch)"/>
              <path d="M112 316h18m123 0h18" stroke="#d4af37" stroke-width="2"/>
              <path d="M147 329v-7m9 7v-7m9 7v-7m9 7v-7m9 7v-7m9 7v-7m9 7v-7m9 7v-7m9 7v-7m9 7v-7" stroke="#346387"/>
            </g>

            <g stroke="#5486a6" stroke-width="1.8" stroke-linejoin="round">
              <path d="M174 222L343 94l31-13 27 10 8 21-9 23-23 13-169 106-22 2-14-12Z" fill="url(#metal)"/>
              <path d="M203 226l142-112 22-10 11 9-5 13-151 104-14 8Z" fill="#0c253c"/>
              <path d="M227 225l126-94 14-7m-126 96 108-80M205 249l172-109" stroke="#2f6a91" fill="none"/>
              <path d="M382 86l165 50 26 14 6 21-12 15-20 2-158-48-23-19-2-18Z" fill="url(#metal)"/>
              <path d="M409 105l126 39 15 9-2 11-13 3-126-38-9-8Z" fill="#0b243a"/>
              <path d="M416 114l116 36 11 7m-128-34 117 37m-123-23 132 40" stroke="#32769e" fill="none"/>
              <path d="M551 150l15-2 16 10 22 6 7 10-5 12-22-5-23 2-13-11Z" fill="#183d58"/>
              <path d="M602 167l16-10 26 9 14 15-5 7-14-12-19-5-10 8m-7 1 9 19 24 14 21-2 2-9-19 1-16-10-5-14" fill="#142f45"/>
              <path d="M646 179l-5 5m7 21 0-6" stroke="#d4af37"/>
              <path d="M180 216l151-116m83-1 107 33m-26 44 46 14" fill="none" stroke="#d4af37" stroke-width="2"/>
            </g>

            <g fill="none" stroke-linecap="round">
              <path d="M149 283c-29-9-17-34 9-36l63-1m78-67c-1 25 21 33 49 22l103-39c30-12 47-9 64 1" stroke="#061624" stroke-width="8"/>
              <path d="M149 283c-29-9-17-34 9-36l63-1m78-67c-1 25 21 33 49 22l103-39c30-12 47-9 64 1" stroke="#339ac7" stroke-width="2.5"/>
            </g>
            <use href="#actuator" transform="translate(205 267) rotate(-48)"/>
            <use href="#actuator" transform="translate(313 175) rotate(1) scale(1.04 .86)"/>
            <use href="#bolt" x="227" y="244"/><use href="#bolt" x="343" y="113"/><use href="#bolt" x="423" y="112"/><use href="#bolt" x="533" y="165"/>
            <use href="#bolt" x="561" y="165"/><use href="#bolt" x="596" y="174"/><use href="#bolt" x="111" y="321"/><use href="#bolt" x="269" y="321"/>

            <g class="diagram-joint" id="drawing-1" transform="translate(191 237)">
              <circle class="halo" r="37"/><circle class="orbit" r="32"/>
              <circle r="23" fill="#0b2036" stroke="url(#gold)" stroke-width="2.5"/>
              <circle class="joint-core" r="14" fill="#163e5a" stroke="#63a3c5" stroke-width="1.6"/>
              <circle r="6" fill="url(#gold)" stroke="#e0bf67"/>
              <path d="M-18 0h5m26 0h5M0-18v5m0 26v5" stroke="#7fb1cb"/>
            </g>
            <g class="diagram-joint" id="drawing-2" transform="translate(386 112)">
              <circle class="halo" r="37"/><circle class="orbit" r="32"/>
              <circle r="22" fill="#0b2036" stroke="url(#gold)" stroke-width="2.5"/>
              <circle class="joint-core" r="13" fill="#163e5a" stroke="#63a3c5" stroke-width="1.6"/>
              <circle r="5.5" fill="url(#gold)" stroke="#e0bf67"/>
              <path d="M-18 0h5m26 0h5M0-18v5m0 26v5" stroke="#7fb1cb"/>
            </g>
            <g class="diagram-callout" id="callout-1">
              <path d="M160 224l-28-31H89v-18"/><circle cx="160" cy="224" r="2.5"/>
              <text x="89" y="160">OMBRO</text>
            </g>
            <g class="diagram-callout" id="callout-2">
              <path d="M408 87l33-31h94"/><circle cx="408" cy="87" r="2.5"/>
              <text x="455" y="43">COTOVELO</text>
            </g>
            <g class="diagram-callout manual">
              <path d="M263 307l44-24h49"/><text x="300" y="268">BASE MANUAL</text>
              <path d="M618 207l-36 42h-56"/><text x="526" y="271">GARRA MANUAL</text>
            </g>
          </svg>
          <figcaption><span class="legend-dot" aria-hidden="true"></span>Destaque indica o comando aceito pelo ESP32.</figcaption>
        </figure>

        <section class="controls" aria-labelledby="controls-title">
          <div class="control-heading">
            <h2 id="controls-title">Movimento</h2>
            <p>Segure para mover. Solte para parar.</p>
          </div>
          <section class="joint" id="joint-1" aria-labelledby="shoulder-title">
            <div class="joint-heading">
              <span class="joint-mark" aria-hidden="true"><svg class="icon" viewBox="0 0 24 24"><path d="M4 20h10m-5 0V9m2-2 9-4"/><circle cx="9" cy="9" r="2.5"/></svg></span>
              <h3 id="shoulder-title">Ombro</h3>
              <span class="joint-state" id="joint-state-1">Aguardando</span>
            </div>
            <div class="joint-actions">
              <button type="button" class="move" data-motor="1" data-dir="1" disabled><svg class="icon" aria-hidden="true"><use href="#i-up"/></svg>Subir</button>
              <button type="button" class="move" data-motor="1" data-dir="-1" disabled><svg class="icon" aria-hidden="true"><use href="#i-down"/></svg>Descer</button>
            </div>
          </section>
          <section class="joint" id="joint-2" aria-labelledby="elbow-title">
            <div class="joint-heading">
              <span class="joint-mark" aria-hidden="true"><svg class="icon" viewBox="0 0 24 24"><path d="m3 19 6-9m4-1 8 5"/><circle cx="11" cy="8.5" r="2.5"/></svg></span>
              <h3 id="elbow-title">Cotovelo</h3>
              <span class="joint-state" id="joint-state-2">Aguardando</span>
            </div>
            <div class="joint-actions">
              <button type="button" class="move" data-motor="2" data-dir="1" disabled><svg class="icon" aria-hidden="true"><use href="#i-out"/></svg>Estender</button>
              <button type="button" class="move" data-motor="2" data-dir="-1" disabled><svg class="icon" aria-hidden="true"><use href="#i-in"/></svg>Recolher</button>
            </div>
          </section>
          <div class="speed">
            <div><label for="speed">Potência dos motores <span>PWM</span></label><output id="speed-value" for="speed">—</output></div>
            <input id="speed" type="range" min="30" max="100" step="1" disabled>
          </div>
        </section>
      </div>
    </main>

    <footer class="command-bar">
      <dl class="command-data">
        <div><dt>Último comando</dt><dd id="last-command">Nenhum comando enviado</dd></div>
        <div><dt>Comunicação</dt><dd id="communication">Verificando resposta do ESP32</dd></div>
      </dl>
      <div class="safety-actions">
        <button class="reset" type="button" id="reset" hidden disabled><svg class="icon" aria-hidden="true"><use href="#i-reset"/></svg>Liberar controles</button>
        <button class="emergency" id="stop" type="button"><span class="stop-mark"><svg class="icon" aria-hidden="true"><use href="#i-stop"/></svg></span>Parada de emergência</button>
      </div>
    </footer>
  </div>
</body>
</html>
)panel";

const char ESTILO_CSS[] PROGMEM = R"panel(:root {
  color-scheme: dark;
  font-family: "Bahnschrift", "Segoe UI", system-ui, sans-serif;
  color: #f1f5f9;
  background: #061426;
  --marine: #061426;
  --blue: #1768ac;
  --cyan: #38bdf8;
  --gold: #d4af37;
  --gold-light: #f5d78e;
  --muted: #a3bbcf;
  --line: #203b54;
  --mono: "Cascadia Code", "SFMono-Regular", Consolas, monospace;
}
* { box-sizing: border-box; }
body { margin: 0; min-width: 280px; min-height: 100svh; }
h1, h2, h3, p, figure, dl, dd { margin: 0; }
button, input { font: inherit; }
button { cursor: pointer; -webkit-tap-highlight-color: transparent; }
button:disabled { cursor: default; }
:focus-visible { outline: 2px solid var(--cyan); outline-offset: 4px; }
[hidden] { display: none !important; }
.symbols { position: absolute; }
.icon { width: 22px; height: 22px; flex: none; fill: none; stroke: currentColor; stroke-width: 1.65; stroke-linecap: round; stroke-linejoin: round; }
.shell { max-width: 1456px; min-height: 100svh; margin: auto; padding: 0 clamp(20px, 4.1vw, 60px); display: flex; flex-direction: column; }
.topbar { min-height: 82px; display: flex; align-items: center; justify-content: space-between; gap: 16px; border-bottom: 1px solid var(--line); }
.brand { display: inline-flex; align-items: center; gap: 12px; color: inherit; text-decoration: none; }
.brand-mark { width: 44px; height: 44px; display: grid; place-items: center; color: var(--gold-light); border: 1px solid #795f2b; background: #1a2634; border-radius: 7px; }
.brand-mark .icon { width: 25px; height: 25px; }
.brand strong, .brand small { display: block; }
.brand strong { font-weight: 680; font-size: 16px; letter-spacing: -.02em; }
.brand small { color: var(--muted); font-size: 12px; margin-top: 3px; }
.link-controls, .connection, .connect, .operation { display: flex; align-items: center; gap: 9px; }
.link-controls { gap: 17px; }
.connection { font: 12px var(--mono); color: var(--muted); }
.signal { width: 7px; height: 7px; border-radius: 50%; background: #849bb1; flex: none; }
.connection[data-status="online"] { color: #9ce7d7; }
.connection[data-status="online"] .signal { background: #6ee7bd; box-shadow: 0 0 0 4px #6ee7bd19; }
.connection[data-status="offline"] .signal, .connection[data-status="incompatible"] .signal { background: #ffa189; }
.connect, .reset { min-height: 40px; justify-content: center; padding: 0 13px; border: 1px solid #32618a; border-radius: 6px; background: #0b2238; color: #cce7fa; font-size: 13px; font-weight: 650; }
.connect .icon, .reset .icon { width: 16px; height: 16px; color: var(--cyan); }
.connect:disabled, .reset:disabled { opacity: .54; }
@media (hover: hover) {
  .connect:hover:enabled, .reset:hover:enabled { background: #15334f; border-color: #56a5d6; }
}
main { padding: clamp(24px, 4.1vh, 46px) 0 24px; }
.intro { display: flex; align-items: center; justify-content: space-between; gap: 14px; margin-bottom: 22px; }
h1 { font-size: clamp(34px, 4.2vw, 62px); line-height: 1.01; font-weight: 670; letter-spacing: -.061em; }
h1 span { color: var(--gold-light); }
.operation { flex: none; padding: 7px 10px; color: var(--muted); background: #0b2136; border: 1px solid var(--line); border-radius: 5px; font: 12px var(--mono); }
.operation-icon { width: 9px; height: 9px; transform: rotate(45deg); border: 1px solid currentColor; flex: none; }
.operation[data-mode="moving"] { color: var(--gold-light); border-color: #725a31; }
.operation[data-mode="moving"] .operation-icon { background: var(--gold-light); }
.operation[data-mode="ready"] { color: #97dfff; }
.operation[data-mode="locked"] { color: #ffb2a8; border-color: #87464c; }
.console { display: grid; grid-template-columns: minmax(0, 1.68fr) minmax(310px, 1fr); min-width: 0; border: 1px solid var(--line); border-radius: 9px; overflow: hidden; }
.schematic {
  position: relative;
  display: flex; flex-direction: column; justify-content: space-between;
  min-width: 0;
  padding: 21px 24px 18px;
  background-color: #091e33;
  background-image: radial-gradient(ellipse at 45% 51%, #1842657d, transparent 62%), linear-gradient(#31557236 1px, transparent 1px), linear-gradient(90deg, #31557236 1px, transparent 1px);
  background-size: auto, 28px 28px, 28px 28px;
}
.figure-head { display: flex; justify-content: space-between; gap: 10px; align-items: center; color: #d1e8f7; font: 12px var(--mono); letter-spacing: .03em; }
.figure-head > span:first-child { display: flex; gap: 9px; align-items: center; }
.figure-head i { width: 14px; height: 11px; display: block; border: 1px solid #50b8f0; border-radius: 1px; position: relative; }
.figure-head i::after { position: absolute; content: ""; left: 3px; right: 3px; top: 4px; border-top: 1px solid #50b8f0; }
.view-label { color: #a2bad0; text-align: right; }
.diagram { width: 100%; height: auto; margin: auto 0; display: block; }
.construction { stroke: #41617a; stroke-width: 1; opacity: .68; }
.diagram-callout { color: #e5bf67; }
.diagram-callout path { stroke: currentColor; fill: none; stroke-width: 1.2; }
.diagram-callout circle { fill: currentColor; }
.diagram-callout text { fill: currentColor; font: 600 13px var(--mono); letter-spacing: .11em; }
.diagram-callout.manual { color: #a6bfd2; }
.diagram-callout.manual path { stroke-dasharray: 4 4; }
.diagram-callout.manual text { font-size: 11px; }
.diagram-callout.is-active { color: #fff0c3; }
.diagram-joint .halo, .diagram-joint .orbit { fill: none; stroke: #cea749; stroke-width: 1.5; transform-origin: 0 0; opacity: .58; }
.diagram-joint .halo { opacity: 0; }
.diagram-joint.is-active .joint-core { fill: #1768ac; stroke: #8fe1ff; }
.diagram-joint.is-active .orbit { stroke: #f5d78e; stroke-width: 2.3; opacity: 1; }
.diagram-joint.is-active .halo { animation: pulse 1.35s ease-out infinite; }
@keyframes pulse { 0% { opacity: .88; transform: scale(.83); } 100% { opacity: 0; transform: scale(1.48); } }
figcaption { display: flex; align-items: center; gap: 8px; color: #b4cadd; font-size: 12px; }
.legend-dot { width: 8px; height: 8px; border-radius: 50%; border: 1px solid var(--gold-light); flex: none; }
.controls { min-width: 0; padding: 22px clamp(16px, 2.3vw, 31px); border-left: 1px solid var(--line); background: #0b1b2e; }
.control-heading h2 { font-size: 23px; font-weight: 660; letter-spacing: -.04em; }
.control-heading p { color: var(--muted); font-size: 13px; margin-top: 4px; }
.joint { padding: 15px 0; border-bottom: 1px solid var(--line); }
.joint-heading { display: flex; align-items: center; gap: 9px; margin-bottom: 9px; }
.joint-mark { display: grid; place-items: center; align-self: center; width: 30px; height: 30px; flex: none; border: 1px solid #344c60; border-radius: 5px; color: var(--gold-light); }
.joint-mark .icon { width: 18px; height: 18px; }
.joint h3 { font-size: 15px; font-weight: 670; }
.joint-state { margin-left: auto; color: var(--muted); font: 11px var(--mono); text-align: right; }
.joint[data-active="true"] .joint-state, .joint[data-limited="true"] .joint-state { color: var(--gold-light); }
.joint-actions { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 8px; }
.move {
  min-height: 53px; padding: 0 8px;
  display: flex; justify-content: center; align-items: center; gap: 7px;
  border: 1px solid #2b587e; border-radius: 6px; background: #102941; color: #edf7ff;
  font-weight: 650; font-size: 13px; user-select: none; -webkit-touch-callout: none; touch-action: none;
  transition: background .15s, border-color .15s, color .15s;
}
.move .icon { color: #69ccfa; width: 19px; height: 19px; }
.move:disabled { background: #101f32; border-color: #223a51; color: #8ca3b8; }
.move:disabled .icon { color: #6688a2; }
.move[aria-disabled="true"] { opacity: .52; }
@media (hover: hover) {
  .move:hover:enabled:not([aria-disabled="true"]) { background: #193b5a; border-color: #61baf0; }
}
.move.is-held:enabled { background: var(--gold-light); border-color: var(--gold-light); color: #151f28; }
.move.is-held:enabled .icon { color: inherit; }
.speed { padding-top: 16px; }
.speed > div { display: flex; align-items: baseline; gap: 8px; justify-content: space-between; }
.speed label { font-size: 13px; font-weight: 650; }
.speed label span { color: var(--muted); margin-left: 4px; font: 10px var(--mono); }
.speed output { color: var(--gold-light); font: 13px var(--mono); }
.speed input { display: block; width: 100%; margin: 5px 0 0; height: 25px; accent-color: var(--gold-light); }
.speed input:disabled { opacity: .45; }
.command-bar {
  margin-top: auto;
  position: sticky; bottom: 0; z-index: 2;
  display: flex; align-items: center; justify-content: space-between; gap: 18px;
  min-height: 81px;
  padding: 12px 0 max(12px, env(safe-area-inset-bottom));
  border-top: 1px solid var(--line); background: var(--marine);
}
.command-data { display: flex; gap: clamp(16px, 3vw, 41px); }
.command-data > div { padding-left: 12px; border-left: 1px solid #31546b; }
.command-data dt { color: var(--muted); font: 11px var(--mono); margin-bottom: 4px; }
.command-data dd { font-size: 13px; line-height: 1.2; }
.safety-actions { display: flex; align-items: center; gap: 8px; }
.reset { gap: 6px; display: flex; align-items: center; }
.emergency {
  display: flex; justify-content: center; align-items: center; gap: 9px; min-height: 48px;
  padding: 6px 15px 6px 9px; touch-action: manipulation;
  font-weight: 720; font-size: 13px; white-space: nowrap;
  border: 1px solid #eb777b; border-radius: 6px; color: #fff3ee; background: #8f2e3b;
}
.stop-mark { width: 29px; height: 29px; display: grid; place-items: center; border: 1px solid #ef9196; border-radius: 50%; }
.stop-mark .icon { width: 19px; height: 19px; }
.emergency:hover, .emergency[data-latched="true"] { background: #a63845; }
.emergency:active { background: #712230; }

@media (max-width: 920px) {
  .shell { padding: 0 24px; }
  .schematic { padding: 18px 16px; }
  .controls { padding: 18px; }
}
@media (max-width: 740px) {
  .topbar { min-height: 68px; }
  .console { grid-template-columns: minmax(0,1fr); }
  .schematic { min-height: 0; padding: 15px 16px; }
  .diagram { max-height: 360px; }
  .controls { border-left: 0; border-top: 1px solid var(--line); }
  .command-bar { flex-wrap: wrap; gap: 10px; min-height: 0; }
  .command-data { flex: 1 1 100%; }
  .command-data > div { flex: 1 1 0; }
  .safety-actions { width: 100%; }
  .emergency { flex: 1; }
}
@media (max-width: 470px) {
  .shell { padding: 0 13px; }
  .topbar { min-height: 62px; }
  .brand { gap: 7px; }
  .brand-mark { width: 35px; height: 35px; }
  .brand-mark .icon { width: 21px; height: 21px; }
  .brand strong { font-size: 14px; }
  .brand small { font-size: 10px; }
  .link-controls { align-items: end; gap: 4px; flex-direction: column; }
  .connection { font-size: 10px; gap: 6px; }
  .connect { min-height: 32px; padding: 0 8px; font-size: 12px; }
  .intro { align-items: start; flex-direction: column; gap: 9px; margin-bottom: 14px; }
  .operation { padding: 5px 8px; font-size: 10px; }
  main { padding: 19px 0 13px; }
  .schematic { padding: 12px 10px; }
  .figure-head, figcaption { font-size: 10px; }
  .diagram-callout text { font-size: 18px; }
  .diagram-callout.manual text { font-size: 13px; }
  .controls { padding: 14px 12px; }
  .control-heading h2 { font-size: 21px; }
  .joint { padding: 10px 0; }
  .joint-heading { margin-bottom: 7px; }
  .joint-mark { width: 26px; height: 26px; }
  .move { min-height: 48px; }
  .speed { padding-top: 11px; }
  .safety-actions .reset, .emergency { padding: 5px 7px; font-size: 12px; }
  .stop-mark { width: 23px; height: 23px; flex: none; }
  .command-data dt { font-size: 10px; }
  .command-data dd { font-size: 11px; }
}
@media (max-width: 360px) {
  .safety-actions:has(.reset:not([hidden])) { flex-direction: column-reverse; }
  .safety-actions:has(.reset:not([hidden])) > button { width: 100%; }
}
@media (max-height: 440px) and (orientation: landscape) {
  .command-bar { position: static; }
}
@media (prefers-reduced-motion: reduce) {
  *, *::before, *::after { transition: none !important; animation: none !important; }
}
)panel";

const char PAINEL_JS[] PROGMEM = R"panel('use strict';

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
      cache: 'no-store', signal: abort.signal,
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
)panel";

const char ICONE_SVG[] PROGMEM = R"panel(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">
  <rect x="2" y="2" width="60" height="60" rx="11" fill="#0b2238" stroke="#d4af37" stroke-width="2"/>
  <path d="M12 51h31M23 51V39l11-15m6-5 10-9m0 0 5 6m-5-6-2 10" fill="none" stroke="#f5d78e" stroke-linecap="round" stroke-linejoin="round" stroke-width="3"/>
  <circle cx="23" cy="38" r="5" fill="#1768ac" stroke="#38bdf8" stroke-width="2"/>
  <circle cx="36" cy="23" r="5" fill="#1768ac" stroke="#38bdf8" stroke-width="2"/>
</svg>
)panel";

const char MANIFESTO_JSON[] PROGMEM = R"panel({"name":"Garra Hidráulica","short_name":"Garra","start_url":"/","display":"standalone","background_color":"#061426","theme_color":"#061426","icons":[{"src":"/icon.svg","sizes":"any","type":"image/svg+xml","purpose":"any"}]}
)panel";
// FIM DOS RECURSOS GERADOS

// ============================================================
//  PROGRAMA
// ============================================================

int   direcao[NUM_MOTORES];          // -1, 0, 1
float duty[NUM_MOTORES];             // força atual (0..1), sobe na rampa
unsigned long ultimoCmd[NUM_MOTORES];
unsigned long inicioGiro[NUM_MOTORES];
bool  travado[NUM_MOTORES];          // parou por tempo máximo: espera soltar o botão
int   velocidade = VELOCIDADE_PADRAO;

WebServer server(80);
SemaphoreHandle_t estadoMutex;
bool servidorAtivo = false;

// ------------------------------------------------------------
//  PWM via LEDC (nativo do ESP32), compatível com core 2.x e 3.x
// ------------------------------------------------------------
#define PWM_MAX ((1 << PWM_BITS) - 1)

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  void pwmIniciar(uint8_t pino, uint8_t) { ledcAttach(pino, PWM_FREQ, PWM_BITS); }
  void pwmEscrever(uint8_t pino, uint8_t, uint32_t v) { ledcWrite(pino, v); }
#else
  void pwmIniciar(uint8_t pino, uint8_t canal) { ledcSetup(canal, PWM_FREQ, PWM_BITS); ledcAttachPin(pino, canal); }
  void pwmEscrever(uint8_t, uint8_t canal, uint32_t v) { ledcWrite(canal, v); }
#endif

void aplicarMotor(int i) {
  int d = direcao[i];
  if (MOTORES[i].invertido) d = -d;
  uint32_t v = (uint32_t)(duty[i] * PWM_MAX);
  uint32_t a = (d > 0) ? v : 0;
  uint32_t b = (d < 0) ? v : 0;
  pwmEscrever(MOTORES[i].pinoA, i * 2,     a);
  pwmEscrever(MOTORES[i].pinoB, i * 2 + 1, b);
}

void pararMotor(int i) {
  direcao[i] = 0;
  duty[i] = 0;
  aplicarMotor(i);
}

void pararTudo() {
  for (int i = 0; i < NUM_MOTORES; i++) pararMotor(i);
}

// ------------------------------------------------------------
String estadoJson() {
  String s = "{\"v\":" + String(velocidade) + ",\"j\":[";
  for (int i = 0; i < NUM_MOTORES; i++) {
    if (i) s += ",";
    s += "{\"id\":" + String(i + PRIMEIRO_ID_MOTOR) +
         ",\"n\":\"" + String(MOTORES[i].nome) + "\"" +
         ",\"m\":\"" + String(MOTORES[i].botaoMenos) + "\"" +
         ",\"p\":\"" + String(MOTORES[i].botaoMais) + "\"" +
         ",\"d\":"   + String(direcao[i]) +
         ",\"t\":"   + (travado[i] ? "true" : "false") + "}";
  }
  return s + "]}";
}

void tratarMensagem(const char* msg) {
  int id, v;
  unsigned long agora = millis();
  switch (msg[0]) {
    case 'm':
      if (sscanf(msg, "m,%d,%d", &id, &v) == 2 &&
          id >= PRIMEIRO_ID_MOTOR && id < PRIMEIRO_ID_MOTOR + NUM_MOTORES) {
        const int j = id - PRIMEIRO_ID_MOTOR;
        v = constrain(v, -1, 1);
        if (v == 0) { pararMotor(j); travado[j] = false; break; }
        if (travado[j]) { ultimoCmd[j] = agora; break; }   // ainda segurando: ignora
        if (v != direcao[j]) {               // começou a girar (ou trocou o sentido)
          pararMotor(j);
          inicioGiro[j] = agora;
          direcao[j] = v;
        }
        ultimoCmd[j] = agora;
      }
      break;
    case 'v':
      if (sscanf(msg, "v,%d", &v) == 1) velocidade = constrain(v, VELOCIDADE_MIN, 100);
      break;
    case 'x':
      pararTudo();
      break;
    default:   // 'p' (ping)
      break;
  }
}

// ------------------------------------------------------------
// Cuida do PWM e dos limites independentemente do atendimento HTTP.
void atualizarMotores(void*) {
  const TickType_t intervalo = pdMS_TO_TICKS(10) ? pdMS_TO_TICKS(10) : 1;
  for (;;) {
    vTaskDelay(intervalo);
    xSemaphoreTake(estadoMutex, portMAX_DELAY);
    const unsigned long agora = millis();
    const float alvo = velocidade / 100.0f;
    const float passo = alvo * 10.0f / RAMPA_MS;

    for (int i = 0; i < NUM_MOTORES; i++) {
      if (travado[i] && agora - ultimoCmd[i] >= TEMPO_SEM_COMANDO_MS) travado[i] = false;
      if (direcao[i] == 0) continue;

      if (agora - ultimoCmd[i] >= TEMPO_SEM_COMANDO_MS) {
        pararMotor(i);
        continue;
      }
      if (agora - inicioGiro[i] >= TEMPO_MAX_MS) {
        pararMotor(i);
        travado[i] = true;
        continue;
      }
      duty[i] = min(alvo, duty[i] + passo);
      aplicarMotor(i);
    }
    xSemaphoreGive(estadoMutex);
  }
}

void servirRecurso(PGM_P tipo, PGM_P conteudo) {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, tipo, conteudo);
}

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < NUM_MOTORES; i++) {
    pinMode(MOTORES[i].pinoA, OUTPUT);
    pinMode(MOTORES[i].pinoB, OUTPUT);
    digitalWrite(MOTORES[i].pinoA, LOW);
    digitalWrite(MOTORES[i].pinoB, LOW);
    pwmIniciar(MOTORES[i].pinoA, i * 2);
    pwmIniciar(MOTORES[i].pinoB, i * 2 + 1);
    pararMotor(i);
  }

  estadoMutex = xSemaphoreCreateMutex();
  if (!estadoMutex || xTaskCreate(atualizarMotores, "motores", 3072, nullptr, 2, nullptr) != pdPASS) {
    Serial.println("Falha ao iniciar a proteção dos motores. Controle desativado.");
    return;
  }

  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(WIFI_SSID, WIFI_SENHA)) {
    Serial.println("Falha ao criar a rede Wi-Fi do ESP32.");
    return;
  }
  Serial.printf("Rede: %s\nSenha: %s\nAbra o painel em http://", WIFI_SSID, WIFI_SENHA);
  Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, []() { servirRecurso(PSTR("text/html; charset=utf-8"), PAGINA_HTML); });
  server.on("/style.css", HTTP_GET, []() { servirRecurso(PSTR("text/css; charset=utf-8"), ESTILO_CSS); });
  server.on("/painel.js", HTTP_GET, []() { servirRecurso(PSTR("text/javascript; charset=utf-8"), PAINEL_JS); });
  server.on("/icon.svg", HTTP_GET, []() { servirRecurso(PSTR("image/svg+xml"), ICONE_SVG); });
  server.on("/manifest.json", HTTP_GET, []() { servirRecurso(PSTR("application/manifest+json"), MANIFESTO_JSON); });
  server.on("/cmd", HTTP_GET, []() {
    const String c = server.arg("c");
    server.sendHeader("Cache-Control", "no-store");
    if (c.length() > 200) {
      server.send(400, "text/plain", "Comando longo demais");
      return;
    }
    xSemaphoreTake(estadoMutex, portMAX_DELAY);
    int ini = 0;
    while (ini < (int)c.length()) {
      int fim = c.indexOf(';', ini);
      if (fim < 0) fim = c.length();
      tratarMensagem(c.substring(ini, fim).c_str());
      ini = fim + 1;
    }
    const String resposta = estadoJson();
    xSemaphoreGive(estadoMutex);
    server.send(200, "application/json", resposta);
  });
  server.onNotFound([]() {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  });
  server.begin();
  servidorAtivo = true;
}

void loop() {
  if (servidorAtivo) server.handleClient();
  delay(1);
}
