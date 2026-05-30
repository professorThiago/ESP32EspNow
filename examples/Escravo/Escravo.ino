/**
 * @file Escravo.ino
 * @brief Exemplo de dispositivo escravo que responde ao mestre.
 *
 * ## O que este exemplo faz
 *
 * 1. Inicializa o ESP-NOW como escravo (tipo=1, label="sensor-01").
 * 2. Aguarda o PING de descoberta do mestre.
 * 3. Ao receber um comando, processa e envia resposta.
 * 4. Responde automaticamente ao heartbeat do mestre.
 *
 * @author  professorThiago
 * @version 2.0.0
 */

#include <ESP32EspNow.h>

// ---------------------------------------------------------------------------
// Payload definido PELO PROJETO — deve ser igual no mestre e no escravo
// ---------------------------------------------------------------------------

struct __attribute__((packed)) CmdMestre {
    uint8_t  acao;
    uint8_t  valor;
    uint32_t sequencia;
};

struct __attribute__((packed)) RespostaEscravo {
    uint8_t  status;
    uint32_t leitura;
    uint32_t sequencia;
};

// ---------------------------------------------------------------------------
// Tipo de dispositivo definido pelo projeto
// ---------------------------------------------------------------------------
static constexpr uint8_t TIPO_SENSOR = 1;

// ---------------------------------------------------------------------------
// Instância global do escravo
// ---------------------------------------------------------------------------
EspNowEscravo escravo;

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== ESP32EspNow — Exemplo Escravo ===\n");

    if (!escravo.begin(TIPO_SENSOR, "sensor-01")) {
        Serial.println("ERRO: falha ao iniciar ESP-NOW!");
        while (true) delay(1000);
    }

    Serial.println("MAC: " + escravo.mac());
    Serial.println("Aguardando PING do mestre...\n");

    // Callback quando o mestre é identificado pela primeira vez
    escravo.aoConectarMestre([](const uint8_t* mac, bool ok) {
        Serial.println("[+] Mestre identificado: " +
                       RegistroDispositivos::macParaString(mac));
    });

    // Callback de mensagem recebida do mestre
    escravo.aoReceberMensagem([](const uint8_t* dados, uint8_t tam) {
        if (tam < sizeof(CmdMestre)) {
            Serial.println("[!] Payload menor que esperado (" +
                           String(tam) + " bytes)");
            return;
        }

        CmdMestre cmd;
        memcpy(&cmd, dados, sizeof(cmd));

        Serial.println("[<] Cmd acao=" + String(cmd.acao) +
                       " valor=" + String(cmd.valor) +
                       " seq=" + String(cmd.sequencia));

        // Monta resposta
        RespostaEscravo resp;
        resp.sequencia = cmd.sequencia;

        switch (cmd.acao) {
            case 0:  // ping
                resp.status  = 0;
                resp.leitura = 0;
                break;
            case 1:  // ligar
                resp.status  = 0;
                resp.leitura = 1;
                Serial.println("    -> Ligando...");
                break;
            case 2:  // desligar
                resp.status  = 0;
                resp.leitura = 0;
                Serial.println("    -> Desligando...");
                break;
            case 3:  // ler
                resp.status  = 0;
                resp.leitura = analogRead(34); // leitura de exemplo
                Serial.println("    -> Leitura: " + String(resp.leitura));
                break;
            default:
                resp.status  = 1; // erro: ação desconhecida
                resp.leitura = 0;
                break;
        }

        // Envia resposta ao mestre
        if (escravo.responder(&resp, sizeof(resp))) {
            Serial.println("[>] Resposta enviada (seq=" +
                           String(resp.sequencia) + ")");
        }
    });

    // Callback de envio (opcional — útil para debug)
    escravo.aoEnviar([](const uint8_t* mac, bool ok) {
        if (!ok) Serial.println("[!] Falha ao enviar resposta ao mestre.");
    });
}

// ---------------------------------------------------------------------------
// loop()
// ---------------------------------------------------------------------------
void loop() {
    escravo.atualizar();  // processa recepção e responde heartbeats
}
