# ESP32EspNow

Biblioteca para gerenciamento completo de **ESP-NOW** em sistemas com um dispositivo **central** e múltiplos **módulos** periféricos — projetada para o padrão de automação de sala de aula.

[![Plataforma](https://img.shields.io/badge/plataforma-ESP32-blue)](https://platformio.org)
[![Licenca](https://img.shields.io/badge/licenca-MIT-green)](LICENSE)
[![Versao](https://img.shields.io/badge/versao-1.0.0-orange)](library.json)

---

## Funcionalidades

| Recurso | Central | Módulo |
|---|:---:|:---:|
| Descoberta automática via broadcast | ✔ | ✔ |
| Registro persistente de peers (NVS) | ✔ | — |
| Envio por MAC, ID lógico ou label | ✔ | — |
| Envio para todos de um tipo | ✔ | — |
| Recepção com buffer ISR seguro | ✔ | ✔ |
| Heartbeat periódico com detecção offline | ✔ | ✔ |
| Relatório de status tipado | — | ✔ |
| Integração com ESP32Connectivity | ✔ | — |
| Integração com DebugManager | ✔ | ✔ |

---

## Instalação

```ini
; platformio.ini
lib_deps =
    https://github.com/professorThiago/ESP32EspNow
    https://github.com/professorThiago/DebugManager
```

---

## Protocolo de mensagens

Todo pacote começa com um `CabecalhoMsg` de 7 bytes:

```
[ versao(1) | tipo(1) | moduloId(1) | timestamp(4) ]
```

### Tipos de mensagem

| Constante | Hex | Direção | Descrição |
|---|---|---|---|
| `TipoMsg::PING` | 0x01 | Central → broadcast | Início da descoberta |
| `TipoMsg::PONG` | 0x02 | Módulo → central | Resposta com tipo e label |
| `TipoMsg::CMD_AC` | 0x10 | Central → módulo | Comando ar condicionado |
| `TipoMsg::CMD_PROJETOR` | 0x11 | Central → módulo | Comando projetor Epson |
| `TipoMsg::CMD_TV` | 0x12 | Central → módulo | Comando TV LG |
| `TipoMsg::CMD_TELA` | 0x13 | Central → módulo | Comando tela RF 433 |
| `TipoMsg::CMD_LUZES` | 0x14 | Central → módulo | Comando lâmpadas SSR |
| `TipoMsg::STATUS` | 0x20 | Módulo → central | Relatório de estado |
| `TipoMsg::PING_HEARTBEAT` | 0xF0 | Central → módulo | Verificação periódica |
| `TipoMsg::PONG_HEARTBEAT` | 0xF1 | Módulo → central | Resposta ao heartbeat |

---

## Uso rápido — Central

```cpp
#include <ESP32EspNow.h>

EspNowCentral central;

void setup() {
    configurarDebug(DEBUG_INFO, -1);

    WiFi.mode(WIFI_STA);
    WiFi.begin("rede", "senha");
    while (WiFi.status() != WL_CONNECTED) delay(100);

    central.begin("sala-01");

    central.aoEncontrarModulo([](const InfoModulo& m) {
        debugInfo("Encontrado: " + String(m.label));
        central.registrarModulo(m);
    });

    central.aoReceberStatus([](const InfoModulo& m, const MsgStatus& s) {
        debugInfo("Status de " + String(m.label) +
                  " | RSSI: " + String(s.rssi));
    });

    central.aoModuloOffline([](const InfoModulo& m) {
        debugAviso(String(m.label) + " offline!");
    });

    central.configurarHeartbeat(30000);
    central.iniciarDescoberta();
}

void loop() { central.atualizar(); }
```

---

## Uso rápido — Módulo

```cpp
#include <ESP32EspNow.h>

EspNowModulo modulo;

void setup() {
    configurarDebug(DEBUG_INFO, -1);
    modulo.begin(TipoModulo::LUZES_RELE, "LUZ-01");

    modulo.aoReceberComando([](const uint8_t* dados, uint8_t tam) {
        if (tam < sizeof(MsgCmdLuzes)) return;
        MsgCmdLuzes cmd;
        memcpy(&cmd, dados, sizeof(cmd));
        // executa...
        uint8_t estado[8] = {0x0F, 4}; // todos ligados, 4 canais
        modulo.reportarStatus(estado);
    });
}

void loop() { modulo.atualizar(); }
```

---

## Envio de comandos — formas disponíveis

```cpp
MsgCmdLuzes cmd;
cmd.mascaraCanal = 0x0F;
cmd.estado = 1;

// Por MAC
uint8_t mac[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
central.enviarPorMac(mac, cmd);

// Por ID lógico (atribuído no registro)
central.enviarPorId(2, cmd);

// Por label
central.enviarComando("LUZ-01", cmd);

// Para todos de um tipo
central.enviarParaTodos(TipoModulo::LUZES_RELE, cmd);
```

---

## Tipos de módulo

| Constante | Descrição |
|---|---|
| `TipoModulo::AR_CONDICIONADO` | IR Fujitsu ARRAH2E |
| `TipoModulo::PROJETOR_IR` | IR Epson (biblioteca EpsonIR) |
| `TipoModulo::TV_LG_IR` | IR LG NEC 32-bit |
| `TipoModulo::TELA_RF433` | RF 433 MHz (biblioteca TelaProjecaoRF) |
| `TipoModulo::LUZES_RELE` | Relé de estado sólido GPIO |

---

## Constantes configuráveis

Redefina **antes** do `#include` para personalizar sem alterar a biblioteca:

```cpp
#define ESPNOW_MAX_MODULOS   20   // máximo de módulos registrados
#define ESPNOW_MAX_LABEL     17   // tamanho máximo do label
#define ESPNOW_MAX_SALA_ID   17   // tamanho máximo do salaId
```

---

## Dependências

```ini
lib_deps =
    https://github.com/professorThiago/DebugManager   ; obrigatória
```

---

## Licença

MIT License — veja [LICENSE](LICENSE)

---

## Autor

**professorThiago** — [github.com/professorThiago](https://github.com/professorThiago)
