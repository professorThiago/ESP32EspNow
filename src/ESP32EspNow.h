/**
 * @file ESP32EspNow.h
 * @brief Infraestrutura ESP-NOW mestre/escravo para ESP32.
 *
 * @details
 * Biblioteca genérica para sistemas distribuídos com ESP-NOW. Cuida de toda
 * a infraestrutura de comunicação sem impor nenhum protocolo de aplicação:
 *
 * - **Descoberta automática** — o mestre faz broadcast de PING; escravos respondem
 *   com um PONG que inclui um `tipoDispositivo` e um `label` definidos pelo projeto.
 * - **Registro persistente** — os dispositivos descobertos são salvos no NVS
 *   (flash interno) e recarregados automaticamente no boot.
 * - **Mensagens com payload livre** — o conteúdo das mensagens é um buffer de
 *   até `ESPNOW_MAX_PAYLOAD` bytes. A biblioteca entrega os bytes brutos ao
 *   callback do projeto, que decide como interpretá-los.
 * - **Heartbeat** — verificação periódica de conectividade com detecção de
 *   dispositivos offline.
 *
 * ### O que a biblioteca NÃO faz (responsabilidade do projeto)
 *
 * - Definir o significado de `tipoDispositivo`
 * - Criar structs de comando/status específicas
 * - Rotear mensagens para MQTT, HTTP ou qualquer outro protocolo
 * - Parsear JSON ou qualquer formato de payload
 *
 * ### Relação mestre/escravo
 *
 * ```
 * Mestre (1)                        Escravo (N)
 * ─────────────────────────────     ──────────────────────────────
 * Conectado ao WiFi (opcional)      Sem WiFi (apenas ESP-NOW)
 * Inicia a descoberta               Responde à descoberta
 * Registra peers no NVS             Registra o mestre como peer
 * Envia mensagens a qualquer peer   Envia mensagens apenas ao mestre
 * Recebe de qualquer peer           Recebe apenas do mestre
 * ```
 *
 * ### Exemplo mínimo — Mestre
 *
 * @code
 * #include <ESP32EspNow.h>
 *
 * EspNowMestre mestre;
 *
 * void setup() {
 *     mestre.begin("meu-sistema");
 *
 *     mestre.aoEncontrarDispositivo([](const Dispositivo& d) {
 *         Serial.println("Encontrado: " + String(d.label));
 *         mestre.registrar(d);
 *     });
 *
 *     mestre.aoReceberMensagem([](const Dispositivo& d,
 *                                  const uint8_t* dados, uint8_t tam) {
 *         Serial.println("Mensagem de: " + String(d.label));
 *     });
 *
 *     mestre.iniciarDescoberta();
 * }
 *
 * void loop() { mestre.atualizar(); }
 * @endcode
 *
 * ### Exemplo mínimo — Escravo
 *
 * @code
 * #include <ESP32EspNow.h>
 *
 * EspNowEscravo escravo;
 *
 * void setup() {
 *     escravo.begin(42, "sensor-01");   // tipoDispositivo=42, label="sensor-01"
 *
 *     escravo.aoReceberMensagem([](const uint8_t* dados, uint8_t tam) {
 *         // processar comando recebido do mestre
 *     });
 * }
 *
 * void loop() { escravo.atualizar(); }
 * @endcode
 *
 * @author  professorThiago (https://github.com/professorThiago)
 * @version 2.0.0
 * @date    2025
 * @license MIT
 *
 * @par Licença MIT
 * Copyright (c) 2025 professorThiago\n
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:\n
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.\n
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 */

#ifndef ESP32_ESPNOW_H
#define ESP32_ESPNOW_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>

// ---------------------------------------------------------------------------
// Limites configuráveis — redefina ANTES do #include para personalizar
// sem precisar alterar a biblioteca.
// ---------------------------------------------------------------------------

/**
 * @brief Número máximo de dispositivos que o mestre pode registrar.
 *
 * Limita a memória usada pelo array interno de `Dispositivo`.
 * Padrão: 20.
 *
 * @par Exemplo
 * @code
 * #define ESPNOW_MAX_DISPOSITIVOS 8   // sistema pequeno
 * #include <ESP32EspNow.h>
 * @endcode
 */
