/**
 * @file MestreEscravo.ino
 * @brief Mesmo arquivo compila como mestre ou escravo via flag de compilação.
 *
 * ## Como usar
 *
 * Para compilar como mestre, adicione no `platformio.ini`:
 * ```ini
 * [env:mestre]
 * build_flags = -DROLE_MESTRE
 *
 * [env:escravo]
 * ; sem flag — compila como escravo por padrão
 * ```
 *
 * ## Protocolo de exemplo — sistema de monitoramento
 *
 * Este exemplo implementa um sistema de monitoramento onde:
 * - O mestre solicita leituras a cada 3 s
 * - Os escravos respondem com uma leitura de ADC simulada
 * - O mestre exibe as leituras no Serial Monitor
 *
 * @author  professorThiago
 * @version 2.0.0
 */

#include <ESP32EspNow.h>

// ---------------------------------------------------------------------------
// Protocolo de aplicação — definido pelo projeto, não pela biblioteca
// ---------------------------------------------------------------------------

/** Tipos de dispositivo neste projeto */
enum TipoDisp : uint8_t {
    TIPO_MONITOR = 1,
};

/** Requisição do mestre */
struct __attribute__((packed)) Requisicao {
    uint8_t  tipo;    // 0=solicitar leitura, 1=configurar
    uint8_t  canal;
    uint16_t seq;
};

/** Leitura do escravo */
struct __attribute__((packed)) Leitura {
    uint8_t  canal;
    int16_t  valor;
    uint8_t  unidade; // 0=raw, 1=celsius, 2=pct
    uint16_t seq;
};

// =============================================================================
#ifdef ROLE_MESTRE
// =============================================================================

EspNowMestre mestre;
uint16_t _seq = 0;

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[MESTRE] Iniciando...");

    mestre.begin("monitor-v1");

    mestre.aoEncontrarDispositivo([](const Dispositivo& d) {
        Serial.println("[MESTRE] Encontrado: " + String(d.label));
        mestre.registrar(d);
    });

    mestre.aoReceberMensagem([](const Dispositivo& d,
                                 const uint8_t* dados, uint8_t tam) {
        if (tam != sizeof(Leitura)) return;
        Leitura l;
        memcpy(&l, dados, sizeof(l));
        Serial.printf("[MESTRE] %s | canal=%d valor=%d seq=%d\n",
                      d.label, l.canal, l.valor, l.seq);
    });

    mestre.aoFicarOffline([](const Dispositivo& d) {
        Serial.println("[MESTRE] OFFLINE: " + String(d.label));
    });

    mestre.configurarHeartbeat(20000);
    mestre.iniciarDescoberta(5000);
    Serial.println("[MESTRE] MAC: " + mestre.mac());
}

void loop() {
    mestre.atualizar();

    static uint32_t tReq = 0;
    if (millis() - tReq > 3000 && mestre.registro().total() > 0) {
        tReq = millis();
        Requisicao req = {0, 0, ++_seq};
        uint8_t n = mestre.enviarParaTodos(&req, sizeof(req));
        Serial.println("[MESTRE] Req seq=" + String(_seq) +
                       " → " + String(n) + " escravo(s)");
    }
}

// =============================================================================
#else  // ROLE_ESCRAVO (padrão)
// =============================================================================

EspNowEscravo escravo;

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[ESCRAVO] Iniciando...");

    escravo.begin(TIPO_MONITOR, "monitor-01");

    escravo.aoConectarMestre([](const uint8_t* mac, bool ok) {
        Serial.println("[ESCRAVO] Mestre: " +
                       RegistroDispositivos::macParaString(mac));
    });

    escravo.aoReceberMensagem([](const uint8_t* dados, uint8_t tam) {
        if (tam != sizeof(Requisicao)) return;

        Requisicao req;
        memcpy(&req, dados, sizeof(req));

        if (req.tipo == 0) {
            // Simula leitura do canal solicitado
            Leitura l;
            l.canal  = req.canal;
            l.valor  = (int16_t)(analogRead(34) / 4); // 0–1023 → 0–255
            l.unidade = 0;
            l.seq    = req.seq;

            escravo.responder(&l, sizeof(l));
            Serial.printf("[ESCRAVO] Leitura canal=%d val=%d seq=%d\n",
                          l.canal, l.valor, l.seq);
        }
    });

    Serial.println("[ESCRAVO] MAC: " + escravo.mac());
    Serial.println("[ESCRAVO] Aguardando mestre...");
}

void loop() {
    escravo.atualizar();
}

#endif
