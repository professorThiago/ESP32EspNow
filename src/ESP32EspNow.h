/**
 * @file ESP32EspNow.h
 * @brief Gerenciamento completo de ESP-NOW para ESP32.
 *
 * @details
 * Biblioteca independente para sistemas com um dispositivo **central** e
 * múltiplos **módulos** periféricos comunicando-se via ESP-NOW.
 *
 * ### Funcionalidades
 *
 * | Recurso                    | Central | Módulo |
 * |----------------------------|:-------:|:------:|
 * | Descoberta automática      | ✔       | ✔      |
 * | Registro de peers (NVS)    | ✔       | —      |
 * | Envio de comandos          | ✔       | ✔      |
 * | Recepção com buffer ISR    | ✔       | ✔      |
 * | Heartbeat periódico        | ✔       | ✔      |
 * | Status de conectividade    | ✔       | ✔      |
 *
 * ### Fluxo de descoberta
 *
 * ```
 * Central                                    Módulo
 *   |--- PING broadcast ------------------>  |
 *   |                                         |--- PONG (tipo + label + fw) -->|
 *   |<-- receberPong() callback ------------ |
 *   |--- registrarModulo() (NVS) ----------  |
 * ```
 *
 * ### Protocolo de mensagens
 *
 * Todo pacote começa com um `CabecalhoMsg` de 7 bytes:
 * ```
 * [ versao | tipo | moduloId | timestamp(4) ]
 * ```
 * Seguido pelo payload específico de cada tipo de mensagem.
 *
 * ### Uso básico — Central
 *
 * @code
 * #include <ESP32EspNow.h>
 *
 * EspNowCentral central;
 *
 * void setup() {
 *     configurarDebug(DEBUG_INFO, -1);
 *     central.begin("sala-01");
 *
 *     central.aoReceberStatus([](const InfoModulo& mod, const MsgStatus& s) {
 *         debugInfo("Status de: " + String(mod.label));
 *     });
 *     central.aoEncontrarModulo([](const InfoModulo& mod) {
 *         debugInfo("Novo modulo: " + String(mod.label));
 *         central.registrarModulo(mod);
 *     });
 *     central.iniciarDescoberta();
 * }
 *
 * void loop() {
 *     central.atualizar();
 * }
 * @endcode
 *
 * ### Uso básico — Módulo
 *
 * @code
 * #include <ESP32EspNow.h>
 *
 * EspNowModulo modulo;
 *
 * void setup() {
 *     configurarDebug(DEBUG_INFO, -1);
 *     modulo.begin(TipoModulo::LUZES_RELE, "LUZ-01");
 *
 *     modulo.aoReceberComando([](const uint8_t* dados, uint8_t tamanho) {
 *         // processar comando
 *     });
 * }
 *
 * void loop() {
 *     modulo.atualizar();
 * }
 * @endcode
 *
 * @author  professorThiago (https://github.com/professorThiago)
 * @version 1.0.0
 * @date    2025
 * @license MIT
 *
 * @par Licença MIT
 * Copyright (c) 2025 professorThiago\n
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:\n
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.\n
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 */

#ifndef ESP32_ESP_NOW_H
#define ESP32_ESP_NOW_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>

// ---------------------------------------------------------------------------
// Dependências internas
// ---------------------------------------------------------------------------
#include "DebugManager.h"

// ---------------------------------------------------------------------------
// Limites configuráveis — redefina antes do #include para personalizar
// ---------------------------------------------------------------------------

/** @brief Número máximo de módulos que o central pode registrar. */
#ifndef ESPNOW_MAX_MODULOS
  #define ESPNOW_MAX_MODULOS  20
#endif

/** @brief Tamanho máximo do label de um módulo (inclui '\0'). */
#ifndef ESPNOW_MAX_LABEL
  #define ESPNOW_MAX_LABEL    17
#endif

/** @brief Tamanho máximo do salaId do central (inclui '\0'). */
#ifndef ESPNOW_MAX_SALA_ID
  #define ESPNOW_MAX_SALA_ID  17
#endif

/** @brief Versão do protocolo. Bump ao alterar qualquer struct de mensagem. */
#define ESPNOW_VERSAO_PROTOCOLO  1

/** @brief Timeout padrão da descoberta em milissegundos. */
#define ESPNOW_TIMEOUT_DESCOBERTA_MS  5000

/** @brief Intervalo padrão de heartbeat em milissegundos. */
#define ESPNOW_INTERVALO_HEARTBEAT_MS  30000

// ---------------------------------------------------------------------------
// Tipos de módulo
// ---------------------------------------------------------------------------

/**
 * @brief Identifica o tipo de equipamento controlado pelo módulo.
 *
 * Usado no PONG de descoberta e no StatusReport para que o central
 * saiba como interpretar o payload de estado.
 */
enum class TipoModulo : uint8_t {
    DESCONHECIDO    = 0,
    AR_CONDICIONADO = 1,  ///< IR Fujitsu ARRAH2E
    PROJETOR_IR     = 2,  ///< IR Epson (protocolo NEC Extended)
    TV_LG_IR        = 3,  ///< IR LG NEC 32-bit
    TELA_RF433      = 4,  ///< RF 433 MHz protocolo TelaProjecaoRF
    LUZES_RELE      = 5,  ///< Relé de estado sólido GPIO
};

