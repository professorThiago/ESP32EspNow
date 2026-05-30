/**
 * @file ESP32EspNow.cpp
 * @brief Implementação da biblioteca ESP32EspNow.
 *
 * @author  professorThiago (https://github.com/professorThiago)
 * @version 2.1.0
 * @license MIT
 */

#include "ESP32EspNow.h"

// =============================================================================
// Protocolo interno — cabeçalho de 10 bytes
// Invisível ao projeto — usado apenas entre instâncias desta biblioteca.
//
// Layout:
//   [ versao(1) | tipo(1) | addr(1) | id(1) | ts(4) | tamPayload(1) | rsv(1) ]
//
// Campo addr:
//   ESPNOW_ADDR_NENHUM (0)    → endereçamento desativado, aceito por todos
//   ESPNOW_ADDR_BROADCAST(63) → PING de descoberta, aceito por todos com addr>0
//   1–62                      → só aceito por dispositivos do mesmo addr
// =============================================================================

enum class _TipoInterno : uint8_t {
    PING     = 0xA1,  // mestre → broadcast
    PONG     = 0xA2,  // escravo → mestre
    MENSAGEM = 0xA3,  // payload livre
    PING_HB  = 0xA4,  // heartbeat
    PONG_HB  = 0xA5,  // resposta heartbeat
};

struct __attribute__((packed)) _Cabecalho {
    uint8_t      versao      = ESPNOW_VERSAO_PROTOCOLO;
    _TipoInterno tipo;
    uint8_t      addr        = ESPNOW_ADDR_NENHUM; ///< endereço 6 bits do remetente
    uint8_t      id          = 0;
    uint32_t     ts          = 0;
    uint8_t      tamPayload  = 0;
    uint8_t      _rsv        = 0;  // reservado — alinhamento e expansão futura
};

// Layout do PONG — payload fixo após o cabeçalho
struct __attribute__((packed)) _PayloadPong {
    uint8_t tipoDispositivo;
    uint8_t versaoFirmware;
    uint8_t enderecoSala;           ///< addr do escravo (0 = sem endereçamento)
    char    label[ESPNOW_MAX_LABEL];
};

static constexpr uint8_t _TAM_CAB = sizeof(_Cabecalho);

// =============================================================================
// Singletons
// =============================================================================

EspNowMestre* EspNowMestre::_instancia = nullptr;
EspNowEscravo* EspNowEscravo::_instancia = nullptr;

// =============================================================================
// Helpers internos de arquivo
// =============================================================================

