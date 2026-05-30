/**
 * @file DescobertaModulos.ino
 * @brief Demonstra o fluxo completo de descoberta e registro de módulos.
 *
 * ## O que este exemplo faz
 *
 * 1. Central (ESP32-S3) conecta ao WiFi.
 * 2. Inicia descoberta via broadcast PING.
 * 3. Ao encontrar um módulo novo, imprime as informações e registra no NVS.
 * 4. Módulos já registrados são carregados automaticamente no boot.
 * 5. Heartbeat automático a cada 15s detecta módulos offline.
 *
 * ## Hardware
 *
 * - 1× ESP32-S3 (central)
 * - 1 ou mais ESP32 com firmware de módulo rodando o exemplo `SistemaCompleto`
 *
 * ## Serial Monitor (115200 baud)
 *
 * ```
 * [INFO]  ESP32 — DebugManager v3.0.0
 * [INFO]  Nivel ativo: 3 - INFO
 * [INFO]  WiFi conectado!
 * [INFO]  IP: 192.168.1.102
 * [INFO]  [EspNowCentral] Iniciado — sala: sala-01
 * [INFO]  [EspNowCentral] Modulos registrados: 0
 * [INFO]  [EspNowCentral] Descoberta iniciada (5000 ms)...
 * [INFO]  [EspNowCentral] PONG de AA:BB:CC:DD:EE:FF — LUZ-01 (LUZES_RELE)
 * [INFO]  >>> Novo modulo encontrado!
 * [INFO]      Label:  LUZ-01
 * [INFO]      Tipo:   LUZES_RELE
 * [INFO]      MAC:    AA:BB:CC:DD:EE:FF
 * [INFO]      FW:     v1
 * [INFO]  [Registro] Modulo adicionado: LUZ-01 | ID: 1 | MAC: AA:BB:CC:DD:EE:FF
 * [INFO]  [EspNowCentral] Descoberta encerrada.
 * ```
 *
 * @author  professorThiago
 * @version 1.0.0
 */

#include <WiFi.h>
#include <ESP32EspNow.h>

// ---------------------------------------------------------------------------
// Configuração
// ---------------------------------------------------------------------------

const char* WIFI_SSID  = "Escola-WiFi";
const char* WIFI_SENHA = "senha_escola";
const char* SALA_ID    = "sala-01";

// ---------------------------------------------------------------------------
// Instância global do central
// ---------------------------------------------------------------------------

EspNowCentral central;

// ---------------------------------------------------------------------------
// Callback — novo módulo encontrado na descoberta
// ---------------------------------------------------------------------------

void aoEncontrarModulo(const InfoModulo& modulo) {
    debugInfo(">>> Novo modulo encontrado!");
    debugInfo("    Label:  " + String(modulo.label));
    debugInfo("    Tipo:   " + String(RegistroModulos::nomeTipo(modulo.tipo)));
    debugInfo("    MAC:    " + RegistroModulos::macParaString(modulo.mac));
    debugInfo("    FW:     v" + String(modulo.versaoFirmware));

    // Registra automaticamente no NVS e adiciona como peer ESP-NOW
    if (central.registrarModulo(modulo)) {
        debugInfo("    >>> Registrado com sucesso!");
    } else {
        debugErro("    >>> Falha no registro (registro cheio?).");
    }
}

// ---------------------------------------------------------------------------
// Callback — STATUS recebido de um módulo
// ---------------------------------------------------------------------------

void aoReceberStatus(const InfoModulo& modulo, const MsgStatus& status) {
    debugInfo("STATUS de " + String(modulo.label) +
              " | RSSI: " + String(status.rssi) + " dBm");
    debugVerbose("  payload[0]: 0x" + String(status.payloadEstado[0], HEX) +
                 "  payload[1]: 0x" + String(status.payloadEstado[1], HEX));
}

// ---------------------------------------------------------------------------
// Callback — módulo ficou offline
// ---------------------------------------------------------------------------

void aoModuloOffline(const InfoModulo& modulo) {
    debugAviso("ALERTA: " + String(modulo.label) + " nao responde!");
}

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------

void setup() {
    // Nível INFO; puxe GPIO0 a GND antes de ligar para forçar DEBUG_TUDO
    configurarDebug(DEBUG_INFO, 0);

    // Conecta ao WiFi (bloqueante para simplificar o exemplo)
    debugInfo("Conectando ao WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_SENHA);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        debugVerbose("Aguardando WiFi...");
    }
    debugInfo("WiFi conectado! IP: " + WiFi.localIP().toString());

    // Inicia ESP-NOW central (após WiFi para fixar o canal correto)
    central.begin(SALA_ID);

    // Registra callbacks
    central.aoEncontrarModulo(aoEncontrarModulo);
    central.aoReceberStatus(aoReceberStatus);
    central.aoModuloOffline(aoModuloOffline);

    // Heartbeat a cada 15s
    central.configurarHeartbeat(15000);

    // Inicia descoberta — módulos próximos têm 5s para responder
    central.iniciarDescoberta(5000);

    // Lista módulos já conhecidos do NVS
    uint8_t total = central.registro().total();
    if (total > 0) {
        debugInfo("Modulos ja registrados no NVS: " + String(total));
        for (uint8_t i = 0; i < total; i++) {
            InfoModulo& m = central.registro().obterPorIndice(i);
            debugInfo("  [" + String(m.id) + "] " + String(m.label) +
                      " — " + RegistroModulos::nomeTipo(m.tipo));
        }
    }
}

// ---------------------------------------------------------------------------
// loop()
// ---------------------------------------------------------------------------

void loop() {
    central.atualizar();

    // Reinicia descoberta manualmente via Serial (envie 'd' + Enter)
    if (Serial.available()) {
        char c = Serial.read();
        while (Serial.available()) Serial.read();

        if (c == 'd' || c == 'D') {
            debugInfo("Reiniciando descoberta...");
            central.iniciarDescoberta(5000);
        } else if (c == 'l' || c == 'L') {
            // Lista módulos registrados
            uint8_t total = central.registro().total();
            debugInfo("Modulos registrados: " + String(total));
            for (uint8_t i = 0; i < total; i++) {
                InfoModulo& m = central.registro().obterPorIndice(i);
                debugInfo("  [" + String(m.id) + "] " + String(m.label) +
                          " — " + RegistroModulos::nomeTipo(m.tipo) +
                          " — " + (m.online ? "ONLINE" : "offline"));
            }
        } else if (c == 'x' || c == 'X') {
            debugAviso("Limpando todos os modulos...");
            central.limparModulos();
        } else if (c == 'h' || c == 'H') {
            central.enviarHeartbeat();
        }
    }
}