// ---------------------------------------------------------------------------
// Tipos de mensagem
// ---------------------------------------------------------------------------

/**
 * @brief Identifica o conteúdo de cada pacote ESP-NOW.
 */
enum class TipoMsg : uint8_t {
    // ── Descoberta ────────────────────────────────────────
    PING            = 0x01,  ///< Central → broadcast: quem está aqui?
    PONG            = 0x02,  ///< Módulo → central: sou eu, tipo X, label Y

    // ── Comandos (central → módulo) ───────────────────────
    CMD_AC          = 0x10,  ///< Comando para ar condicionado
    CMD_PROJETOR    = 0x11,  ///< Comando para projetor
    CMD_TV          = 0x12,  ///< Comando para TV LG
    CMD_TELA        = 0x13,  ///< Comando para tela RF 433
    CMD_LUZES       = 0x14,  ///< Comando para lâmpadas

    // ── Feedback (módulo → central) ───────────────────────
    STATUS          = 0x20,  ///< Relatório de estado do módulo

    // ── Controle ──────────────────────────────────────────
    PING_HEARTBEAT  = 0xF0,  ///< Verificação de conectividade
    PONG_HEARTBEAT  = 0xF1,  ///< Resposta ao heartbeat
};

// ---------------------------------------------------------------------------
// Estruturas de mensagem
// ---------------------------------------------------------------------------

/**
 * @brief Cabeçalho presente em TODOS os pacotes ESP-NOW.
 *
 * @note Usar `__attribute__((packed))` é obrigatório para garantir que o
 *       layout de bytes seja idêntico entre diferentes compiladores e
 *       firmwares. Sem isso, o alinhamento pode variar e corromper os dados.
 */
struct __attribute__((packed)) CabecalhoMsg {
    uint8_t  versao    = ESPNOW_VERSAO_PROTOCOLO; ///< Versão do protocolo
    TipoMsg  tipo;                                 ///< Tipo da mensagem
    uint8_t  moduloId  = 0;                        ///< ID lógico (0 = não cadastrado)
    uint32_t timestamp = 0;                        ///< millis() do remetente
};

/**
 * @brief Pacote PING — enviado pelo central em broadcast na descoberta.
 *
 * Não possui payload extra. Módulos que recebem este pacote devem
 * responder com um MsgPong.
 */
struct __attribute__((packed)) MsgPing {
    CabecalhoMsg cabecalho;
};

/**
 * @brief Pacote PONG — resposta do módulo ao PING de descoberta.
 *
 * Contém as informações de identificação do módulo para que o central
 * possa registrá-lo.
 */
struct __attribute__((packed)) MsgPong {
    CabecalhoMsg cabecalho;
    TipoModulo   tipo;                          ///< Tipo do módulo
    uint8_t      versaoFirmware;                ///< Versão do firmware do módulo
    char         label[ESPNOW_MAX_LABEL];       ///< Label legível (ex: "AC-01")
};

/**
 * @brief Comando para o módulo de ar condicionado.
 *
 * `mascanaCanal`: bit0 = unidade 1, bit1 = unidade 2 (dois ACs no mesmo módulo).
 */
struct __attribute__((packed)) MsgCmdAc {
    CabecalhoMsg cabecalho;
    uint8_t      mascaraCanal;    ///< Bitmask das unidades: 1=AC1, 2=AC2, 3=ambos
    uint8_t      ligar;           ///< 0=desligar, 1=ligar
    uint8_t      temperatura;     ///< 16–30 °C
    uint8_t      modo;            ///< 0=cool, 1=heat, 2=fan, 3=auto
    uint8_t      velocidadeFan;   ///< 0=quiet, 1=low, 2=med, 3=high, 4=auto
};

/**
 * @brief Comando para o módulo de projetor Epson.
 *
 * O campo `comando` recebe diretamente um `EPSON_CMD_*` da biblioteca EpsonIR.
 */
struct __attribute__((packed)) MsgCmdProjetor {
    CabecalhoMsg cabecalho;
    uint8_t      comando;  ///< EPSON_CMD_* (ex: EPSON_CMD_POWER = 0x09)
};

/**
 * @brief Comando para o módulo de TV LG.
 */
struct __attribute__((packed)) MsgCmdTv {
    CabecalhoMsg cabecalho;
    uint8_t      comando;
    ///< 0=power, 1=vol+, 2=vol-, 3=mute, 4=HDMI1, 5=HDMI2, 6=AV, 7=screenshare
};

/**
 * @brief Comando para o módulo de tela de projeção RF 433 MHz.
 *
 * `mascaraCanal` bits 0–1 indicam o slot de endereço RF (0, 1 ou 2).
 * `comando` 0xFF activa o modo learn para o slot indicado.
 */
