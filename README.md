# ESP32EspNow

Infraestrutura ESP-NOW **mestre/escravo** para ESP32 — genérica, sem protocolo de aplicação imposto.

[![Plataforma](https://img.shields.io/badge/plataforma-ESP32-blue)](https://platformio.org)
[![Licença](https://img.shields.io/badge/licença-MIT-green)](LICENSE)
[![Versão](https://img.shields.io/badge/versão-2.0.0-orange)](library.json)

---

## O que a biblioteca faz

| Recurso | Mestre | Escravo |
|---|:---:|:---:|
| Descoberta automática via broadcast | ✔ | ✔ |
| Registro persistente no NVS | ✔ | — |
| Envio por MAC / ID / label / tipo / todos | ✔ | — |
| Recepção com buffer ISR seguro | ✔ | ✔ |
| Heartbeat + detecção de offline/online | ✔ | ✔ |
| Payload completamente livre | ✔ | ✔ |

## O que o **projeto** define

- Tipos de dispositivo (enum, constantes)
- Structs de comando e resposta
- Lógica de roteamento (MQTT, HTTP, Serial...)
- Interpretação do payload recebido

---

## Instalação

```ini
lib_deps =
    https://github.com/professorThiago/ESP32EspNow
```

---

## Uso mínimo

### Mestre

```cpp
#include <ESP32EspNow.h>

EspNowMestre mestre;

void setup() {
    mestre.begin("meu-sistema");

    mestre.aoEncontrarDispositivo([](const Dispositivo& d) {
        Serial.println("Encontrado: " + String(d.label));
        mestre.registrar(d);
    });

    mestre.aoReceberMensagem([](const Dispositivo& d,
                                 const uint8_t* dados, uint8_t tam) {
        // interprete dados conforme seu protocolo
    });

    mestre.iniciarDescoberta();
}

void loop() { mestre.atualizar(); }
```

### Escravo

```cpp
#include <ESP32EspNow.h>

EspNowEscravo escravo;

void setup() {
    escravo.begin(1, "sensor-01");   // tipo=1, label="sensor-01"

    escravo.aoReceberMensagem([](const uint8_t* dados, uint8_t tam) {
        uint8_t resposta[] = {0x01, 0xFF};
        escravo.responder(resposta, sizeof(resposta));
    });
}

void loop() { escravo.atualizar(); }
```

---

## Envio no mestre — formas disponíveis

```cpp
// Por MAC
mestre.enviarPorMac(mac, &cmd, sizeof(cmd));

// Por ID lógico (atribuído no registro)
mestre.enviarPorId(2, &cmd, sizeof(cmd));

// Por label
mestre.enviarPorLabel("sensor-01", &cmd, sizeof(cmd));

// Para todos de um tipo
mestre.enviarParaTipo(TIPO_SENSOR, &cmd, sizeof(cmd));

// Para todos os registrados
mestre.enviarParaTodos(&cmd, sizeof(cmd));
```

---

## Constantes configuráveis

Redefina **antes** do `#include`:

```cpp
#define ESPNOW_MAX_DISPOSITIVOS  8    // padrão: 20
#define ESPNOW_MAX_LABEL         9    // padrão: 17 (8 chars + '\0')
#define ESPNOW_MAX_PAYLOAD       64   // padrão: 200
#define ESPNOW_TIMEOUT_DESCOBERTA_MS  3000  // padrão: 5000
#define ESPNOW_INTERVALO_HEARTBEAT_MS 10000 // padrão: 30000
#include <ESP32EspNow.h>
```

---

## Estrutura do registro

```cpp
// Iterar todos os dispositivos
for (uint8_t i = 0; i < mestre.registro().total(); i++) {
    Dispositivo& d = mestre.registro().porIndice(i);
    Serial.println(String(d.id) + ": " + d.label);
}

// Buscar por tipo
Dispositivo* lista[10];
uint8_t n = mestre.registro().porTipo(TIPO_SENSOR, lista, 10);

// Converter MAC
String s = RegistroDispositivos::macParaString(d.mac);
uint8_t mac[6];
RegistroDispositivos::stringParaMac("AA:BB:CC:DD:EE:FF", mac);
```

---

## Protocolo interno

O cabeçalho de 9 bytes é **invisível ao projeto** — apenas garante que a biblioteca funcione corretamente entre diferentes instâncias. O payload a partir do byte 10 é inteiramente do projeto.

---

## Exemplos incluídos

| Exemplo | Descrição |
|---|---|
| `Mestre` | Mestre completo com descoberta, envio periódico e heartbeat |
| `Escravo` | Escravo com resposta a comandos |
| `MestreEscravo` | Um único arquivo, compila como mestre ou escravo via `-DROLE_MESTRE` |

---

## Licença

MIT — veja [LICENSE](LICENSE)

## Autor

**professorThiago** — [github.com/professorThiago](https://github.com/professorThiago)
