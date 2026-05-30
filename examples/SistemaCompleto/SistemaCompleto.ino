/**
 * @file SistemaCompleto.ino
 * @brief Integração completa: central com ESP32Connectivity + ESP32EspNow.
 *
 * ## O que este exemplo faz
 *
 * Demonstra o fluxo completo do sistema de automação de sala de aula:
 *
 * **Central (compile com `#define ROLE_CENTRAL`):**
 * 1. Conecta ao WiFi e MQTT via ESP32Connectivity.
 * 2. Descobre módulos via ESP-NOW.
 * 3. Recebe comandos JSON do MQTT e roteia para o módulo correto via ESP-NOW.
 * 4. Recebe STATUS dos módulos via ESP-NOW e publica no MQTT.
 *
 * **Módulo de lâmpadas (compile sem `#define ROLE_CENTRAL`):**
 * 1. Aguarda PING de descoberta do central.
 * 2. Responde com PONG (tipo LUZES_RELE, label "LUZ-01").
 * 3. Recebe MsgCmdLuzes e simula execução.
 * 4. Reporta estado de volta ao central.
 *
 * ## Compilação
 *
 * Para compilar como central, adicione no `platformio.ini`:
 * ```ini
 * build_flags = -DROLE_CENTRAL
 * ```
 * Sem essa flag, compila como módulo.
 *
 * ## Tópicos MQTT (central)
 *
 * ```
 * Recebe:  classroom/sala-01/cmd/luzes/1   {"mascaraCanal":15,"estado":1}
 * Publica: classroom/sala-01/status/luzes/1  {"canal0":1,"canal1":1,"canal2":1,"canal3":1}
 * Publica: classroom/sala-01/heartbeat       {"ts":123456,"modulos":3}
 * ```
 *
 * @author  professorThiago
 * @version 1.0.0
 */

#include <ESP32EspNow.h>

// ---------------------------------------------------------------------------
// Definição de papel: central ou módulo
// Adicione -DROLE_CENTRAL no platformio.ini para compilar como central
// ---------------------------------------------------------------------------

// #define ROLE_CENTRAL   // descomente para testar como central sem platformio.ini

// =============================================================================
#ifdef ROLE_CENTRAL
// =============================================================================

#include <ESP32Connectivity.h>
#include <ArduinoJson.h>

// ── Configurações ──────────────────────────────────────────────────────────

const char* WIFI_SSID  = "Escola-WiFi";
const char* WIFI_SENHA = "senha_escola";
const char* SALA_ID    = "sala-01";

const char* TOPICOS_PUB[] = {
    "classroom/sala-01/status/luzes/+",
    "classroom/sala-01/status/ac/+",
    "classroom/sala-01/status/projetor/+",
    "classroom/sala-01/heartbeat",
};
const char* TOPICOS_REC[] = {
    "classroom/sala-01/cmd/+/+",
    "classroom/all/cmd/+",
    "classroom/sala-01/aviso",
    "classroom/all/aviso",
};

ConfigWiFi   wifiCfg  = { WIFI_SSID, WIFI_SENHA };
ConfigMQTT   mqttCfg  = { "broker.escola.br", 1883, "central-sala-01", "", "" };
ConfigTopicos topicos  = { TOPICOS_PUB, 4, TOPICOS_REC, 4 };

// ── Instâncias ─────────────────────────────────────────────────────────────

EspNowCentral central;

// ── Roteamento MQTT → ESP-NOW ───────────────────────────────────────────────