struct __attribute__((packed)) MsgCmdTela {
    CabecalhoMsg cabecalho;
    uint8_t      mascaraCanal;  ///< bits 0–1 = slot RF (0–2)
    uint8_t      comando;       ///< 0=parar, 1=cima, 2=baixo, 3=learn, 0xFF=startLearn
};

/**
 * @brief Comando para o módulo de lâmpadas (relé de estado sólido).
 */
struct __attribute__((packed)) MsgCmdLuzes {
    CabecalhoMsg cabecalho;
    uint8_t      mascaraCanal;  ///< Bitmask: bit0=ch1, bit1=ch2, bit2=ch3, bit3=ch4
    uint8_t      estado;        ///< 0=desligar, 1=ligar, 2=toggle
};

/**
 * @brief Relatório de estado enviado pelo módulo após executar um comando.
 *
 * `payloadEstado` é um buffer de 8 bytes cujo significado depende do tipo:
 *
 * | TipoModulo         | byte[0]                         | byte[1]         |
 * |--------------------|----------------------------------|-----------------|
 * | AR_CONDICIONADO    | estado AC1 packed               | estado AC2 packed|
 * | PROJETOR_IR        | bit0=on, bit1=freeze, bit2=mute | ultimo cmd byte  |
 * | TV_LG_IR           | bit0=on, bit1=mudo              | volume (0-100)  |
 * | TELA_RF433         | bitmask slots configurados       | 1=aprendendo    |
 * | LUZES_RELE         | bitmask canais ligados           | total canais    |
 */
struct __attribute__((packed)) MsgStatus {
    CabecalhoMsg cabecalho;
    TipoModulo   tipo;
    uint8_t      payloadEstado[8];  ///< Estado específico do tipo de módulo
    int8_t       rssi;              ///< RSSI do módulo em dBm
};

/**
 * @brief Pacote de heartbeat — verificação de conectividade.
 *
 * O central envia PING_HEARTBEAT; o módulo responde com PONG_HEARTBEAT.
 */
struct __attribute__((packed)) MsgHeartbeat {
    CabecalhoMsg cabecalho;
};

// Verificação em tempo de compilação — ESP-NOW limita a 250 bytes por pacote
static_assert(sizeof(MsgCmdAc)      <= 250, "MsgCmdAc excede 250 bytes");
static_assert(sizeof(MsgCmdProjetor)<= 250, "MsgCmdProjetor excede 250 bytes");
static_assert(sizeof(MsgCmdTv)      <= 250, "MsgCmdTv excede 250 bytes");
static_assert(sizeof(MsgCmdTela)    <= 250, "MsgCmdTela excede 250 bytes");
static_assert(sizeof(MsgCmdLuzes)   <= 250, "MsgCmdLuzes excede 250 bytes");
static_assert(sizeof(MsgStatus)     <= 250, "MsgStatus excede 250 bytes");

// ---------------------------------------------------------------------------
// Informações de um módulo registrado
// ---------------------------------------------------------------------------

/**
 * @brief Representa um módulo periférico conhecido pelo central.
 *
 * Instâncias desta struct são salvas no NVS e carregadas no boot.
 */
struct InfoModulo {
    uint8_t    mac[6];                  ///< Endereço MAC do módulo
    TipoModulo tipo;                    ///< Tipo do módulo
    uint8_t    id;                      ///< ID lógico sequencial (1–ESPNOW_MAX_MODULOS)
    uint8_t    versaoFirmware;          ///< Versão do firmware reportada no PONG
    char       label[ESPNOW_MAX_LABEL]; ///< Label legível (ex: "PROJ-02")
    bool       online;                  ///< true se respondeu ao último heartbeat
    uint32_t   ultimoContato;           ///< millis() do último pacote recebido
};

// ---------------------------------------------------------------------------
// Tipos de callback
// ---------------------------------------------------------------------------

/** @brief Callback chamado quando o central recebe um STATUS de um módulo. */
typedef void (*CallbackStatus)(const InfoModulo& modulo, const MsgStatus& status);

/** @brief Callback chamado quando um novo módulo é encontrado na descoberta. */
typedef void (*CallbackModuloEncontrado)(const InfoModulo& modulo);

/** @brief Callback chamado quando um módulo registrado fica offline (sem heartbeat). */
typedef void (*CallbackModuloOffline)(const InfoModulo& modulo);

/** @brief Callback chamado quando o módulo recebe um comando do central. */
typedef void (*CallbackComando)(const uint8_t* dados, uint8_t tamanho);

/** @brief Callback chamado após cada envio ESP-NOW (sucesso ou falha). */
typedef void (*CallbackEnvio)(const uint8_t* mac, bool sucesso);

// =============================================================================
// RegistroModulos — gerência persistente de peers no NVS
// =============================================================================

/**
 * @brief Gerencia o cadastro persistente de módulos no NVS.
 *
 * Usada internamente pelo `EspNowCentral`, mas pode ser acessada
 * diretamente para consultas e manutenção do registro.
 *
 * @par Exemplo
 * @code
 * RegistroModulos registro;
 * registro.begin();
 *
 * // Listar todos os módulos
 * for (uint8_t i = 0; i < registro.total(); i++) {
 *     InfoModulo m = registro.obterPorIndice(i);
 *     debugInfo(String(m.label) + " — " + macParaString(m.mac));
 * }
 * @endcode
 */