static bool _macsIguais(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

static void _preencherCab(_Cabecalho& cab, _TipoInterno tipo,
                           uint8_t addr = ESPNOW_ADDR_NENHUM,
                           uint8_t id = 0, uint8_t tamPayload = 0)
{
    cab.versao      = ESPNOW_VERSAO_PROTOCOLO;
    cab.tipo        = tipo;
    cab.addr        = addr;
    cab.id          = id;
    cab.ts          = millis();
    cab.tamPayload  = tamPayload;
    cab._rsv        = 0;
}

/**
 * @brief Lê os 6 pinos de jumper e retorna o endereço de 6 bits.
 *
 * Jumper fechado (pino a GND) = bit 1, aberto (pull-up) = bit 0.
 * Retorna ESPNOW_ADDR_NENHUM (0) se todos abertos (inválido).
 */
static uint8_t _lerJumpers(const uint8_t pinos[ESPNOW_BITS_ENDERECO])
{
    uint8_t addr = 0;
    for (uint8_t i = 0; i < ESPNOW_BITS_ENDERECO; i++) {
        pinMode(pinos[i], INPUT_PULLUP);
    }
    // Pequeno delay para estabilizar os pull-ups antes da leitura
    delay(5);
    for (uint8_t i = 0; i < ESPNOW_BITS_ENDERECO; i++) {
        // Jumper fechado = LOW = bit ativo
        if (digitalRead(pinos[i]) == LOW) {
            addr |= (1 << i);
        }
    }
    return addr;
}

static bool _adicionarPeerESP(const uint8_t mac[6])
{
    if (esp_now_is_peer_exist(mac)) return true;
    esp_now_peer_info_t p{};
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;
    p.encrypt = false;
    return esp_now_add_peer(&p) == ESP_OK;
}

/**
 * @brief Verifica se um endereço recebido deve ser aceito por este dispositivo.
 *
 * Regra:
 *  - Se o filtro local está desativado (_addrAtivo=false): aceita tudo.
 *  - PING/BROADCAST (addr=ESPNOW_ADDR_BROADCAST): aceita se addr local > 0.
 *  - Endereço sem filtro (addr=0): aceita sempre (compatibilidade).
 *  - Caso contrário: só aceita se addr == _addr local.
 */
static bool _deveAceitar(bool addrAtivo, uint8_t addrLocal,
                          uint8_t addrPacote, _TipoInterno tipo)
{
    // Filtro desativado — aceita tudo (modo transparente)
    if (!addrAtivo) return true;

    // PING de descoberta com addr de broadcast — aceita se temos addr válido
    if (tipo == _TipoInterno::PING && addrPacote == ESPNOW_ADDR_BROADCAST)
        return addrLocal >= ESPNOW_ADDR_MIN;

    // Pacote sem endereçamento — aceita (compatibilidade com dispositivos sem jumper)
    if (addrPacote == ESPNOW_ADDR_NENHUM) return true;

    // Filtra pelo endereço
    return addrPacote == addrLocal;
}

// =============================================================================
// RegistroDispositivos
// =============================================================================

void RegistroDispositivos::begin(const char* nsNVS)
{
    strncpy(_ns, nsNVS, sizeof(_ns) - 1);
    _total  = 0;
    _proxId = 1;
    memset(_dispositivos, 0, sizeof(_dispositivos));
    carregar();
}

bool RegistroDispositivos::adicionar(const Dispositivo& d)
{
    // Atualiza se MAC já existe
    Dispositivo* ex = porMac(d.mac);
    if (ex) {
        uint8_t idSalvo = ex->id;
        *ex = d;
        if (ex->id == 0) ex->id = idSalvo ? idSalvo : _gerarId();
        _salvar(ex - _dispositivos);
        return true;
    }

    if (_total >= ESPNOW_MAX_DISPOSITIVOS) return false;

    _dispositivos[_total]    = d;
    _dispositivos[_total].id = _gerarId();
    _salvar(_total);
    _total++;
    return true;
}

bool RegistroDispositivos::remover(uint8_t id)
{
    for (uint8_t i = 0; i < _total; i++) {
        if (_dispositivos[i].id != id) continue;

        char chave[12]; snprintf(chave, sizeof(chave), "d%02d", i);
        _prefs.begin(_ns, false);
        _prefs.remove(chave);
        _prefs.end();

        for (uint8_t j = i; j < _total - 1; j++)
            _dispositivos[j] = _dispositivos[j + 1];
        memset(&_dispositivos[_total - 1], 0, sizeof(Dispositivo));
        _total--;
        salvar();
        return true;
    }
    return false;
}

void RegistroDispositivos::limpar()
{
    _prefs.begin(_ns, false);
    _prefs.clear();
    _prefs.end();
    memset(_dispositivos, 0, sizeof(_dispositivos));
    _total  = 0;
    _proxId = 1;
}

Dispositivo* RegistroDispositivos::porMac(const uint8_t mac[6])
{
    for (uint8_t i = 0; i < _total; i++)
        if (_macsIguais(_dispositivos[i].mac, mac)) return &_dispositivos[i];
    return nullptr;
}

Dispositivo* RegistroDispositivos::porId(uint8_t id)
{
    for (uint8_t i = 0; i < _total; i++)
        if (_dispositivos[i].id == id) return &_dispositivos[i];
    return nullptr;
}

Dispositivo* RegistroDispositivos::porLabel(const char* label)
{
    for (uint8_t i = 0; i < _total; i++)
        if (strncmp(_dispositivos[i].label, label, ESPNOW_MAX_LABEL) == 0)
            return &_dispositivos[i];
    return nullptr;
}

uint8_t RegistroDispositivos::porTipo(uint8_t tipo,
                                        Dispositivo** saida, uint8_t maxSaida)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < _total && n < maxSaida; i++)
        if (_dispositivos[i].tipoDispositivo == tipo) saida[n++] = &_dispositivos[i];
    return n;
}

Dispositivo& RegistroDispositivos::porIndice(uint8_t i)
{
    if (i >= _total) i = 0;
    return _dispositivos[i];
}

