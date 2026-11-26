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
## Abrir o painel pela rede do ESP32

1. No computador, celular ou tablet, conecte-se ao Wi-Fi **Garra-ESP-Enzo** com a senha exibida no Monitor Serial. Se o sistema perguntar, escolha permanecer conectado mesmo sem internet.
2. Com o ESP32 ligado e o Wi-Fi conectado, abra **[http://192.168.4.1](http://192.168.4.1)** no navegador.

O ESP32 hospeda a página e recebe os comandos pelo Wi-Fi que ele cria. A conexão com o painel é automática. Use **Conectar** ou **Reconectar** para tentar novamente.

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
| `web/` | Cópias legíveis do HTML, CSS, JavaScript, ícone e manifesto embutidos no sketch |
| `referencia/firmware-anexado.ino.txt` | Sketch anexado para consulta, com a senha antiga ocultada |

A rota HTTP `GET /cmd?c=...` preserva `p` para consultar, `m,<id>,<sentido>` para mover ou parar, `v,<30–100>` para PWM e `x` para parar tudo. O firmware mantém o JSON com `v` e `j`, a frequência de 1 kHz, a rampa de partida de 150 ms e a potência inicial de 70%. A proteção roda em uma tarefa do FreeRTOS separada do servidor HTTP, para que uma requisição lenta não atrase a verificação dos motores.

Ainda é preciso confirmar no braço físico os quatro sentidos de movimento, a parada ao soltar e a resposta à perda de Wi-Fi.