class RegistroModulos {
public:
    /**
     * @brief Inicializa o registro e carrega os módulos salvos no NVS.
     *
     * @param nsNVS  Namespace NVS. Padrão: `"espnow_reg"`.
     */
    void begin(const char* nsNVS = "espnow_reg");

    /**
     * @brief Adiciona um módulo ao registro e salva no NVS.
     *
     * Se já existir um módulo com o mesmo MAC, atualiza o registro.
     *
     * @param modulo  Informações do módulo a registrar.
     * @return true   se registrado com sucesso.
     * @return false  se o registro estiver cheio (ESPNOW_MAX_MODULOS).
     */
    bool adicionar(const InfoModulo& modulo);

    /**
     * @brief Remove um módulo pelo ID lógico e atualiza o NVS.
     *
     * @param id  ID lógico do módulo (1–ESPNOW_MAX_MODULOS).
     * @return true   se removido.
     */
    bool remover(uint8_t id);

    /**
     * @brief Remove todos os módulos e limpa o NVS.
     */
    void limpar();

    /**
     * @brief Busca um módulo pelo MAC.
     *
     * @param mac  Array de 6 bytes com o endereço MAC.
     * @return Ponteiro para o InfoModulo, ou nullptr se não encontrado.
     */
    InfoModulo* buscarPorMac(const uint8_t mac[6]);

    /**
     * @brief Busca um módulo pelo ID lógico.
     *
     * @param id  ID lógico (1–ESPNOW_MAX_MODULOS).
     * @return Ponteiro para o InfoModulo, ou nullptr se não encontrado.
     */
    InfoModulo* buscarPorId(uint8_t id);

    /**
     * @brief Busca módulos por tipo.
     *
     * @param tipo    Tipo de módulo a buscar.
     * @param saida   Array que receberá os ponteiros encontrados.
     * @param maxSaida Capacidade do array `saida`.
     * @return Quantidade de módulos encontrados.
     *
     * @par Exemplo
     * @code
     * InfoModulo* acs[6];
     * uint8_t n = registro.buscarPorTipo(TipoModulo::AR_CONDICIONADO, acs, 6);
     * for (uint8_t i = 0; i < n; i++) {
     *     debugInfo("AC encontrado: " + String(acs[i]->label));
     * }
     * @endcode
     */
    uint8_t buscarPorTipo(TipoModulo tipo,
                          InfoModulo** saida,
                          uint8_t maxSaida);

    /**
     * @brief Retorna o módulo pelo índice no array interno (0-based).
     *
     * Útil para iterar sobre todos os módulos registrados.
     *
     * @param indice  Índice de 0 a `total()-1`.
     */
    InfoModulo& obterPorIndice(uint8_t indice);

    /** @brief Retorna o número total de módulos registrados. */
    uint8_t total() const { return _total; }

    /** @brief Retorna true se o registro está cheio. */
    bool cheio() const { return _total >= ESPNOW_MAX_MODULOS; }

    /** @brief Salva todos os módulos no NVS. */
    void salvar();

    /** @brief Carrega os módulos do NVS. */
    void carregar();

    /**
     * @brief Converte um MAC de 6 bytes para string legível.
     *
     * @param mac  Array de 6 bytes.
     * @return String no formato "AA:BB:CC:DD:EE:FF".
     *
     * @par Exemplo
     * @code
     * debugInfo("MAC: " + RegistroModulos::macParaString(modulo.mac));
     * @endcode
     */
    static String macParaString(const uint8_t mac[6]);

    /**
     * @brief Retorna o nome legível de um TipoModulo.
     *
     * @par Exemplo
     * @code
     * debugInfo(RegistroModulos::nomeTipo(TipoModulo::PROJETOR_IR)); // "PROJETOR_IR"
     * @endcode
     */
    static const char* nomeTipo(TipoModulo tipo);

private:
    InfoModulo  _modulos[ESPNOW_MAX_MODULOS];
    uint8_t     _total   = 0;
    uint8_t     _proxId  = 1;
    Preferences _prefs;
    char        _ns[16]  = "espnow_reg";

    void   _salvarModulo(uint8_t indice);
    void   _carregarModulo(uint8_t indice);
    uint8_t _gerarId();
};

// =============================================================================
// EspNowCentral — dispositivo mestre
// =============================================================================