void RegistroDispositivos::salvar()
{
    _prefs.begin(_ns, false);
    _prefs.putUChar("total",  _total);
    _prefs.putUChar("proxId", _proxId);
    _prefs.end();
    for (uint8_t i = 0; i < _total; i++) _salvar(i);
}

void RegistroDispositivos::carregar()
{
    _prefs.begin(_ns, true);
    _total  = _prefs.getUChar("total",  0);
    _proxId = _prefs.getUChar("proxId", 1);
    _prefs.end();
    if (_total > ESPNOW_MAX_DISPOSITIVOS) _total = ESPNOW_MAX_DISPOSITIVOS;
    for (uint8_t i = 0; i < _total; i++) _carregar(i);
}

void RegistroDispositivos::_salvar(uint8_t i)
{
    char chave[12]; snprintf(chave, sizeof(chave), "d%02d", i);
    _prefs.begin(_ns, false);
    _prefs.putBytes(chave, &_dispositivos[i], sizeof(Dispositivo));
    _prefs.putUChar("total",  _total);
    _prefs.putUChar("proxId", _proxId);
    _prefs.end();
}

void RegistroDispositivos::_carregar(uint8_t i)
{
    char chave[12]; snprintf(chave, sizeof(chave), "d%02d", i);
    _prefs.begin(_ns, true);
    size_t lido = _prefs.getBytes(chave, &_dispositivos[i], sizeof(Dispositivo));
    _prefs.end();
    if (lido != sizeof(Dispositivo)) memset(&_dispositivos[i], 0, sizeof(Dispositivo));
}

uint8_t RegistroDispositivos::_gerarId()
{
    uint8_t id = _proxId++;
    if (_proxId > ESPNOW_MAX_DISPOSITIVOS) _proxId = 1;
    return id;
}

