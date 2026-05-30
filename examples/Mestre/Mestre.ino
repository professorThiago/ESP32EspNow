/**
 * @file Mestre.ino
 * @brief Exemplo de dispositivo mestre com endereçamento por jumper.
 *
 * Jumper FECHADO (pino a GND) = bit 1 | Jumper ABERTO (pull-up) = bit 0
 *
 * Exemplo addr=3 (0b000011): bit0=fechado, bit1=fechado, demais=abertos.
 * Com addr=3, só conversa com escravos addr=3.
 *
 * Comente configurarEnderecoFisico() para modo sem isolamento.
 *
 * Comandos Serial: d=descoberta l=listar h=heartbeat x=limpar
 *
 * @author  professorThiago  @version 2.1.0
 */

#include <ESP32EspNow.h>

const uint8_t JUMPERS[ESPNOW_BITS_ENDERECO] = {4, 5, 6, 7, 8, 9};

enum MeuTipo : uint8_t { TIPO_SENSOR = 1, TIPO_ATUADOR = 2 };

struct __attribute__((packed)) CmdMestre   { uint8_t acao; uint8_t valor; uint16_t seq; };
struct __attribute__((packed)) RespEscravo { uint8_t status; uint32_t leitura; uint16_t seq; };

EspNowMestre mestre;
uint16_t _seq = 0;

void aoEncontrar(const Dispositivo& d) {
    Serial.println("\n[+] " + String(d.label) +
                   " tipo=" + String(d.tipoDispositivo) +
                   " addr=" + String(d.enderecoSala) +
                   " MAC=" + RegistroDispositivos::macParaString(d.mac));
    if (mestre.registrar(d))
        Serial.println("    ID=" + String(mestre.registro().porMac(d.mac)->id));
}

void aoReceber(const Dispositivo& d, const uint8_t* dados, uint8_t tam) {
    if (tam != sizeof(RespEscravo)) return;
    RespEscravo r; memcpy(&r, dados, sizeof(r));
    Serial.println("[<] " + String(d.label) +
                   " leitura=" + String(r.leitura) +
                   " seq=" + String(r.seq));
}

void aoOffline(const Dispositivo& d) { Serial.println("[!] OFFLINE: " + String(d.label)); }
void aoOnline (const Dispositivo& d) { Serial.println("[+] ONLINE:  " + String(d.label)); }

void setup() {
    Serial.begin(115200); delay(500);
    Serial.println("\n=== ESP32EspNow v2.1 — Mestre ===\n");

    if (!mestre.begin("meu-sistema")) {
        Serial.println("ERRO: ESP-NOW falhou!"); while (true) delay(1000);
    }

    // Endereçamento por jumper — comente para modo genérico (sem isolamento)
    if (!mestre.configurarEnderecoFisico(JUMPERS)) {
        Serial.println("ATENCAO: addr=0, modo sem isolamento de sala.");
    }
    // Alternativa por software: mestre.configurarEnderecoPorSoftware(3);

    Serial.println("MAC  : " + mestre.mac());
    Serial.println("Addr : " + String(mestre.endereco()) +
                   (mestre.endereco() > 0 ? " (filtro ON)" : " (filtro OFF)"));

    mestre.aoEncontrarDispositivo(aoEncontrar);
    mestre.aoReceberMensagem(aoReceber);
    mestre.aoFicarOffline(aoOffline);
    mestre.aoVoltarOnline(aoOnline);
    mestre.configurarHeartbeat(15000);

    Serial.println("NVS  : " + String(mestre.registro().total()) + " dispositivo(s)");
    for (uint8_t i = 0; i < mestre.registro().total(); i++) {
        Dispositivo& d = mestre.registro().porIndice(i);
        Serial.println("  [" + String(d.id) + "] " + String(d.label) +
                       " addr=" + String(d.enderecoSala));
    }

    Serial.println("\nDescoberta iniciada (5s)...");
    mestre.iniciarDescoberta(5000);
}

void loop() {
    mestre.atualizar();

    static uint32_t tEnvio = 0;
    if (millis() - tEnvio > 5000 && mestre.registro().total() > 0) {
        tEnvio = millis();
        CmdMestre cmd = {3, 0, ++_seq};
        uint8_t n = mestre.enviarParaTodos(&cmd, sizeof(cmd));
        Serial.println("[>] seq=" + String(_seq) + " -> " + String(n) + " escravo(s)");
    }

    if (Serial.available()) {
        char c = Serial.read();
        while (Serial.available()) Serial.read();
        switch (c) {
            case 'd': mestre.iniciarDescoberta(5000); break;
            case 'l':
                for (uint8_t i = 0; i < mestre.registro().total(); i++) {
                    Dispositivo& d = mestre.registro().porIndice(i);
                    Serial.println("[" + String(d.id) + "] " + String(d.label) +
                                   " addr=" + String(d.enderecoSala) +
                                   " " + (d.online ? "ONLINE" : "offline"));
                } break;
            case 'h': mestre.enviarHeartbeat(); break;
            case 'x': mestre.limpar(); Serial.println("Registro limpo."); break;
        }
    }
}