/**
 * @brief Gerencia o ESP-NOW do lado do dispositivo central.
 *
 * O central é o único dispositivo conectado ao WiFi/MQTT. Ele descobre,
 * registra e se comunica com todos os módulos periféricos via ESP-NOW.
 *
 * @note WiFi e ESP-NOW coexistem em `WIFI_STA`. O canal ESP-NOW é fixado
 *       automaticamente ao canal do WiFi após a conexão.
 *
 * @par Uso completo
 * @code
 * EspNowCentral central;
 *
 * void setup() {
 *     configurarDebug(DEBUG_INFO, -1);
 *
 *     // Inicia ESP-NOW (WiFi já deve estar em WIFI_STA)
 *     central.begin("sala-01");
 *
 *     // Callbacks
 *     central.aoEncontrarModulo([](const InfoModulo& m) {
 *         debugInfo("Encontrado: " + String(m.label));
 *         central.registrarModulo(m);   // salva no NVS + adiciona peer
 *     });
 *     central.aoReceberStatus([](const InfoModulo& m, const MsgStatus& s) {
 *         debugInfo("Status de " + String(m.label) +
 *                   " — RSSI: " + String(s.rssi));
 *     });
 *     central.aoModuloOffline([](const InfoModulo& m) {
 *         debugAviso(String(m.label) + " ficou offline!");
 *     });
 *
 *     central.iniciarDescoberta();
 * }
 *
 * void loop() {
 *     central.atualizar();
 *
 *     // Enviar comando de exemplo a cada 10s
 *     static uint32_t t = 0;
 *     if (millis() - t > 10000) {
 *         t = millis();
 *         MsgCmdLuzes cmd;
 *         cmd.mascaraCanal = 0x0F;   // todos os 4 canais
 *         cmd.estado       = 2;       // toggle
 *         central.enviarComando(TipoModulo::LUZES_RELE, "LUZ-01", cmd);
 *     }
 * }
 * @endcode
 */
class EspNowCentral {
public:
    // -----------------------------------------------------------------------
    // Inicialização
    // -----------------------------------------------------------------------

    /**
     * @brief Inicializa o ESP-NOW no modo central.
     *
     * Deve ser chamado após `WiFi.mode(WIFI_STA)` e, de preferência, após
     * a conexão WiFi estar estabelecida (para fixar o canal correto).
     *
     * @param salaId   Identificador desta sala (ex: "sala-01").
     *                 Usado no cabeçalho dos PINGs.
     * @param nsNVS    Namespace NVS para o registro de módulos.
     *                 Padrão: `"espnow_reg"`.
     * @return true    se o ESP-NOW foi inicializado com sucesso.
     *
     * @par Exemplo
     * @code
     * WiFi.mode(WIFI_STA);
     * WiFi.begin("rede", "senha");
     * while (WiFi.status() != WL_CONNECTED) delay(100);
     * central.begin("sala-01");
     * @endcode
     */
    bool begin(const char* salaId, const char* nsNVS = "espnow_reg");

    /**
     * @brief Avança as máquinas de estado e processa mensagens recebidas.
     *
     * **Deve ser chamado em todo `loop()`.**
     * Processa o buffer de recepção (ISR-safe), verifica timeouts de
     * descoberta e dispara verificações de heartbeat.
     */
    void atualizar();

    // -----------------------------------------------------------------------
    // Descoberta de módulos
    // -----------------------------------------------------------------------

    /**
     * @brief Inicia uma rodada de descoberta via broadcast PING.
     *
     * Envia um PING para FF:FF:FF:FF:FF:FF e aguarda PONGs por
     * `timeoutMs` milissegundos. Módulos não cadastrados aparecem
     * no callback `aoEncontrarModulo()`.
     *
     * @param timeoutMs  Tempo de escuta após o PING (padrão: 5000 ms).
     *
     * @par Exemplo
     * @code
     * central.iniciarDescoberta(3000);   // descoberta de 3s
     * @endcode
     */
    void iniciarDescoberta(uint32_t timeoutMs = ESPNOW_TIMEOUT_DESCOBERTA_MS);

    /**
     * @brief Verifica se há uma descoberta em andamento.
     *
     * @return true  se ainda dentro do timeout de descoberta.
     */
    bool descobertaAtiva() const;

    // -----------------------------------------------------------------------
    // Gerenciamento de módulos
    // -----------------------------------------------------------------------

    /**
     * @brief Registra um módulo encontrado: adiciona ao NVS e ao ESP-NOW.
     *
     * Chamado tipicamente dentro do callback `aoEncontrarModulo()`.
     *
     * @param modulo  InfoModulo recebido no callback.
     * @return true   se registrado com sucesso.
     *
     * @par Exemplo
     * @code
     * central.aoEncontrarModulo([](const InfoModulo& m) {
     *     central.registrarModulo(m);
     * });
     * @endcode
     */
    bool registrarModulo(const InfoModulo& modulo);

    /**
     * @brief Remove um módulo do registro e do ESP-NOW pelo ID lógico.
     *
     * @param id  ID lógico do módulo.
     * @return true   se removido.
     */
    bool removerModulo(uint8_t id);

    /**
     * @brief Remove todos os módulos registrados.
     */
    void limparModulos();

    /**
     * @brief Acesso direto ao registro de módulos para consultas avançadas.
     *
     * @return Referência para o RegistroModulos interno.
     *
     * @par Exemplo
     * @code
     * uint8_t total = central.registro().total();
     * InfoModulo* m = central.registro().buscarPorId(3);
     * @endcode
     */
    RegistroModulos& registro() { return _registro; }

