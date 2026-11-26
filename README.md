# Garra Hidráulica · Central de comando

Painel web para o protótipo escolar de um braço hidráulico com seringas. O ESP32 controla dois motores DC de 6 V pela ponte H. A base e a garra são operadas manualmente.

| Articulação | Comandos no painel | GPIOs do ESP32 | ID em `/cmd` |
| --- | --- | --- | --- |
| Ombro | Subir e Descer | 25 e 26 | 1 |
| Cotovelo | Estender e Recolher | 32 e 33 | 2 |

O cotovelo mantém as saídas reaproveitadas da antiga base, conforme a [versão corrigida de produto-final](https://github.com/tavasreidocartola/produto-final/tree/bab4613d178ea54d0dc66dc22537e5f5c257e21e). Uma cópia do anexo original, com a antiga pinagem e a senha ocultada, está em `referencia/firmware-anexado.ino.txt`.

## Gravar no ESP32

1. Baixe o ZIP do repositório e extraia a pasta inteira.
2. Abra **`garra_esp32/garra_esp32.ino`** na Arduino IDE. O sketch já contém todo o painel e usa as bibliotecas que acompanham o pacote de placas ESP32 da Espressif.
3. Conecte a placa pelo USB. Selecione o modelo da placa e a porta e clique em **Carregar**. A compilação deste projeto foi verificada com o alvo **ESP32 Dev Module** do pacote Espressif 3.3.7.
4. Abra o **Monitor Serial** a **115200 baud** e pressione **EN/RESET** na placa. O ESP32 gera e guarda uma senha Wi-Fi na primeira inicialização e a exibe no monitor.
5. Conecte o computador à rede **Garra-Carangueijo** com a senha exibida. Se o computador já tiver essa rede salva, use **Esquecer rede** antes de reconectar. Se o sistema perguntar, escolha permanecer conectado mesmo sem internet.

## Abrir no localhost

Com o [Node.js](https://nodejs.org/) 18 ou mais recente instalado, abra o terminal na pasta que contém `servidor.mjs` e execute:

```text
node servidor.mjs
```

Abra **[http://localhost:8080](http://localhost:8080)** no navegador. O painel tenta conectar automaticamente. **Conectar** ou **Reconectar** força uma nova tentativa. O servidor roda no computador e encaminha os comandos para o ESP32 pelo Wi-Fi. Mantenha esse terminal aberto durante o uso.

Pelo celular ou tablet, conecte-se à mesma rede Wi-Fi e abra **[http://192.168.4.1](http://192.168.4.1)**. O ESP32 serve o mesmo painel diretamente. Também é possível usar este endereço no computador.

## Operar o braço

- Segure um botão com o mouse, com o dedo ou com **Espaço/Enter** enquanto o botão estiver focado. Solte para parar. O painel ignora o sentido oposto enquanto a articulação já tiver um botão pressionado.
- O controle deslizante ajusta a potência PWM configurada. O desenho indica os comandos que o firmware aceitou. O protótipo não possui leitura de ângulo, velocidade física ou posição.
- **Parada de emergência** envia o comando `x` para zerar as saídas dos dois motores e bloqueia os controles do painel. **Liberar controles** fica disponível após o ESP32 responder com os dois motores parados.
- A perda de foco encerra o acionamento. Se o enlace cair, o firmware zera cada saída após cerca de 400 ms sem renovação. Um acionamento contínuo também é limitado a 6 s. A parada pela rede depende do firmware e da alimentação estarem operando.
- Use um painel por vez. Espere a resposta do ESP32 para confirmar que a parada chegou ao firmware.

## Arquivos e integração

| Caminho | Função |
| --- | --- |
| `garra_esp32/garra_esp32.ino` | Firmware pronto para gravação, com o painel embutido |
| `web/` | HTML, CSS, JavaScript, ícone e manifesto |
| `servidor.mjs` | Servidor de localhost sem dependências externas |
| `scripts/embutir.mjs` | Atualiza os recursos embutidos no sketch a partir de `web/` |
| `referencia/firmware-anexado.ino.txt` | Sketch anexado para consulta, com a senha antiga ocultada |
| `test/servidor.test.mjs` | Testes do encaminhamento HTTP com um ESP32 simulado |

A rota HTTP `GET /cmd?c=...` preserva `p` para consultar, `m,<id>,<sentido>` para mover ou parar, `v,<30–100>` para PWM e `x` para parar tudo. O firmware mantém o JSON com `v` e `j`, a frequência de 1 kHz, a rampa de partida de 150 ms e a potência inicial de 70%. A proteção roda em uma tarefa do FreeRTOS separada do servidor HTTP, para que uma requisição lenta não atrase a verificação dos motores.

Após editar a interface, rode `node scripts/embutir.mjs` para atualizar também a versão servida pelo ESP32. Para verificar a sincronização e testar o servidor:

```text
node scripts/embutir.mjs --check
node --test
```

Ainda é preciso confirmar no braço físico os quatro sentidos de movimento, a parada ao soltar e a resposta à perda de Wi-Fi.