#ifndef ESPNOW_MAX_DISPOSITIVOS
  #define ESPNOW_MAX_DISPOSITIVOS  20
#endif

/**
 * @brief Tamanho máximo do label de um dispositivo (inclui terminador `\0`).
 * Padrão: 17 (16 chars + `\0`).
 */
#ifndef ESPNOW_MAX_LABEL
  #define ESPNOW_MAX_LABEL  17
#endif

/**
 * @brief Tamanho máximo do identificador do sistema (inclui `\0`).
 * Padrão: 17.
 */
#ifndef ESPNOW_MAX_SISTEMA_ID
  #define ESPNOW_MAX_SISTEMA_ID  17
#endif

/**
 * @brief Tamanho máximo do payload de mensagem em bytes.
 *
 * O ESP-NOW limita cada pacote a 250 bytes. A biblioteca usa 9 bytes
 * para o cabeçalho interno, restando até 241 bytes para o payload.
 * Padrão: 200 (reserva margem de segurança).
 *
 * @note Reduza este valor se precisar economizar memória RAM, pois
 *       um buffer deste tamanho é alocado na stack para recepção.
 */
#ifndef ESPNOW_MAX_PAYLOAD
  #define ESPNOW_MAX_PAYLOAD  200
#endif

/**
 * @brief Timeout padrão da descoberta em milissegundos.
 * Padrão: 5000 (5 segundos).
 */
#ifndef ESPNOW_TIMEOUT_DESCOBERTA_MS
  #define ESPNOW_TIMEOUT_DESCOBERTA_MS  5000
#endif

/**
 * @brief Intervalo padrão de heartbeat em milissegundos.
 * Padrão: 30000 (30 segundos).
 */
#ifndef ESPNOW_INTERVALO_HEARTBEAT_MS
  #define ESPNOW_INTERVALO_HEARTBEAT_MS  30000
#endif

// ---------------------------------------------------------------------------
// Versão do protocolo interno
// ---------------------------------------------------------------------------

/**
 * @brief Versão do protocolo interno da biblioteca.
 *
 * Pacotes com versão diferente são descartados silenciosamente.
 * Mude este valor apenas ao alterar o layout do cabeçalho interno.
 *
 * @warning Todos os dispositivos de um sistema devem usar a mesma versão.
 */
#define ESPNOW_VERSAO_PROTOCOLO  2

// ---------------------------------------------------------------------------
// Estrutura de dispositivo
// ---------------------------------------------------------------------------

/**
 * @brief Representa um dispositivo remoto conhecido pelo mestre.
 *
 * Instâncias desta struct são salvas no NVS e recarregadas no boot.
 * O campo `tipoDispositivo` e o campo `label` são definidos pelo projeto —
 * a biblioteca não impõe nenhum significado a eles.
 *
 * @par Exemplo de uso no projeto
 * @code
 * // No projeto, defina os tipos:
 * enum MeuTipo : uint8_t { SENSOR_TEMP = 1, ATUADOR_LED = 2 };
 *
 * // Ao receber no callback:
 * mestre.aoEncontrarDispositivo([](const Dispositivo& d) {
 *     if (d.tipoDispositivo == SENSOR_TEMP) { ... }
 * });
 * @endcode
 */
struct Dispositivo {
    uint8_t  mac[6];                     ///< Endereço MAC único do dispositivo
    uint8_t  tipoDispositivo;            ///< Tipo definido pelo projeto (0–255)
    uint8_t  id;                         ///< ID lógico sequencial (atribuído pelo mestre)
    uint8_t  versaoFirmware;             ///< Versão de firmware reportada no PONG
    char     label[ESPNOW_MAX_LABEL];    ///< Nome legível (ex: "sensor-01")
    bool     online;                     ///< true = respondeu ao último heartbeat
    uint32_t ultimoContato;              ///< millis() do último pacote recebido
};

// ---------------------------------------------------------------------------
// Tipos de callback
// ---------------------------------------------------------------------------

/**
 * @brief Callback de mensagem recebida pelo **mestre**.
 *
 * @param dispositivo  Referência ao dispositivo remetente (do registro).
 * @param dados        Ponteiro para os bytes do payload.
 * @param tamanho      Número de bytes no payload.
 */
typedef void (*CallbackMensagemMestre)(const Dispositivo& dispositivo,
                                       const uint8_t*     dados,
                                       uint8_t            tamanho);

