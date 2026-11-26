import { readFile, writeFile } from 'node:fs/promises';

const sketch = new URL('../garra_esp32/garra_esp32.ino', import.meta.url);
const web = new URL('../web/', import.meta.url);
const start = '// INÍCIO DOS RECURSOS GERADOS';
const end = '// FIM DOS RECURSOS GERADOS';
const resources = [
  ['PAGINA_HTML', 'index.html'], ['ESTILO_CSS', 'style.css'], ['PAINEL_JS', 'painel.js'],
  ['ICONE_SVG', 'icon.svg'], ['MANIFESTO_JSON', 'manifest.json']
];

const original = await readFile(sketch, 'utf8');
const first = original.indexOf(start);
const last = original.indexOf(end);
if (first === -1 || last <= first) throw new Error('Marcadores de recursos ausentes no sketch');

const sections = await Promise.all(resources.map(async ([name, file]) => {
  const content = (await readFile(new URL(file, web), 'utf8')).replaceAll('\r\n', '\n');
  if (content.includes(')panel"')) throw new Error('Delimitador C++ presente em ' + file);
  return 'const char ' + name + '[] PROGMEM = R"panel(' + content + ')panel";';
}));
const update = original.slice(0, first + start.length) + '\n' + sections.join('\n\n') + '\n' + original.slice(last);
if (process.argv.includes('--check')) {
  if (update !== original) {
    console.error('Sketch desatualizado. Execute node scripts/embutir.mjs.');
    process.exitCode = 1;
  }
} else {
  await writeFile(sketch, update);
}