void processarComandoMQTT(const char* topico, const String& payload) {
    debugInfo("MQTT recebido: " + String(topico));
    debugTudo("Payload: " + payload);

    // Parseia tópico: classroom/{sala}/cmd/{tipo}/{id}
    // Ex: classroom/sala-01/cmd/luzes/1
    char buf[64];
    strncpy(buf, topico, sizeof(buf) - 1);

    // Extrai tipo e ID do tópico
    char* parte = strtok(buf, "/");
    int   campo = 0;
    char  tipo[16] = {};
    char  idStr[8] = {};

    while (parte) {
        if (campo == 3) strncpy(tipo,  parte, sizeof(tipo)  - 1);
        if (campo == 4) strncpy(idStr, parte, sizeof(idStr) - 1);
        parte = strtok(nullptr, "/");
        campo++;
    }

    uint8_t id = atoi(idStr);

    // ── Comando de lâmpadas ───────────────────────────────────────────────
    if (strcmp(tipo, "luzes") == 0) {
        StaticJsonDocument<64> doc;
        if (deserializeJson(doc, payload) != DeserializationError::Ok) {
            debugErro("JSON invalido no comando luzes.");
            return;
        }
        MsgCmdLuzes cmd;
        _preencherCabecalhoHelper(cmd.cabecalho, TipoMsg::CMD_LUZES, id);
        cmd.mascaraCanal = doc["mascaraCanal"] | 0x0F;
        cmd.estado       = doc["estado"]       | 0;

        uint8_t enviados = 0;
        if (id == 0) {
            // id=0 → broadcast para todos do tipo
            enviados = central.enviarParaTodos(TipoModulo::LUZES_RELE, cmd);
        } else {
            enviados = central.enviarPorId(id, cmd) ? 1 : 0;
        }
        debugInfo("Cmd luzes enviado para " + String(enviados) + " modulo(s).");
    }

    // ── Comando de AC ─────────────────────────────────────────────────────
    else if (strcmp(tipo, "ac") == 0) {
        StaticJsonDocument<96> doc;
        deserializeJson(doc, payload);
        MsgCmdAc cmd;
        _preencherCabecalhoHelper(cmd.cabecalho, TipoMsg::CMD_AC, id);
        cmd.mascaraCanal  = doc["mascaraCanal"]  | 3;
        cmd.ligar         = doc["ligar"]         | 0;
        cmd.temperatura   = doc["temperatura"]   | 24;
        cmd.modo          = doc["modo"]          | 0;
        cmd.velocidadeFan = doc["velocidadeFan"] | 0;
        central.enviarPorId(id, cmd);
    }

    // ── Comando de projetor ───────────────────────────────────────────────
    else if (strcmp(tipo, "projetor") == 0) {
        StaticJsonDocument<32> doc;
        deserializeJson(doc, payload);
        MsgCmdProjetor cmd;
        _preencherCabecalhoHelper(cmd.cabecalho, TipoMsg::CMD_PROJETOR, id);
        cmd.comando = doc["comando"] | 0x09; // EPSON_CMD_POWER por padrão
        central.enviarPorId(id, cmd);
    }
}

// Helper para preencher cabeçalho sem expor o detalhe interno
void _preencherCabecalhoHelper(CabecalhoMsg& cab, TipoMsg tipo, uint8_t id) {
    cab.versao    = ESPNOW_VERSAO_PROTOCOLO;
    cab.tipo      = tipo;
    cab.moduloId  = id;
    cab.timestamp = millis();
}

// ── Publicação de STATUS no MQTT ───────────────────────────────────────────

void publicarStatus(const InfoModulo& modulo, const MsgStatus& status) {
    char topico[64];
    const char* tipoStr = "";
    switch (modulo.tipo) {
        case TipoModulo::LUZES_RELE:      tipoStr = "luzes";    break;
        case TipoModulo::AR_CONDICIONADO: tipoStr = "ac";       break;
        case TipoModulo::PROJETOR_IR:     tipoStr = "projetor"; break;
        case TipoModulo::TV_LG_IR:        tipoStr = "tv";       break;
        case TipoModulo::TELA_RF433:      tipoStr = "tela";     break;
        default:                          tipoStr = "desconhecido"; break;
    }

    snprintf(topico, sizeof(topico),
             "classroom/%s/status/%s/%d",
             SALA_ID, tipoStr, modulo.id);

    // Monta JSON de status
    char payload[128];
    if (modulo.tipo == TipoModulo::LUZES_RELE) {
        snprintf(payload, sizeof(payload),
                 "{\"canal0\":%d,\"canal1\":%d,\"canal2\":%d,\"canal3\":%d,\"rssi\":%d}",
                 (status.payloadEstado[0] >> 0) & 1,
                 (status.payloadEstado[0] >> 1) & 1,
                 (status.payloadEstado[0] >> 2) & 1,
                 (status.payloadEstado[0] >> 3) & 1,
                 status.rssi);
    } else {
        snprintf(payload, sizeof(payload),
                 "{\"estado\":%d,\"rssi\":%d}",
                 status.payloadEstado[0], status.rssi);
    }

    conectividade.publicar(topico, payload);
}

// ── setup() central ────────────────────────────────────────────────────────