String RegistroDispositivos::macParaString(const uint8_t mac[6])
{
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

bool RegistroDispositivos::stringParaMac(const char* str, uint8_t mac[6])
{
    return sscanf(str, "%hhX:%hhX:%hhX:%hhX:%hhX:%hhX",
                  &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6;
}

// =============================================================================
// EspNowMestre — inicialização
// =============================================================================

bool EspNowMestre::begin(const char* sistemaId, const char* nsNVS)
{
    _instancia = this;
    strncpy(_sistemaId, sistemaId, ESPNOW_MAX_SISTEMA_ID - 1);

    WiFi.mode(WIFI_STA);
    WiFi.macAddress(_mac);

    if (esp_now_init() != ESP_OK) return false;

    esp_now_register_recv_cb(_onRecv);
    esp_now_register_send_cb(_onSend);

    _registro.begin(nsNVS);

    for (uint8_t i = 0; i < _registro.total(); i++)
        _adicionarPeer(_registro.porIndice(i).mac);

    return true;
}

bool EspNowMestre::configurarEnderecoFisico(const uint8_t pinos[ESPNOW_BITS_ENDERECO])
{
    uint8_t addr = _lerJumpers(pinos);
    return configurarEnderecoPorSoftware(addr);
}

bool EspNowMestre::configurarEnderecoPorSoftware(uint8_t addr)
{
    if (addr == ESPNOW_ADDR_NENHUM) {
        Serial.println("[EspNowMestre] ERRO: endereco 0 invalido. "
                       "Configure os jumpers antes de ligar o dispositivo.");
        return false;
    }
    if (addr > ESPNOW_ADDR_MAX) {
        Serial.println("[EspNowMestre] ERRO: endereco " + String(addr) +
                       " reservado (max=" + String(ESPNOW_ADDR_MAX) + ").");
        return false;
    }
    _addr      = addr;
    _addrAtivo = true;
    Serial.println("[EspNowMestre] Endereco fisico: " + String(_addr) +
                   " (0b" + String(_addr, BIN) + ")");
    return true;
}

// =============================================================================
// EspNowMestre — loop
// =============================================================================

void EspNowMestre::atualizar()
{
    _processar();
    _verificarTimeoutDescoberta();

    if (_intervaloHeartbeat > 0 &&
        millis() - _ultimoHeartbeat >= _intervaloHeartbeat) {
        _ultimoHeartbeat = millis();
        enviarHeartbeat();
        _verificarOffline();
    }
}

// =============================================================================
// EspNowMestre — descoberta
// =============================================================================

void EspNowMestre::iniciarDescoberta(uint32_t timeoutMs)
{
    _descobertaAtiva   = true;
    _inicioDescoberta  = millis();
    _timeoutDescoberta = timeoutMs;

    static const uint8_t broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    _adicionarPeer(broadcast);

    uint8_t buf[_TAM_CAB];
    _Cabecalho cab;
    // PING usa addr=BROADCAST para que escravos com qualquer addr respondam
    // mas só os do mesmo addr do mestre serão aceitos pelo filtro deles
    uint8_t addrPing = _addrAtivo ? ESPNOW_ADDR_BROADCAST : ESPNOW_ADDR_NENHUM;
    _preencherCab(cab, _TipoInterno::PING, addrPing);
    memcpy(buf, &cab, _TAM_CAB);
    _enviar(broadcast, buf, _TAM_CAB);
}

bool EspNowMestre::descobertaAtiva() const { return _descobertaAtiva; }

void EspNowMestre::_verificarTimeoutDescoberta()
{
    if (_descobertaAtiva &&
        millis() - _inicioDescoberta >= _timeoutDescoberta) {
        _descobertaAtiva = false;
    }
}

// =============================================================================
// EspNowMestre — gerenciamento
// =============================================================================

bool EspNowMestre::registrar(const Dispositivo& d)
{
    if (!_registro.adicionar(d)) return false;
    _adicionarPeer(d.mac);
    return true;
}

bool EspNowMestre::remover(uint8_t id)
{
    Dispositivo* d = _registro.porId(id);
    if (!d) return false;
    esp_now_del_peer(d->mac);
    return _registro.remover(id);
}

void EspNowMestre::limpar()
{
    for (uint8_t i = 0; i < _registro.total(); i++)
        esp_now_del_peer(_registro.porIndice(i).mac);
    _registro.limpar();
}

// =============================================================================
// EspNowMestre — envio
// =============================================================================

bool EspNowMestre::enviarPorMac(const uint8_t mac[6],
                                  const void* dados, uint8_t tamanho)
{
    if (tamanho > ESPNOW_MAX_PAYLOAD) tamanho = ESPNOW_MAX_PAYLOAD;

    uint8_t buf[_TAM_CAB + ESPNOW_MAX_PAYLOAD];
    _Cabecalho cab;
    _preencherCab(cab, _TipoInterno::MENSAGEM, _addr, 0, tamanho);
    memcpy(buf, &cab, _TAM_CAB);
    memcpy(buf + _TAM_CAB, dados, tamanho);
    return _enviar(mac, buf, _TAM_CAB + tamanho);
}

bool EspNowMestre::enviarPorId(uint8_t id, const void* dados, uint8_t tamanho)
{
    Dispositivo* d = _registro.porId(id);
    if (!d) return false;
    return enviarPorMac(d->mac, dados, tamanho);
}

bool EspNowMestre::enviarPorLabel(const char* label,
                                    const void* dados, uint8_t tamanho)
{
    Dispositivo* d = _registro.porLabel(label);
    if (!d) return false;
    return enviarPorMac(d->mac, dados, tamanho);
}

uint8_t EspNowMestre::enviarParaTipo(uint8_t tipo,
                                      const void* dados, uint8_t tamanho)
{
    Dispositivo* lista[ESPNOW_MAX_DISPOSITIVOS];
    uint8_t n = _registro.porTipo(tipo, lista, ESPNOW_MAX_DISPOSITIVOS);
    uint8_t ok = 0;
    for (uint8_t i = 0; i < n; i++)
        if (enviarPorMac(lista[i]->mac, dados, tamanho)) ok++;
    return ok;
}

uint8_t EspNowMestre::enviarParaTodos(const void* dados, uint8_t tamanho)
{
    uint8_t ok = 0;
    for (uint8_t i = 0; i < _registro.total(); i++)
        if (enviarPorMac(_registro.porIndice(i).mac, dados, tamanho)) ok++;
    return ok;
}

// =============================================================================
// EspNowMestre — heartbeat
// =============================================================================

void EspNowMestre::configurarHeartbeat(uint32_t intervaloMs)
{
    _intervaloHeartbeat = intervaloMs;
}

void EspNowMestre::enviarHeartbeat()
{
    uint8_t buf[_TAM_CAB];
    _Cabecalho cab;
    _preencherCab(cab, _TipoInterno::PING_HB, _addr);
    memcpy(buf, &cab, _TAM_CAB);

    for (uint8_t i = 0; i < _registro.total(); i++)
        _enviar(_registro.porIndice(i).mac, buf, _TAM_CAB);
}

void EspNowMestre::_verificarOffline()
{
    if (_intervaloHeartbeat == 0) return;
    uint32_t limite = _intervaloHeartbeat * 3;
    for (uint8_t i = 0; i < _registro.total(); i++) {
        Dispositivo& d = _registro.porIndice(i);
        bool eraOnline = d.online;
        if (millis() - d.ultimoContato > limite) {
            if (d.online) {
                d.online = false;
                if (_cbOffline) _cbOffline(d);
            }
        } else if (!eraOnline) {
            d.online = true;
            if (_cbOnline) _cbOnline(d);
        }
    }
}

// =============================================================================
// EspNowMestre — utilitários
// =============================================================================

String EspNowMestre::mac() const
{
    return RegistroDispositivos::macParaString(_mac);
}

// =============================================================================
// EspNowMestre — processamento do buffer
// =============================================================================

void EspNowMestre::_processar()
{
    if (!_temDados) return;

    uint8_t buf[250];
    int     tam;
    uint8_t macOrigem[6];

    noInterrupts();
    tam = _tamRx;
    memcpy(buf, _bufRx, tam);
    memcpy(macOrigem, _macRx, 6);
    _temDados = false;
    interrupts();

    if (tam < _TAM_CAB) return;

    _Cabecalho cab;
    memcpy(&cab, buf, _TAM_CAB);
    if (cab.versao != ESPNOW_VERSAO_PROTOCOLO) return;

    // ── Filtro de endereço ────────────────────────────────────────────────
    if (!_deveAceitar(_addrAtivo, _addr, cab.addr, cab.tipo)) return;

    // Atualiza contato
    Dispositivo* d = _registro.porMac(macOrigem);
    if (d) { d->online = true; d->ultimoContato = millis(); }

    switch (cab.tipo) {
        case _TipoInterno::PONG:    _processarPong(macOrigem, buf, tam);    break;
        case _TipoInterno::MENSAGEM:_processarMensagem(macOrigem, buf, tam);break;
        case _TipoInterno::PONG_HB: _processarPongHB(macOrigem);            break;
        default: break;
    }
}

void EspNowMestre::_processarPong(const uint8_t* mac, const uint8_t* buf, int tam)
{
    if (tam < _TAM_CAB + (int)sizeof(_PayloadPong)) return;

    _PayloadPong pp;
    memcpy(&pp, buf + _TAM_CAB, sizeof(pp));

    _Cabecalho cab;
    memcpy(&cab, buf, _TAM_CAB);

    if (_registro.porMac(mac) != nullptr) return;

    Dispositivo d{};
    memcpy(d.mac, mac, 6);
    d.tipoDispositivo = pp.tipoDispositivo;
    d.id              = cab.id;
    d.versaoFirmware  = pp.versaoFirmware;
    d.enderecoSala    = pp.enderecoSala;   // addr físico do escravo
    d.online          = true;
    d.ultimoContato   = millis();
    strncpy(d.label, pp.label, ESPNOW_MAX_LABEL - 1);

    if (_cbEncontrado) _cbEncontrado(d);
}

void EspNowMestre::_processarMensagem(const uint8_t* mac,
                                       const uint8_t* buf, int tam)
{
    if (!_cbMensagem) return;

    _Cabecalho cab;
    memcpy(&cab, buf, _TAM_CAB);

    const uint8_t* payload    = buf + _TAM_CAB;
    uint8_t        tamPayload = cab.tamPayload;

    // Garante que não lemos além do buffer recebido
    if (_TAM_CAB + tamPayload > (uint8_t)tam) tamPayload = tam - _TAM_CAB;

    Dispositivo* d = _registro.porMac(mac);
    if (d) {
        _cbMensagem(*d, payload, tamPayload);
    }
}

void EspNowMestre::_processarPongHB(const uint8_t* mac)
{
    Dispositivo* d = _registro.porMac(mac);
    if (d) {
        bool eraOffline = !d->online;
        d->online        = true;
        d->ultimoContato = millis();
        if (eraOffline && _cbOnline) _cbOnline(*d);
    }
}

// =============================================================================
// EspNowMestre — internos de rede
// =============================================================================

bool EspNowMestre::_adicionarPeer(const uint8_t mac[6])
{
    return _adicionarPeerESP(mac);
}

bool EspNowMestre::_enviar(const uint8_t mac[6],
                             const uint8_t* dados, uint8_t tamanho)
{
    return esp_now_send(mac, dados, tamanho) == ESP_OK;
}

// =============================================================================
// EspNowMestre — callbacks estáticos
// =============================================================================

void EspNowMestre::_onRecv(const esp_now_recv_info_t* info,
                             const uint8_t* dados, int tam)
{
    if (!_instancia || _instancia->_temDados) return;
    int copia = (tam > 250) ? 250 : tam;
    memcpy((void*)_instancia->_bufRx, dados, copia);
    memcpy(_instancia->_macRx, info->src_addr, 6);
    _instancia->_tamRx    = copia;
    _instancia->_temDados = true;
}

void EspNowMestre::_onSend(const uint8_t* mac, esp_now_send_status_t status)
{
    if (!_instancia) return;
    bool ok = (status == ESP_NOW_SEND_SUCCESS);
    if (_instancia->_cbEnvio) _instancia->_cbEnvio(mac, ok);
}

// =============================================================================
// EspNowEscravo — inicialização
// =============================================================================

bool EspNowEscravo::begin(uint8_t tipoDispositivo, const char* label,
                           uint8_t versaoFirmware)
{
    _instancia = this;
    _tipo      = tipoDispositivo;
    strncpy(_label, label, ESPNOW_MAX_LABEL - 1);
    _versaoFirmware = versaoFirmware;

    WiFi.mode(WIFI_STA);
    WiFi.macAddress(_mac);

    if (esp_now_init() != ESP_OK) return false;

    esp_now_register_recv_cb(_onRecv);
    esp_now_register_send_cb(_onSend);

    return true;
}

bool EspNowEscravo::configurarEnderecoFisico(const uint8_t pinos[ESPNOW_BITS_ENDERECO])
{
    uint8_t addr = _lerJumpers(pinos);
    return configurarEnderecoPorSoftware(addr);
}

bool EspNowEscravo::configurarEnderecoPorSoftware(uint8_t addr)
{
    if (addr == ESPNOW_ADDR_NENHUM) {
        Serial.println("[EspNowEscravo] ERRO: endereco 0 invalido. "
                       "Configure os jumpers antes de ligar o dispositivo.");
        return false;
    }
    if (addr > ESPNOW_ADDR_MAX) {
        Serial.println("[EspNowEscravo] ERRO: endereco " + String(addr) +
                       " reservado (max=" + String(ESPNOW_ADDR_MAX) + ").");
        return false;
    }
    _addr      = addr;
    _addrAtivo = true;
    Serial.println("[EspNowEscravo] Endereco fisico: " + String(_addr) +
                   " (0b" + String(_addr, BIN) + ")");
    return true;
}

// =============================================================================
// EspNowEscravo — loop
// =============================================================================

void EspNowEscravo::atualizar()
{
    _processar();
}

// =============================================================================
// EspNowEscravo — comunicação
// =============================================================================

bool EspNowEscravo::responder(const void* dados, uint8_t tamanho)
{
    if (!_mestreRegistrado) return false;
    if (tamanho > ESPNOW_MAX_PAYLOAD) tamanho = ESPNOW_MAX_PAYLOAD;

    uint8_t buf[_TAM_CAB + ESPNOW_MAX_PAYLOAD];
    _Cabecalho cab;
    _preencherCab(cab, _TipoInterno::MENSAGEM, _addr, _id, tamanho);
    memcpy(buf, &cab, _TAM_CAB);
    memcpy(buf + _TAM_CAB, dados, tamanho);
    return _enviar(_macMestre, buf, _TAM_CAB + tamanho);
}

String EspNowEscravo::macMestre() const
{
    if (!_mestreRegistrado) return "";
    return RegistroDispositivos::macParaString(_macMestre);
}

String EspNowEscravo::mac() const
{
    return RegistroDispositivos::macParaString(_mac);
}

// =============================================================================
// EspNowEscravo — processamento do buffer
// =============================================================================

void EspNowEscravo::_processar()
{
    if (!_temDados) return;

    uint8_t buf[250];
    int     tam;
    uint8_t macOrigem[6];

    noInterrupts();
    tam = _tamRx;
    memcpy(buf, _bufRx, tam);
    memcpy(macOrigem, _macRx, 6);
    _temDados = false;
    interrupts();

    if (tam < _TAM_CAB) return;

    _Cabecalho cab;
    memcpy(&cab, buf, _TAM_CAB);
    if (cab.versao != ESPNOW_VERSAO_PROTOCOLO) return;

    // ── Filtro de endereço ────────────────────────────────────────────────
    if (!_deveAceitar(_addrAtivo, _addr, cab.addr, cab.tipo)) return;

    switch (cab.tipo) {
        case _TipoInterno::PING:
            _registrarMestre(macOrigem);
            _responderPing(macOrigem, cab.id);
            break;
        case _TipoInterno::MENSAGEM:
            if (_cbMensagem) {
                const uint8_t* payload    = buf + _TAM_CAB;
                uint8_t        tamPayload = cab.tamPayload;
                if (_TAM_CAB + tamPayload > (uint8_t)tam)
                    tamPayload = tam - _TAM_CAB;
                _cbMensagem(payload, tamPayload);
            }
            break;
        case _TipoInterno::PING_HB:
            _responderHeartbeat();
            break;
        default:
            break;
    }
}

void EspNowEscravo::_responderPing(const uint8_t* macMestre, uint8_t idRecebido)
{
    _PayloadPong pp{};
    pp.tipoDispositivo = _tipo;
    pp.versaoFirmware  = _versaoFirmware;
    pp.enderecoSala    = _addr;   // envia o addr físico para o mestre
    strncpy(pp.label, _label, ESPNOW_MAX_LABEL - 1);

    uint8_t buf[_TAM_CAB + sizeof(_PayloadPong)];
    _Cabecalho cab;
    // PONG inclui o addr do escravo para que o mestre possa exibi-lo
    _preencherCab(cab, _TipoInterno::PONG, _addr, _id, sizeof(_PayloadPong));
    memcpy(buf, &cab, _TAM_CAB);
    memcpy(buf + _TAM_CAB, &pp, sizeof(pp));

    _enviar(macMestre, buf, sizeof(buf));
}

void EspNowEscravo::_responderHeartbeat()
{
    if (!_mestreRegistrado) return;
    uint8_t buf[_TAM_CAB];
    _Cabecalho cab;
    _preencherCab(cab, _TipoInterno::PONG_HB, _addr, _id);
    memcpy(buf, &cab, _TAM_CAB);
    _enviar(_macMestre, buf, _TAM_CAB);
}

void EspNowEscravo::_registrarMestre(const uint8_t* mac)
{
    if (_mestreRegistrado && _macsIguais(_macMestre, mac)) return;

    memcpy(_macMestre, mac, 6);
    _mestreRegistrado = true;
    _adicionarPeer(mac);

    if (_cbMestre) _cbMestre(mac, true);
}

// =============================================================================
// EspNowEscravo — internos de rede
// =============================================================================

bool EspNowEscravo::_adicionarPeer(const uint8_t mac[6])
{
    return _adicionarPeerESP(mac);
}

bool EspNowEscravo::_enviar(const uint8_t mac[6],
                              const uint8_t* dados, uint8_t tamanho)
{
    return esp_now_send(mac, dados, tamanho) == ESP_OK;
}

// =============================================================================
// EspNowEscravo — callbacks estáticos
// =============================================================================

void EspNowEscravo::_onRecv(const esp_now_recv_info_t* info,
                              const uint8_t* dados, int tam)
{
    if (!_instancia || _instancia->_temDados) return;
    int copia = (tam > 250) ? 250 : tam;
    memcpy((void*)_instancia->_bufRx, dados, copia);
    memcpy(_instancia->_macRx, info->src_addr, 6);
    _instancia->_tamRx    = copia;
    _instancia->_temDados = true;
}

void EspNowEscravo::_onSend(const uint8_t* mac, esp_now_send_status_t status)
{
    if (!_instancia) return;
    bool ok = (status == ESP_NOW_SEND_SUCCESS);
    if (_instancia->_cbEnvio) _instancia->_cbEnvio(mac, ok);
}