/**
 * @brief Callback de mensagem recebida pelo **escravo**.
 *
 * @param dados    Ponteiro para os bytes do payload.
 * @param tamanho  Número de bytes no payload.
 */
typedef void (*CallbackMensagemEscravo)(const uint8_t* dados, uint8_t tamanho);

/**
 * @brief Callback chamado quando um novo dispositivo é encontrado na descoberta.
 *
 * @param dispositivo  Informações do dispositivo encontrado.
 */
typedef void (*CallbackDispositivo)(const Dispositivo& dispositivo);

/**
 * @brief Callback chamado após cada tentativa de envio ESP-NOW.
 *
 * @param mac      MAC do destinatário.
 * @param sucesso  true = entregue; false = sem confirmação.
 */
typedef void (*CallbackEnvio)(const uint8_t* mac, bool sucesso);

// =============================================================================
// RegistroDispositivos
// =============================================================================

/**
 * @brief Gerencia o cadastro persistente de dispositivos no NVS.
 *
 * Usada internamente pelo `EspNowMestre`, mas acessível via
 * `mestre.registro()` para consultas e manutenção avançadas.
 *
 * @par Exemplo
 * @code
 * // Iterar todos os dispositivos registrados
 * for (uint8_t i = 0; i < mestre.registro().total(); i++) {
 *     const Dispositivo& d = mestre.registro().porIndice(i);
 *     Serial.println(String(d.id) + ": " + d.label);
 * }
 * @endcode
 */
class RegistroDispositivos {
public:
    /**
     * @brief Inicializa o registro e carrega dados salvos do NVS.
     *
     * @param nsNVS  Namespace NVS (até 15 chars). Padrão: `"espnow"`.
     */
    void begin(const char* nsNVS = "espnow");

    /**
     * @brief Adiciona ou atualiza um dispositivo no registro.
     *
     * Se já existir um dispositivo com o mesmo MAC, atualiza os campos e
     * salva no NVS. Caso contrário, adiciona uma nova entrada.
     *
     * @param dispositivo  Dados do dispositivo.
     * @return true   se adicionado/atualizado com sucesso.
     * @return false  se o registro estiver cheio (ESPNOW_MAX_DISPOSITIVOS).
     */
    bool adicionar(const Dispositivo& dispositivo);

    /**
     * @brief Remove um dispositivo pelo ID lógico.
     *
     * @param id  ID lógico do dispositivo (atribuído pelo mestre).
     * @return true   se removido com sucesso.
     */
    bool remover(uint8_t id);

    /**
     * @brief Remove todos os dispositivos e limpa o NVS.
     */
    void limpar();

    /**
     * @brief Busca um dispositivo pelo endereço MAC.
     *
     * @param mac  Array de 6 bytes com o MAC.
     * @return Ponteiro para o Dispositivo, ou `nullptr` se não encontrado.
     */
    Dispositivo* porMac(const uint8_t mac[6]);

    /**
     * @brief Busca um dispositivo pelo ID lógico.
     *
     * @param id  ID lógico.
     * @return Ponteiro para o Dispositivo, ou `nullptr` se não encontrado.
     */
    Dispositivo* porId(uint8_t id);

    /**
     * @brief Busca um dispositivo pelo label.
     *
     * @param label  String de label (comparação exata).
     * @return Ponteiro para o Dispositivo, ou `nullptr` se não encontrado.
     */
    Dispositivo* porLabel(const char* label);

    /**
     * @brief Busca dispositivos por tipo.
     *
     * @param tipo      Tipo a buscar (valor definido pelo projeto).
     * @param saida     Array de ponteiros que receberá os resultados.
     * @param maxSaida  Capacidade do array `saida`.
     * @return Número de dispositivos encontrados.
     *
     * @par Exemplo
     * @code
     * Dispositivo* sensores[10];
     * uint8_t n = mestre.registro().porTipo(SENSOR_TEMP, sensores, 10);
     * for (uint8_t i = 0; i < n; i++) { ... }
     * @endcode
     */
    uint8_t porTipo(uint8_t tipo, Dispositivo** saida, uint8_t maxSaida);