    // -----------------------------------------------------------------------
    // Envio de comandos
    // -----------------------------------------------------------------------

    /**
     * @brief Envia um comando para um módulo específico pelo MAC.
     *
     * @tparam T      Tipo da struct de comando (MsgCmdAc, MsgCmdProjetor, etc.).
     * @param mac     MAC de 6 bytes do módulo destino.
     * @param cmd     Struct de comando preenchida.
     * @return true   se o envio foi aceito pela pilha ESP-NOW.
     *
     * @par Exemplo
     * @code
     * uint8_t mac[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
     * MsgCmdAc cmd;
     * cmd.ligar = 1; cmd.temperatura = 22;
     * central.enviarPorMac(mac, cmd);
     * @endcode
     */
    template<typename T>
    bool enviarPorMac(const uint8_t mac[6], const T& cmd) {
        return _enviar(mac, reinterpret_cast<const uint8_t*>(&cmd), sizeof(T));
    }

    /**
     * @brief Envia um comando para um módulo pelo ID lógico.
     *
     * @tparam T   Tipo da struct de comando.
     * @param id   ID lógico do módulo (atribuído no registro).
     * @param cmd  Struct de comando preenchida.
     * @return true   se o módulo foi encontrado e o envio aceito.
     *
     * @par Exemplo
     * @code
     * MsgCmdLuzes cmd;
     * cmd.mascaraCanal = 0x03;  // canais 1 e 2
     * cmd.estado = 1;            // ligar
     * central.enviarPorId(2, cmd);
     * @endcode
     */
    template<typename T>
    bool enviarPorId(uint8_t id, const T& cmd) {
        InfoModulo* m = _registro.buscarPorId(id);
        if (!m) {
            debugAviso("[EspNowCentral] ID " + String(id) + " nao encontrado.");
            return false;
        }
        return enviarPorMac(m->mac, cmd);
    }

    /**
     * @brief Envia um comando para um módulo pelo label.
     *
     * @tparam T      Tipo da struct de comando.
     * @param label   Label do módulo (ex: "LUZ-01").
     * @param cmd     Struct de comando preenchida.
     * @return true   se o módulo foi encontrado e o envio aceito.
     *
     * @par Exemplo
     * @code
     * MsgCmdProjetor cmd;
     * cmd.comando = EPSON_CMD_FREEZE;
     * central.enviarComando("PROJ-01", cmd);
     * @endcode
     */
    template<typename T>
    bool enviarComando(const char* label, const T& cmd) {
        for (uint8_t i = 0; i < _registro.total(); i++) {
            InfoModulo& m = _registro.obterPorIndice(i);
            if (strncmp(m.label, label, ESPNOW_MAX_LABEL) == 0) {
                return enviarPorMac(m.mac, cmd);
            }
        }
        debugAviso("[EspNowCentral] Label '" + String(label) + "' nao encontrado.");
        return false;
    }

    /**
     * @brief Envia um comando para TODOS os módulos de um tipo.
     *
     * Útil para comandos de broadcast como "desligar todos os ACs".
     *
     * @tparam T    Tipo da struct de comando.
     * @param tipo  TipoModulo alvo.
     * @param cmd   Struct de comando preenchida.
     * @return Número de módulos para os quais o envio foi aceito.
     *
     * @par Exemplo
     * @code
     * MsgCmdLuzes cmd;
     * cmd.mascaraCanal = 0x0F;
     * cmd.estado = 0;  // desligar tudo
     * uint8_t enviados = central.enviarParaTodos(TipoModulo::LUZES_RELE, cmd);
     * debugInfo("Enviado para " + String(enviados) + " modulos de luzes");
     * @endcode
     */
    template<typename T>
    uint8_t enviarParaTodos(TipoModulo tipo, const T& cmd) {
        InfoModulo* encontrados[ESPNOW_MAX_MODULOS];
        uint8_t n = _registro.buscarPorTipo(tipo, encontrados, ESPNOW_MAX_MODULOS);
        uint8_t ok = 0;
        for (uint8_t i = 0; i < n; i++) {
            if (enviarPorMac(encontrados[i]->mac, cmd)) ok++;
        }
        return ok;
    }

    // -----------------------------------------------------------------------
    // Heartbeat
    // -----------------------------------------------------------------------

    /**
     * @brief Configura o intervalo de heartbeat automático.
     *
     * O central envia PING_HEARTBEAT para todos os módulos registrados
     * periodicamente. Módulos que não respondem dentro de `3 × intervalo`
     * são marcados como offline e disparam `aoModuloOffline()`.
     *
     * @param intervaloMs  Intervalo em ms. 0 desabilita o heartbeat.
     *                     Padrão: ESPNOW_INTERVALO_HEARTBEAT_MS (30 s).
     *
     * @par Exemplo
     * @code
     * central.configurarHeartbeat(10000);   // verifica a cada 10s
     * central.configurarHeartbeat(0);       // desabilita
     * @endcode
     */
    void configurarHeartbeat(uint32_t intervaloMs = ESPNOW_INTERVALO_HEARTBEAT_MS);