void setup() {
    configurarDebug(DEBUG_INFO, 0);

    // WiFi + MQTT via ESP32Connectivity
    conectividade.beginSimples(wifiCfg, mqttCfg, topicos);
    conectividade.configurarBufferMQTT(512);
    conectividade.registrarCallbackMensagem(processarComandoMQTT);
    conectividade.registrarCallbackMQTTConectado([]() {
        debugInfo("MQTT conectado — iniciando descoberta ESP-NOW.");
        central.iniciarDescoberta(5000);
    });

    // ESP-NOW central (após WiFi para fixar canal)
    // Aguarda WiFi conectar antes de iniciar ESP-NOW
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        delay(200);
    }

    central.begin(SALA_ID);

    central.aoEncontrarModulo([](const InfoModulo& m) {
        debugInfo("Novo modulo: " + String(m.label));
        central.registrarModulo(m);

        // Publica descoberta no MQTT
        char msg[64];
        snprintf(msg, sizeof(msg), "{\"label\":\"%s\",\"tipo\":\"%s\"}",
                 m.label, RegistroModulos::nomeTipo(m.tipo));
        conectividade.publicar(
            ("classroom/" + String(SALA_ID) + "/discovery").c_str(), msg);
    });

    central.aoReceberStatus(publicarStatus);

    central.aoModuloOffline([](const InfoModulo& m) {
        debugAviso("Modulo offline: " + String(m.label));
        char msg[48];
        snprintf(msg, sizeof(msg), "{\"label\":\"%s\",\"online\":false}", m.label);
        conectividade.publicar(
            ("classroom/" + String(SALA_ID) + "/offline").c_str(), msg);
    });

    central.configurarHeartbeat(30000);
    debugInfo("Central pronto.");
}

// ── loop() central ─────────────────────────────────────────────────────────

void loop() {
    conectividade.update();   // WiFi + MQTT
    central.atualizar();      // ESP-NOW + heartbeat

    // Heartbeat MQTT a cada 30s
    static uint32_t tHb = 0;
    if (millis() - tHb > 30000) {
        tHb = millis();
        char payload[48];
        snprintf(payload, sizeof(payload),
                 "{\"ts\":%lu,\"modulos\":%d}",
                 millis(), central.registro().total());
        conectividade.publicar(
            ("classroom/" + String(SALA_ID) + "/heartbeat").c_str(), payload,
            false  // não enfileirar — heartbeat é descartável
        );
    }
}

// =============================================================================
#else  // ROLE_MODULO — módulo de lâmpadas
// =============================================================================

EspNowModulo modulo;

// Estado simulado das 4 lâmpadas (bitmask)
uint8_t estadoLuzes = 0x00;

void executarCmdLuzes(const MsgCmdLuzes& cmd) {
    for (uint8_t ch = 0; ch < 4; ch++) {
        if (!(cmd.mascaraCanal & (1 << ch))) continue;

        switch (cmd.estado) {
            case 0:  estadoLuzes &= ~(1 << ch); break;  // desligar
            case 1:  estadoLuzes |=  (1 << ch); break;  // ligar
            case 2:  estadoLuzes ^=  (1 << ch); break;  // toggle
        }
    }

    debugInfo("Luzes: 0x" + String(estadoLuzes, HEX) +
              " | CH1:" + ((estadoLuzes >> 0) & 1) +
              " CH2:" + ((estadoLuzes >> 1) & 1) +
              " CH3:" + ((estadoLuzes >> 2) & 1) +
              " CH4:" + ((estadoLuzes >> 3) & 1));
}

void setup() {
    configurarDebug(DEBUG_INFO, -1);
    modulo.begin(TipoModulo::LUZES_RELE, "LUZ-01");

    modulo.aoReceberComando([](const uint8_t* dados, uint8_t tam) {
        if (tam < sizeof(CabecalhoMsg)) return;

        CabecalhoMsg cab;
        memcpy(&cab, dados, sizeof(cab));

        if (cab.tipo == TipoMsg::CMD_LUZES && tam >= sizeof(MsgCmdLuzes)) {
            MsgCmdLuzes cmd;
            memcpy(&cmd, dados, sizeof(cmd));
            executarCmdLuzes(cmd);

            // Reporta estado ao central
            uint8_t estado[8] = {};
            estado[0] = estadoLuzes;
            estado[1] = 4;  // 4 canais disponíveis
            modulo.reportarStatus(estado);
        }
    });

    modulo.aoEnviar([](const uint8_t* mac, bool ok) {
        if (!ok) debugAviso("Falha ao enviar status ao central.");
    });
}

void loop() {
    modulo.atualizar();
}

#endif // ROLE_CENTRAL / ROLE_MODULO