    /**
     * @brief Retorna um dispositivo pelo índice no array interno.
     *
     * Útil para iteração sequencial.
     *
     * @param indice  Índice de 0 a `total()-1`.
     */
    Dispositivo& porIndice(uint8_t indice);

    /** @brief Retorna o número de dispositivos registrados. */
    uint8_t total() const { return _total; }

    /** @brief Retorna `true` se o registro atingiu ESPNOW_MAX_DISPOSITIVOS. */
    bool cheio() const { return _total >= ESPNOW_MAX_DISPOSITIVOS; }

    /** @brief Salva todos os dispositivos no NVS. */
    void salvar();

    /** @brief Recarrega todos os dispositivos do NVS. */
    void carregar();

    // -----------------------------------------------------------------------
    // Utilitários estáticos
    // -----------------------------------------------------------------------

    /**
     * @brief Converte um MAC de 6 bytes para string legível.
     *
     * @param mac  Array de 6 bytes.
     * @return String no formato `"AA:BB:CC:DD:EE:FF"`.
     */
    static String macParaString(const uint8_t mac[6]);

    /**
     * @brief Converte uma string `"AA:BB:CC:DD:EE:FF"` para 6 bytes.
     *
     * @param str   String de entrada.
     * @param mac   Array de 6 bytes que receberá o resultado.
     * @return true  se a conversão foi bem-sucedida.
     */
    static bool stringParaMac(const char* str, uint8_t mac[6]);

private:
    Dispositivo _dispositivos[ESPNOW_MAX_DISPOSITIVOS];
    uint8_t     _total   = 0;
    uint8_t     _proxId  = 1;
    Preferences _prefs;
    char        _ns[16]  = "espnow";

    void    _salvar(uint8_t indice);
    void    _carregar(uint8_t indice);
    uint8_t _gerarId();
};

// =============================================================================
// EspNowMestre
// =============================================================================

/**
 * @brief Gerencia o ESP-NOW do lado do dispositivo mestre.
 *
 * O mestre descobre, registra e troca mensagens com escravos remotos.
 * O conteúdo das mensagens (payload) é completamente livre — a biblioteca
 * apenas entrega os bytes brutos ao callback registrado.
 *
 * @par Fluxo típico
 * ```
 * 1. begin()              — inicializa ESP-NOW e carrega registro do NVS
 * 2. aoEncontrarDispositivo() — registra callback de descoberta
 * 3. aoReceberMensagem()  — registra callback de recepção
 * 4. iniciarDescoberta()  — envia PING broadcast e aguarda PONGs
 * 5. atualizar()          — chama no loop() para processar eventos
 * 6. enviar*()            — envia payload a um ou mais dispositivos
 * ```
 *
 * @par Envio de payload
 * O payload é um ponteiro para qualquer struct ou array de bytes.
 * Recomenda-se usar structs `__attribute__((packed))` para garantir
 * layout consistente entre diferentes firmwares.
 *
 * @code
 * struct __attribute__((packed)) ComandoLED {
 *     uint8_t canal;
 *     uint8_t brilho;
 * };
 *
 * ComandoLED cmd = {1, 200};
 * mestre.enviarPorId(3, &cmd, sizeof(cmd));
 * @endcode
 */
class EspNowMestre {
public:
    // -----------------------------------------------------------------------
    // Inicialização
    // -----------------------------------------------------------------------

    /**
     * @brief Inicializa o ESP-NOW no modo mestre.
     *
     * Deve ser chamado após `WiFi.mode(WIFI_STA)`. Se o WiFi estiver
     * conectado, o canal ESP-NOW é fixado automaticamente ao canal do AP.
     *
     * @param sistemaId  Identificador do sistema (ex: "meu-projeto").
     *                   Máx: ESPNOW_MAX_SISTEMA_ID-1 chars.
     * @param nsNVS      Namespace NVS para o registro. Padrão: `"espnow"`.
     * @return true      se o ESP-NOW foi inicializado com sucesso.
     *
     * @par Exemplo
     * @code
     * WiFi.mode(WIFI_STA);
     * mestre.begin("sistema-abc");
     * @endcode
     */
    bool begin(const char* sistemaId, const char* nsNVS = "espnow");

    /**
     * @brief Processa mensagens recebidas, heartbeat e timeout de descoberta.
     *
     * **Deve ser chamado em todo `loop()`.**
     */
    void atualizar();