    /**
     * @brief Envia heartbeat imediatamente para todos os módulos registrados.
     */
    void enviarHeartbeat();

    // -----------------------------------------------------------------------
    // Callbacks
    // -----------------------------------------------------------------------

    /**
     * @brief Registra callback para quando um novo módulo é encontrado.
     *
     * Disparado durante a descoberta ao receber um PONG de um módulo
     * ainda não cadastrado no registro.
     *
     * @param cb  Função `void cb(const InfoModulo& modulo)`.
     */
    void aoEncontrarModulo(CallbackModuloEncontrado cb) { _cbEncontrado = cb; }

    /**
     * @brief Registra callback para quando um STATUS é recebido.
     *
     * @param cb  Função `void cb(const InfoModulo& modulo, const MsgStatus& status)`.
     */
    void aoReceberStatus(CallbackStatus cb) { _cbStatus = cb; }

    /**
     * @brief Registra callback para quando um módulo fica offline.
     *
     * @param cb  Função `void cb(const InfoModulo& modulo)`.
     */
    void aoModuloOffline(CallbackModuloOffline cb) { _cbOffline = cb; }

    /**
     * @brief Registra callback chamado após cada envio ESP-NOW.
     *
     * @param cb  Função `void cb(const uint8_t* mac, bool sucesso)`.
     */
    void aoEnviar(CallbackEnvio cb) { _cbEnvio = cb; }

    // -----------------------------------------------------------------------
    // Utilitários
    // -----------------------------------------------------------------------

    /** @brief Retorna o MAC do central como string "AA:BB:CC:DD:EE:FF". */
    String mac() const { return RegistroModulos::macParaString(_mac); }

    /** @brief Retorna o canal WiFi/ESP-NOW atual. */
    uint8_t canal() const;

private:
    RegistroModulos _registro;

    char    _salaId[ESPNOW_MAX_SALA_ID] = {};
    uint8_t _mac[6] = {};

    // Descoberta
    bool     _descobertaAtiva  = false;
    uint32_t _inicioDescoberta = 0;
    uint32_t _timeoutDescoberta = ESPNOW_TIMEOUT_DESCOBERTA_MS;

    // Heartbeat
    uint32_t _intervaloHeartbeat = ESPNOW_INTERVALO_HEARTBEAT_MS;
    uint32_t _ultimoHeartbeat    = 0;

    // Buffer ISR → loop
    volatile bool   _temDados   = false;
    uint8_t         _bufferRx[250];
    volatile int    _tamanhoRx  = 0;
    uint8_t         _macRx[6]   = {};

    // Callbacks
    CallbackModuloEncontrado _cbEncontrado = nullptr;
    CallbackStatus           _cbStatus     = nullptr;
    CallbackModuloOffline    _cbOffline    = nullptr;
    CallbackEnvio            _cbEnvio      = nullptr;

    // Singleton para callbacks estáticos do ESP-NOW
    static EspNowCentral* _instancia;
    static void _cbRecvEstatico(const esp_now_recv_info_t* info,
                                const uint8_t* dados, int tamanho);
    static void _cbSendEstatico(const uint8_t* mac,
                                esp_now_send_status_t status);

    // Internos
    void _processarBuffer();
    void _processarPong(const uint8_t* macOrigem, const MsgPong& pong);
    void _processarStatus(const uint8_t* macOrigem, const MsgStatus& status);
    void _processarHeartbeatPong(const uint8_t* macOrigem);
    void _verificarModulosOffline();
    void _verificarTimeoutDescoberta();
    bool _adicionarPeer(const uint8_t mac[6]);
    bool _enviar(const uint8_t mac[6], const uint8_t* dados, uint8_t tamanho);
};

// =============================================================================
// EspNowModulo — dispositivo periférico
// =============================================================================

/**
 * @brief Gerencia o ESP-NOW do lado do módulo periférico.
 *
 * O módulo não tem WiFi ativo — usa apenas ESP-NOW. Ao receber um PING
 * de descoberta do central, responde com um PONG contendo seu tipo e label.
 * Após isso, fica aguardando comandos e envia STATUS em resposta.
 *
 * @par Uso completo
 * @code
 * EspNowModulo modulo;
 *
 * void setup() {
 *     configurarDebug(DEBUG_INFO, -1);
 *     modulo.begin(TipoModulo::LUZES_RELE, "LUZ-01");
 *
 *     modulo.aoReceberComando([](const uint8_t* dados, uint8_t tam) {
 *         if (tam < sizeof(MsgCmdLuzes)) return;
 *         MsgCmdLuzes cmd;
 *         memcpy(&cmd, dados, sizeof(cmd));
 *         // executa o comando
 *         luzes.executar(cmd);
 *         // reporta estado de volta
 *         uint8_t estado[8];
 *         luzes.obterEstado(estado);
 *         modulo.reportarStatus(estado);
 *     });
 * }
 *
 * void loop() {
 *     modulo.atualizar();
 * }
 * @endcode
 */
class EspNowModulo {
public:
    // -----------------------------------------------------------------------
    // Inicialização
    // -----------------------------------------------------------------------

