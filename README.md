# ESP32EspNow

Infraestrutura ESP-NOW **mestre/escravo** para ESP32 — genérica, sem protocolo de aplicação imposto.

[![Plataforma](https://img.shields.io/badge/plataforma-ESP32-blue)](https://platformio.org)
[![Licença](https://img.shields.io/badge/licença-MIT-green)](LICENSE)
[![Versão](https://img.shields.io/badge/versão-2.1.0-orange)](library.json)

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
| **Endereçamento físico por jumper (opcional)** | ✔ | ✔ |

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

## Endereçamento físico por jumper

Permite isolar múltiplos sistemas independentes no **mesmo canal de rádio**.
Útil quando há várias salas/zonas com mestres e escravos distintos.

### Como funciona

6 pinos GPIO com pull-up interno. Jumper **fechado** (pino a GND) = bit **1**.
Jumper **aberto** (pull-up) = bit **0**.

```
addr = bit5<<5 | bit4<<4 | bit3<<3 | bit2<<2 | bit1<<1 | bit0<<0

Addr 0    → inválido — dispositivo fica MUDO (erro no Serial)
Addr 1–62 → endereços válidos
Addr 63   → reservado (broadcast interno de descoberta)
```

### Regra de filtragem

```
PING recebido (broadcast addr=63) → escravo aceita se seu addr > 0
MENSAGEM addr=X                   → aceita APENAS se addr local == X
Modo sem endereçamento (addr=0)   → aceita TUDO (compatibilidade)
```

### Exemplo de hardware — addr=5 (0b000101)

```
GPIO4 (bit0) → jumper FECHADO → 1
GPIO5 (bit1) → jumper ABERTO  → 0
GPIO6 (bit2) → jumper FECHADO → 1
GPIO7 (bit3) → jumper ABERTO  → 0
GPIO8 (bit4) → jumper ABERTO  → 0
GPIO9 (bit5) → jumper ABERTO  → 0
addr = 0b000101 = 5
```

### Uso no código

```cpp
const uint8_t JUMPERS[ESPNOW_BITS_ENDERECO] = {4, 5, 6, 7, 8, 9};

// Mestre
mestre.begin("sala");
mestre.configurarEnderecoFisico(JUMPERS);  // lê jumpers e ativa filtro

// Escravo
escravo.begin(1, "sensor-01");
escravo.configurarEnderecoFisico(JUMPERS); // mesmos pinos, mesmo addr

// Alternativa por software (sem hardware extra)
mestre.configurarEnderecoPorSoftware(5);
escravo.configurarEnderecoPorSoftware(5);

// Consultar endereço lido
Serial.println(mestre.endereco()); // imprime 5
```

---

## Uso mínimo sem endereçamento

```cpp
// Mestre
EspNowMestre mestre;
void setup() {
    mestre.begin("meu-sistema");
    mestre.aoEncontrarDispositivo([](const Dispositivo& d) { mestre.registrar(d); });
    mestre.aoReceberMensagem([](const Dispositivo& d, const uint8_t* buf, uint8_t n) { });
    mestre.iniciarDescoberta();
}
void loop() { mestre.atualizar(); }

// Escravo
EspNowEscravo escravo;
void setup() {
    escravo.begin(1, "sensor-01");
    escravo.aoReceberMensagem([](const uint8_t* buf, uint8_t n) {
        escravo.responder(buf, n);
    });
}
void loop() { escravo.atualizar(); }
```

---

## Envio no mestre

```cpp
mestre.enviarPorMac(mac, &cmd, sizeof(cmd));
mestre.enviarPorId(2, &cmd, sizeof(cmd));
mestre.enviarPorLabel("sensor-01", &cmd, sizeof(cmd));
mestre.enviarParaTipo(TIPO_SENSOR, &cmd, sizeof(cmd));
mestre.enviarParaTodos(&cmd, sizeof(cmd));
```

---

## Constantes configuráveis

```cpp
#define ESPNOW_MAX_DISPOSITIVOS  8    // padrão: 20
#define ESPNOW_MAX_PAYLOAD       64   // padrão: 200
#define ESPNOW_TIMEOUT_DESCOBERTA_MS  3000   // padrão: 5000
#define ESPNOW_INTERVALO_HEARTBEAT_MS 10000  // padrão: 30000
#include <ESP32EspNow.h>
```

---

## Struct Dispositivo

```cpp
struct Dispositivo {
    uint8_t  mac[6];
    uint8_t  tipoDispositivo;   // definido pelo projeto
    uint8_t  id;                // sequencial, atribuído pelo mestre
    uint8_t  versaoFirmware;
    uint8_t  enderecoSala;      // addr lido dos jumpers (0 = sem endereço)
    char     label[17];
    bool     online;
    uint32_t ultimoContato;
};
```

---

## Exemplos incluídos

| Exemplo | Descrição |
|---|---|
| `Mestre` | Mestre com jumpers, descoberta e envio periódico |
| `Escravo` | Escravo com jumpers e resposta a comandos |
| `MestreEscravo` | Arquivo único, `-DROLE_MESTRE` compila como mestre |

---

## Licença

MIT — veja [LICENSE](LICENSE)

## Autor

**professorThiago** — [github.com/professorThiago](https://github.com/professorThiago)

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