    // -----------------------------------------------------------------------
    // Descoberta
    // -----------------------------------------------------------------------

    /**
     * @brief Envia um PING broadcast e aguarda respostas de escravos.
     *
     * Escravos disponíveis respondem com um PONG durante o `timeoutMs`.
     * Dispositivos não cadastrados disparam o callback `aoEncontrarDispositivo()`.
     *
     * @param timeoutMs  Janela de escuta após o PING. Padrão: 5000 ms.
     *
     * @par Exemplo
     * @code
     * mestre.iniciarDescoberta(3000);   // janela de 3s
     * @endcode
     */
    void iniciarDescoberta(uint32_t timeoutMs = ESPNOW_TIMEOUT_DESCOBERTA_MS);

    /**
     * @brief Retorna `true` se há uma descoberta em andamento.
     */
    bool descobertaAtiva() const;

    // -----------------------------------------------------------------------
    // Gerenciamento de dispositivos
    // -----------------------------------------------------------------------

    /**
     * @brief Registra um dispositivo: salva no NVS e adiciona como peer ESP-NOW.
     *
     * Normalmente chamado dentro do callback `aoEncontrarDispositivo()`.
     *
     * @param dispositivo  Dispositivo a registrar (recebido no callback).
     * @return true   se registrado com sucesso.
     *
     * @par Exemplo
     * @code
     * mestre.aoEncontrarDispositivo([](const Dispositivo& d) {
     *     mestre.registrar(d);
     * });
     * @endcode
     */
    bool registrar(const Dispositivo& dispositivo);

    /**
     * @brief Remove um dispositivo do registro e do ESP-NOW pelo ID.
     *
     * @param id  ID lógico do dispositivo.
     * @return true   se removido.
     */
    bool remover(uint8_t id);

    /**
     * @brief Remove todos os dispositivos registrados.
     */
    void limpar();

    /**
     * @brief Acesso direto ao registro para consultas avançadas.
     *
     * @return Referência para o RegistroDispositivos interno.
     *
     * @par Exemplo
     * @code
     * uint8_t total = mestre.registro().total();
     * Dispositivo* d = mestre.registro().porLabel("sensor-01");
     * @endcode
     */
    RegistroDispositivos& registro() { return _registro; }

    // -----------------------------------------------------------------------
    // Envio de mensagens
    // -----------------------------------------------------------------------

    /**
     * @brief Envia um payload para um dispositivo pelo MAC.
     *
     * @param mac     MAC de 6 bytes do destinatário.
     * @param dados   Ponteiro para o payload.
     * @param tamanho Tamanho do payload em bytes (máx: ESPNOW_MAX_PAYLOAD).
     * @return true   se aceito pela pilha ESP-NOW.
     *
     * @par Exemplo
     * @code
     * struct Cmd { uint8_t acao; uint8_t valor; };
     * Cmd c = {1, 100};
     * uint8_t mac[] = {0xAA, ...};
     * mestre.enviarPorMac(mac, &c, sizeof(c));
     * @endcode
     */
    bool enviarPorMac(const uint8_t mac[6],
                      const void*   dados,
                      uint8_t       tamanho);

    /**
     * @brief Envia um payload para um dispositivo pelo ID lógico.
     *
     * @param id      ID lógico do dispositivo.
     * @param dados   Ponteiro para o payload.
     * @param tamanho Tamanho em bytes.
     * @return true   se o dispositivo foi encontrado e o envio aceito.
     *
     * @par Exemplo
     * @code
     * mestre.enviarPorId(2, &cmd, sizeof(cmd));
     * @endcode
     */
    bool enviarPorId(uint8_t     id,
                     const void* dados,
                     uint8_t     tamanho);

    /**
     * @brief Envia um payload para um dispositivo pelo label.
     *
     * @param label   Label do dispositivo (ex: "sensor-01").
     * @param dados   Ponteiro para o payload.
     * @param tamanho Tamanho em bytes.
     * @return true   se o dispositivo foi encontrado e o envio aceito.
     *
     * @par Exemplo
     * @code
     * mestre.enviarPorLabel("atuador-01", &cmd, sizeof(cmd));
     * @endcode
     */
    bool enviarPorLabel(const char* label,
                        const void* dados,
                        uint8_t     tamanho);