    /**
     * @brief Inicializa o ESP-NOW no modo módulo.
     *
     * Configura `WIFI_STA` sem conectar a nenhuma rede. O canal é fixo
     * e deve coincidir com o do central (ambos devem usar o mesmo canal
     * ou `channel=0` para auto).
     *
     * @param tipo           TipoModulo deste dispositivo.
     * @param label          Label legível (ex: "AC-02"). Máx: ESPNOW_MAX_LABEL-1 chars.
     * @param versaoFirmware Versão do firmware deste módulo (padrão: 1).
     * @return true          se o ESP-NOW foi inicializado com sucesso.
     *
     * @par Exemplo
     * @code
     * modulo.begin(TipoModulo::PROJETOR_IR, "PROJ-01");
     * modulo.begin(TipoModulo::AR_CONDICIONADO, "AC-02", 2);
     * @endcode
     */
    bool begin(TipoModulo tipo, const char* label, uint8_t versaoFirmware = 1);

    /**
     * @brief Avança a máquina de estados e processa mensagens.
     *
     * **Deve ser chamado em todo `loop()`.**
     */
    void atualizar();

    // -----------------------------------------------------------------------
    // Comunicação com o central
    // -----------------------------------------------------------------------

    /**
     * @brief Envia um relatório de estado ao central.
     *
     * Deve ser chamado após executar qualquer comando para dar feedback
     * ao central (e por consequência ao MQTT).
     *
     * @param payloadEstado  Array de 8 bytes com o estado atual.
     *                       O significado depende do TipoModulo.
     * @return true          se o envio foi aceito e o central está registrado.
     *
     * @par Exemplo
     * @code
     * uint8_t estado[8] = {};
     * estado[0] = mascaraCanaIsLigados;
     * modulo.reportarStatus(estado);
     * @endcode
     */
    bool reportarStatus(const uint8_t payloadEstado[8]);

    /**
     * @brief Verifica se o central já foi identificado (respondeu ao PING).
     *
     * @return true  se o MAC do central está registrado.
     */
    bool centralConectado() const { return _centralRegistrado; }

    /**
     * @brief Retorna o MAC do central como string, ou "" se não conectado.
     */
    String macCentral() const;

    // -----------------------------------------------------------------------
    // Callbacks
    // -----------------------------------------------------------------------

    /**
     * @brief Registra callback chamado quando um comando é recebido.
     *
     * O callback recebe o buffer bruto e seu tamanho. Use `memcpy` para
     * copiar para a struct de comando adequada.
     *
     * @param cb  Função `void cb(const uint8_t* dados, uint8_t tamanho)`.
     *
     * @note O callback é chamado fora da ISR (no `atualizar()`), portanto
     *       é seguro usar `Serial`, `Preferences`, etc.
     */
    void aoReceberComando(CallbackComando cb) { _cbComando = cb; }

    /**
     * @brief Registra callback chamado após cada envio ESP-NOW.
     *
     * @param cb  Função `void cb(const uint8_t* mac, bool sucesso)`.
     */
    void aoEnviar(CallbackEnvio cb) { _cbEnvio = cb; }

    // -----------------------------------------------------------------------
    // Utilitários
    // -----------------------------------------------------------------------

    /** @brief Retorna o MAC deste módulo como string. */
    String mac() const { return RegistroModulos::macParaString(_mac); }

    /** @brief Retorna o ID lógico atribuído pelo central (0 = não cadastrado). */
    uint8_t id() const { return _id; }

    /** @brief Retorna o label configurado. */
    const char* label() const { return _label; }

    /** @brief Retorna o tipo deste módulo. */
    TipoModulo tipo() const { return _tipo; }

private:
    TipoModulo _tipo    = TipoModulo::DESCONHECIDO;
    char       _label[ESPNOW_MAX_LABEL] = {};
    uint8_t    _versaoFirmware = 1;
    uint8_t    _id      = 0;
    uint8_t    _mac[6]  = {};

    uint8_t    _macCentral[6]   = {};
    bool       _centralRegistrado = false;

    // Buffer ISR → loop
    volatile bool  _temDados   = false;
    uint8_t        _bufferRx[250];
    volatile int   _tamanhoRx  = 0;
    uint8_t        _macRx[6]   = {};

    // Callbacks
    CallbackComando _cbComando = nullptr;
    CallbackEnvio   _cbEnvio   = nullptr;

    // Singleton para callbacks estáticos
    static EspNowModulo* _instancia;
    static void _cbRecvEstatico(const esp_now_recv_info_t* info,
                                const uint8_t* dados, int tamanho);
    static void _cbSendEstatico(const uint8_t* mac,
                                esp_now_send_status_t status);

    // Internos
    void _processarBuffer();
    void _responderPing(const uint8_t* macCentral);
    void _responderHeartbeat(const uint8_t* macCentral);
    void _registrarCentral(const uint8_t* mac);
    bool _adicionarPeer(const uint8_t mac[6]);
    bool _enviar(const uint8_t mac[6], const uint8_t* dados, uint8_t tamanho);
};

#endif // ESP32_ESP_NOW_H
