/**
 * @file Escravo.ino
 * @brief Exemplo de escravo com endereçamento por jumper.
 *
 * O escravo SÓ responde ao mestre do mesmo endereço.
 * Comente configurarEnderecoFisico() para modo sem isolamento.
 *
 * @author  professorThiago  @version 2.1.0
 */

#include <ESP32EspNow.h>

const uint8_t JUMPERS[ESPNOW_BITS_ENDERECO] = {4, 5, 6, 7, 8, 9};

static constexpr uint8_t TIPO_SENSOR = 1;

struct __attribute__((packed)) CmdMestre   { uint8_t acao; uint8_t valor; uint16_t seq; };
struct __attribute__((packed)) RespEscravo { uint8_t status; uint32_t leitura; uint16_t seq; };

EspNowEscravo escravo;

void setup() {
    Serial.begin(115200); delay(500);
    Serial.println("\n=== ESP32EspNow v2.1 — Escravo ===\n");

    if (!escravo.begin(TIPO_SENSOR, "sensor-01")) {
        Serial.println("ERRO: ESP-NOW falhou!"); while (true) delay(1000);
    }

    // Endereçamento por jumper — comente para modo genérico
    if (!escravo.configurarEnderecoFisico(JUMPERS)) {
        Serial.println("ATENCAO: addr=0, modo sem isolamento de sala.");
    }
    // Alternativa: escravo.configurarEnderecoPorSoftware(3);

    Serial.println("MAC  : " + escravo.mac());
    Serial.println("Addr : " + String(escravo.endereco()) +
                   (escravo.endereco() > 0 ? " (filtro ON)" : " (filtro OFF)"));
    Serial.println("Aguardando PING do mestre addr=" +
                   String(escravo.endereco()) + "...\n");

    escravo.aoConectarMestre([](const uint8_t* mac, bool ok) {
        Serial.println("[+] Mestre: " + RegistroDispositivos::macParaString(mac));
    });

    escravo.aoReceberMensagem([](const uint8_t* dados, uint8_t tam) {
        if (tam < sizeof(CmdMestre)) return;

        CmdMestre cmd; memcpy(&cmd, dados, sizeof(cmd));
        Serial.println("[<] acao=" + String(cmd.acao) +
                       " valor=" + String(cmd.valor) +
                       " seq=" + String(cmd.seq));

        RespEscravo r;
        r.seq     = cmd.seq;
        r.status  = 0;
        r.leitura = (cmd.acao == 3) ? (uint32_t)analogRead(34) : 0;

        if (escravo.responder(&r, sizeof(r)))
            Serial.println("[>] Resposta enviada leitura=" + String(r.leitura));
    });

    escravo.aoEnviar([](const uint8_t* mac, bool ok) {
        if (!ok) Serial.println("[!] Falha ao enviar ao mestre.");
    });
}

void loop() {
    escravo.atualizar();
}