    /**
     * @brief Envia um payload para todos os dispositivos de um tipo.
     *
     * @param tipo    Tipo de dispositivo (valor definido pelo projeto).
     * @param dados   Ponteiro para o payload.
     * @param tamanho Tamanho em bytes.
     * @return Número de dispositivos para os quais o envio foi aceito.
     *
     * @par Exemplo
     * @code
     * uint8_t n = mestre.enviarParaTipo(SENSOR_TEMP, &cmd, sizeof(cmd));
     * Serial.println("Enviado para " + String(n) + " sensores.");
     * @endcode
     */
    uint8_t enviarParaTipo(uint8_t     tipo,
                           const void* dados,
                           uint8_t     tamanho);

    /**
     * @brief Envia um payload para TODOS os dispositivos registrados.
     *
     * @param dados   Ponteiro para o payload.
     * @param tamanho Tamanho em bytes.
     * @return Número de dispositivos para os quais o envio foi aceito.
     */
    uint8_t enviarParaTodos(const void* dados, uint8_t tamanho);

    // -----------------------------------------------------------------------
    // Heartbeat
    // -----------------------------------------------------------------------

    /**
     * @brief Configura o intervalo de heartbeat automático.
     *
     * O mestre envia um PING_HEARTBEAT para cada dispositivo registrado.
     * Dispositivos que não respondem em `3 × intervalo` são marcados como
     * offline e disparam o callback `aoFicarOffline()`.
     *
     * @param intervaloMs  Intervalo em ms. `0` desabilita. Padrão: 30000 ms.
     *
     * @par Exemplo
     * @code
     * mestre.configurarHeartbeat(10000);  // verifica a cada 10s
     * mestre.configurarHeartbeat(0);      // desabilita
     * @endcode
     */
    void configurarHeartbeat(uint32_t intervaloMs = ESPNOW_INTERVALO_HEARTBEAT_MS);

    /**
     * @brief Envia heartbeat imediatamente para todos os dispositivos.
     */
    void enviarHeartbeat();

    // -----------------------------------------------------------------------
    // Callbacks
    // -----------------------------------------------------------------------

    /**
     * @brief Callback chamado quando um dispositivo novo é encontrado.
     *
     * Disparado durante a descoberta ao receber PONG de um dispositivo
     * ainda não cadastrado. Chame `registrar(d)` dentro do callback
     * para persistir o dispositivo.
     *
     * @param cb  `void cb(const Dispositivo& dispositivo)`.
     */
    void aoEncontrarDispositivo(CallbackDispositivo cb) { _cbEncontrado = cb; }

    /**
     * @brief Callback chamado quando uma mensagem de payload é recebida.
     *
     * @param cb  `void cb(const Dispositivo& disp, const uint8_t* dados, uint8_t tam)`.
     */
    void aoReceberMensagem(CallbackMensagemMestre cb) { _cbMensagem = cb; }

    /**
     * @brief Callback chamado quando um dispositivo fica offline.
     *
     * @param cb  `void cb(const Dispositivo& dispositivo)`.
     */
    void aoFicarOffline(CallbackDispositivo cb) { _cbOffline = cb; }

    /**
     * @brief Callback chamado quando um dispositivo que estava offline volta.
     *
     * @param cb  `void cb(const Dispositivo& dispositivo)`.
     */
    void aoVoltarOnline(CallbackDispositivo cb) { _cbOnline = cb; }

    /**
     * @brief Callback chamado após cada tentativa de envio.
     *
     * @param cb  `void cb(const uint8_t* mac, bool sucesso)`.
     */
    void aoEnviar(CallbackEnvio cb) { _cbEnvio = cb; }

    // -----------------------------------------------------------------------
    // Utilitários
    // -----------------------------------------------------------------------

    /** @brief Retorna o MAC deste mestre como string `"AA:BB:CC:DD:EE:FF"`. */
    String mac() const;

    /** @brief Retorna o canal WiFi/ESP-NOW atual. */
    uint8_t canal() const { return WiFi.channel(); }

private:
    // ── Identidade ──────────────────────────────────────────────────────────
    char    _sistemaId[ESPNOW_MAX_SISTEMA_ID] = {};
    uint8_t _mac[6] = {};

