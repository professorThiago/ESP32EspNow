/**
 * @file Mestre.ino
 * @brief Exemplo de dispositivo mestre com descoberta e troca de mensagens.
 *
 * ## O que este exemplo faz
 *
 * 1. Inicializa o ESP-NOW como mestre.
 * 2. Inicia descoberta — escravos próximos têm 5 s para responder.
 * 3. Registra automaticamente todos os escravos encontrados no NVS.
 * 4. A cada 5 s, envia uma mensagem para todos os escravos registrados.
 * 5. Exibe no Serial Monitor cada mensagem recebida dos escravos.
 * 6. Heartbeat a cada 15 s detecta escravos offline.
 *
 * ## Comandos no Serial Monitor (115200 baud)
 *
 * | Char | Ação                          |
 * |------|-------------------------------|
 * | `d`  | Reiniciar descoberta          |
 * | `l`  | Listar dispositivos           |
 * | `h`  | Enviar heartbeat manualmente  |
 * | `x`  | Limpar registro               |
 *
 * @author  professorThiago
 * @version 2.0.0
 */

#include <ESP32EspNow.h>

// ---------------------------------------------------------------------------
// Tipos de dispositivo definidos PELO PROJETO (não pela biblioteca)
// ---------------------------------------------------------------------------
enum MeuTipo : uint8_t {
    TIPO_SENSOR   = 1,
    TIPO_ATUADOR  = 2,
    TIPO_DISPLAY  = 3,
};

// ---------------------------------------------------------------------------
// Payload definido PELO PROJETO
// ---------------------------------------------------------------------------

/** Comando enviado do mestre para os escravos */
struct __attribute__((packed)) CmdMestre {
    uint8_t  acao;    // 0=ping, 1=ligar, 2=desligar, 3=ler
    uint8_t  valor;
    uint32_t sequencia;
};

/** Resposta enviada do escravo para o mestre */
struct __attribute__((packed)) RespostaEscravo {
    uint8_t  status;   // 0=ok, 1=erro
    uint32_t leitura;
    uint32_t sequencia;
};

// ---------------------------------------------------------------------------
// Instância global do mestre
// ---------------------------------------------------------------------------
EspNowMestre mestre;

uint32_t _seq = 0;

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

void aoEncontrar(const Dispositivo& d) {
    Serial.println("\n[+] Novo dispositivo encontrado:");
    Serial.println("    Label : " + String(d.label));
    Serial.println("    Tipo  : " + String(d.tipoDispositivo));
    Serial.println("    MAC   : " + RegistroDispositivos::macParaString(d.mac));
    Serial.println("    FW    : v" + String(d.versaoFirmware));

    if (mestre.registrar(d)) {
        Serial.println("    >>> Registrado com sucesso (ID: " +
                       String(mestre.registro().porMac(d.mac)->id) + ")");
    }
}

void aoReceber(const Dispositivo& d, const uint8_t* dados, uint8_t tam) {
    Serial.print("[MSG] De '" + String(d.label) + "' (" + String(tam) + " bytes)");

    if (tam == sizeof(RespostaEscravo)) {
        RespostaEscravo r;
        memcpy(&r, dados, sizeof(r));
        Serial.println(" | status=" + String(r.status) +
                       " | leitura=" + String(r.leitura) +
                       " | seq=" + String(r.sequencia));
    } else {
        // Payload desconhecido — imprime em hex
        Serial.print(" | hex: ");
        for (uint8_t i = 0; i < tam; i++) {
            if (dados[i] < 0x10) Serial.print('0');
            Serial.print(dados[i], HEX);
            Serial.print(' ');
        }
        Serial.println();
    }
}

void aoOffline(const Dispositivo& d) {
    Serial.println("[!] OFFLINE: " + String(d.label));
}

void aoOnline(const Dispositivo& d) {
    Serial.println("[+] ONLINE:  " + String(d.label));
}

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n=== ESP32EspNow — Exemplo Mestre ===\n");

    // Inicia (WIFI_STA sem conectar a nenhuma rede)
    if (!mestre.begin("meu-sistema")) {
        Serial.println("ERRO: falha ao iniciar ESP-NOW!");
        while (true) delay(1000);
    }

    Serial.println("MAC: " + mestre.mac());
    Serial.println("Canal: " + String(mestre.canal()));

    // Registra callbacks
    mestre.aoEncontrarDispositivo(aoEncontrar);
    mestre.aoReceberMensagem(aoReceber);
    mestre.aoFicarOffline(aoOffline);
    mestre.aoVoltarOnline(aoOnline);

    // Heartbeat a cada 15 s
    mestre.configurarHeartbeat(15000);

    // Lista dispositivos salvos no NVS
    uint8_t total = mestre.registro().total();
    Serial.println("Dispositivos no NVS: " + String(total));
    for (uint8_t i = 0; i < total; i++) {
        Dispositivo& d = mestre.registro().porIndice(i);
        Serial.println("  [" + String(d.id) + "] " + String(d.label) +
                       " tipo=" + String(d.tipoDispositivo));
    }

    // Inicia descoberta
    Serial.println("\nIniciando descoberta (5 s)...\n");
    mestre.iniciarDescoberta(5000);
}

// ---------------------------------------------------------------------------
// loop()
// ---------------------------------------------------------------------------
void loop() {
    mestre.atualizar();

    // Envia mensagem a todos a cada 5 s
    static uint32_t tEnvio = 0;
    if (millis() - tEnvio > 5000 && mestre.registro().total() > 0) {
        tEnvio = millis();
        CmdMestre cmd;
        cmd.acao      = 3;       // ler
        cmd.valor     = 0;
        cmd.sequencia = ++_seq;

        uint8_t n = mestre.enviarParaTodos(&cmd, sizeof(cmd));
        Serial.println("[>] Cmd seq=" + String(_seq) +
                       " enviado para " + String(n) + " dispositivo(s)");
    }

    // Comandos via Serial Monitor
    if (Serial.available()) {
        char c = Serial.read();
        while (Serial.available()) Serial.read();

        switch (c) {
            case 'd': case 'D':
                Serial.println("\nReiniciando descoberta...");
                mestre.iniciarDescoberta(5000);
                break;

            case 'l': case 'L':
                Serial.println("\nDispositivos registrados: " +
                               String(mestre.registro().total()));
                for (uint8_t i = 0; i < mestre.registro().total(); i++) {
                    Dispositivo& d = mestre.registro().porIndice(i);
                    Serial.println("  [" + String(d.id) + "] " + String(d.label) +
                                   " | tipo=" + String(d.tipoDispositivo) +
                                   " | " + (d.online ? "ONLINE" : "offline") +
                                   " | MAC: " + RegistroDispositivos::macParaString(d.mac));
                }
                break;

            case 'h': case 'H':
                Serial.println("Enviando heartbeat...");
                mestre.enviarHeartbeat();
                break;

            case 'x': case 'X':
                Serial.println("Limpando registro...");
                mestre.limpar();
                break;
        }
    }
}