    // ── Registro ────────────────────────────────────────────────────────────
    RegistroDispositivos _registro;

    // ── Descoberta ──────────────────────────────────────────────────────────
    bool     _descobertaAtiva    = false;
    uint32_t _inicioDescoberta   = 0;
    uint32_t _timeoutDescoberta  = ESPNOW_TIMEOUT_DESCOBERTA_MS;

    // ── Heartbeat ───────────────────────────────────────────────────────────
    uint32_t _intervaloHeartbeat = ESPNOW_INTERVALO_HEARTBEAT_MS;
    uint32_t _ultimoHeartbeat    = 0;

    // ── Buffer ISR → loop ───────────────────────────────────────────────────
    volatile bool _temDados  = false;
    uint8_t       _bufRx[250];
    volatile int  _tamRx     = 0;
    uint8_t       _macRx[6]  = {};

    // ── Callbacks ────────────────────────────────────────────────────────────
    CallbackDispositivo    _cbEncontrado = nullptr;
    CallbackMensagemMestre _cbMensagem   = nullptr;
    CallbackDispositivo    _cbOffline    = nullptr;
    CallbackDispositivo    _cbOnline     = nullptr;
    CallbackEnvio          _cbEnvio      = nullptr;

    // ── Singleton ESP-NOW ───────────────────────────────────────────────────
    static EspNowMestre* _instancia;
    static void _onRecv(const esp_now_recv_info_t* info,
                        const uint8_t* dados, int tam);
    static void _onSend(const uint8_t* mac, esp_now_send_status_t status);

    // ── Internos ────────────────────────────────────────────────────────────
    void _processar();
    void _processarPong(const uint8_t* mac, const uint8_t* buf, int tam);
    void _processarMensagem(const uint8_t* mac, const uint8_t* buf, int tam);
    void _processarPongHB(const uint8_t* mac);
    void _verificarOffline();
    void _verificarTimeoutDescoberta();
    bool _adicionarPeer(const uint8_t mac[6]);
    bool _enviar(const uint8_t mac[6], const uint8_t* dados, uint8_t tamanho);
};

// =============================================================================
// EspNowEscravo
// =============================================================================

/**
 * @brief Gerencia o ESP-NOW do lado do dispositivo escravo.
 *
 * O escravo não conecta ao WiFi — usa apenas ESP-NOW. Aguarda o PING
 * de descoberta do mestre, responde com um PONG e fica disponível para
 * receber mensagens e enviar respostas.
 *
 * @par Fluxo típico
 * ```
 * 1. begin()              — inicializa ESP-NOW
 * 2. aoReceberMensagem()  — registra callback de recepção
 * 3. atualizar()          — chama no loop()
 * 4. responder()          — envia payload de volta ao mestre
 * ```
 *
 * @par Exemplo completo
 * @code
 * EspNowEscravo escravo;
 *
 * void setup() {
 *     escravo.begin(1, "atuador-01");
 *
 *     escravo.aoReceberMensagem([](const uint8_t* dados, uint8_t tam) {
 *         // interpretar comando
 *         uint8_t resposta[] = {0x01, 0xFF};  // payload livre
 *         escravo.responder(resposta, sizeof(resposta));
 *     });
 * }
 *
 * void loop() { escravo.atualizar(); }
 * @endcode
 */
class EspNowEscravo {
public:
    // -----------------------------------------------------------------------
    // Inicialização
    // -----------------------------------------------------------------------

    /**
     * @brief Inicializa o ESP-NOW no modo escravo.
     *
     * Configura `WIFI_STA` sem conectar a nenhuma rede.
     *
     * @param tipoDispositivo  Valor numérico definido pelo projeto (0–255).
     *                         Enviado no PONG para que o mestre identifique
     *                         o tipo deste escravo.
     * @param label            Nome legível (ex: "sensor-01").
     *                         Máx: ESPNOW_MAX_LABEL-1 chars.
     * @param versaoFirmware   Versão do firmware (padrão: 1).
     * @return true            se o ESP-NOW foi inicializado com sucesso.
     *
     * @par Exemplo
     * @code
     * escravo.begin(42, "sensor-01");
     * escravo.begin(TIPO_ATUADOR, "atuador-02", 3);
     * @endcode
     */
    bool begin(uint8_t     tipoDispositivo,
               const char* label,
               uint8_t     versaoFirmware = 1);

    /**
     * @brief Processa mensagens recebidas e responde a heartbeats.
     *
     * **Deve ser chamado em todo `loop()`.**
     */
    void atualizar();

    // -----------------------------------------------------------------------
    // Comunicação
    // -----------------------------------------------------------------------

    /**
     * @brief Envia um payload de resposta ao mestre.
     *
     * Só funciona após o mestre ter sido identificado (depois do primeiro PING).
     *
     * @param dados    Ponteiro para o payload.
     * @param tamanho  Tamanho em bytes (máx: ESPNOW_MAX_PAYLOAD).
     * @return true    se o mestre está registrado e o envio foi aceito.
     *
     * @par Exemplo
     * @code
     * uint8_t leitura[] = {0x01, 0xAB, 0xCD};
     * escravo.responder(leitura, sizeof(leitura));
     * @endcode
     */
    bool responder(const void* dados, uint8_t tamanho);

    /**
     * @brief Verifica se o mestre já foi identificado.
     *
     * @return true  se o mestre respondeu a pelo menos um PING.
     */
    bool mestreConectado() const { return _mestreRegistrado; }

    /**
     * @brief Retorna o MAC do mestre como string, ou `""` se não identificado.
     */
    String macMestre() const;

    // -----------------------------------------------------------------------
    // Callbacks
    // -----------------------------------------------------------------------

    /**
     * @brief Callback chamado quando uma mensagem de payload é recebida.
     *
     * @param cb  `void cb(const uint8_t* dados, uint8_t tamanho)`.
     *
     * @note Chamado fora da ISR (no `atualizar()`): é seguro usar
     *       `Serial`, `Preferences`, `delay()`, etc.
     */
    void aoReceberMensagem(CallbackMensagemEscravo cb) { _cbMensagem = cb; }

    /**
     * @brief Callback chamado quando o mestre é identificado pela primeira vez.
     *
     * @param cb  `void cb(const uint8_t* mac, bool sucesso)`.
     */
    void aoConectarMestre(CallbackEnvio cb) { _cbMestre = cb; }

    /**
     * @brief Callback chamado após cada tentativa de envio.
     *
     * @param cb  `void cb(const uint8_t* mac, bool sucesso)`.
     */
    void aoEnviar(CallbackEnvio cb) { _cbEnvio = cb; }

    // -----------------------------------------------------------------------
    // Utilitários
    // -----------------------------------------------------------------------

    /** @brief Retorna o MAC deste escravo como string. */
    String mac() const;

    /** @brief Retorna o ID lógico atribuído pelo mestre (0 = não cadastrado). */
    uint8_t id() const { return _id; }

    /** @brief Retorna o label configurado. */
    const char* label() const { return _label; }

    /** @brief Retorna o tipo configurado. */
    uint8_t tipoDispositivo() const { return _tipo; }

private:
    uint8_t _tipo           = 0;
    char    _label[ESPNOW_MAX_LABEL] = {};
    uint8_t _versaoFirmware = 1;
    uint8_t _id             = 0;
    uint8_t _mac[6]         = {};

    uint8_t _macMestre[6]       = {};
    bool    _mestreRegistrado   = false;

    // Buffer ISR → loop
    volatile bool _temDados = false;
    uint8_t       _bufRx[250];
    volatile int  _tamRx    = 0;
    uint8_t       _macRx[6] = {};

    // Callbacks
    CallbackMensagemEscravo _cbMensagem = nullptr;
    CallbackEnvio           _cbMestre   = nullptr;
    CallbackEnvio           _cbEnvio    = nullptr;

    // Singleton
    static EspNowEscravo* _instancia;
    static void _onRecv(const esp_now_recv_info_t* info,
                        const uint8_t* dados, int tam);
    static void _onSend(const uint8_t* mac, esp_now_send_status_t status);

    // Internos
    void _processar();
    void _responderPing(const uint8_t* macMestre, uint8_t idRecebido);
    void _responderHeartbeat();
    void _registrarMestre(const uint8_t* mac);
    bool _adicionarPeer(const uint8_t mac[6]);
    bool _enviar(const uint8_t mac[6], const uint8_t* dados, uint8_t tamanho);
};

#endif // ESP32_ESPNOW_H
