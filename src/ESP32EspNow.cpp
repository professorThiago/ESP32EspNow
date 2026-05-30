/**
 * @file ESP32EspNow.cpp
 * @brief Implementação da biblioteca ESP32EspNow.
 *
 * @author  professorThiago (https://github.com/professorThiago)
 * @version 1.0.0
 * @license MIT
 */

#include "ESP32EspNow.h"

// =============================================================================
// Singletons estáticos
// =============================================================================

EspNowCentral* EspNowCentral::_instancia = nullptr;
EspNowModulo*  EspNowModulo::_instancia  = nullptr;

// =============================================================================
// Helpers internos (arquivo)
// =============================================================================

static bool _macsIguais(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

static bool _macBroadcast(const uint8_t mac[6])
{
    for (uint8_t i = 0; i < 6; i++) if (mac[i] != 0xFF) return false;
    return true;
}

static void _preencherCabecalho(CabecalhoMsg& cab, TipoMsg tipo, uint8_t id = 0)
{
    cab.versao    = ESPNOW_VERSAO_PROTOCOLO;
    cab.tipo      = tipo;
    cab.moduloId  = id;
    cab.timestamp = millis();
}

// =============================================================================
// RegistroModulos
// =============================================================================

void RegistroModulos::begin(const char* nsNVS)
{
    strncpy(_ns, nsNVS, sizeof(_ns) - 1);
    _total  = 0;
    _proxId = 1;
    memset(_modulos, 0, sizeof(_modulos));
    carregar();
    debugInfo("[Registro] " + String(_total) + " modulo(s) carregado(s) do NVS.");
}

bool RegistroModulos::adicionar(const InfoModulo& modulo)
{
    // Atualiza se MAC já existe
    InfoModulo* existente = buscarPorMac(modulo.mac);
    if (existente) {
        *existente = modulo;
        if (existente->id == 0) existente->id = _gerarId();
        _salvarModulo(existente - _modulos);
        debugInfo("[Registro] Modulo atualizado: " + String(existente->label));
        return true;
    }

    if (_total >= ESPNOW_MAX_MODULOS) {
        debugErro("[Registro] Registro cheio (" +
                  String(ESPNOW_MAX_MODULOS) + " modulos).");
        return false;
    }

    _modulos[_total]    = modulo;
    _modulos[_total].id = _gerarId();
    _salvarModulo(_total);
    _total++;

    debugInfo("[Registro] Modulo adicionado: " + String(modulo.label) +
              " | ID: " + String(_modulos[_total - 1].id) +
              " | MAC: " + macParaString(modulo.mac));
    return true;
}

bool RegistroModulos::remover(uint8_t id)
{
    for (uint8_t i = 0; i < _total; i++) {
        if (_modulos[i].id == id) {
            debugInfo("[Registro] Removendo modulo ID " + String(id) +
                      " (" + String(_modulos[i].label) + ")");

            // Apaga do NVS
            char chave[12];
            snprintf(chave, sizeof(chave), "m%02d", i);
            _prefs.begin(_ns, false);
            _prefs.remove(chave);
            _prefs.end();

            // Compacta array
            for (uint8_t j = i; j < _total - 1; j++) {
                _modulos[j] = _modulos[j + 1];
            }
            memset(&_modulos[_total - 1], 0, sizeof(InfoModulo));
            _total--;
            salvar(); // salva estado completo
            return true;
        }
    }
    debugAviso("[Registro] ID " + String(id) + " nao encontrado para remocao.");
    return false;
}

void RegistroModulos::limpar()
{
    _prefs.begin(_ns, false);
    _prefs.clear();
    _prefs.end();
    memset(_modulos, 0, sizeof(_modulos));
    _total  = 0;
    _proxId = 1;
    debugInfo("[Registro] Registro limpo.");
}

InfoModulo* RegistroModulos::buscarPorMac(const uint8_t mac[6])
{
    for (uint8_t i = 0; i < _total; i++) {
        if (_macsIguais(_modulos[i].mac, mac)) return &_modulos[i];
    }
    return nullptr;
}

InfoModulo* RegistroModulos::buscarPorId(uint8_t id)
{
    for (uint8_t i = 0; i < _total; i++) {
        if (_modulos[i].id == id) return &_modulos[i];
    }
    return nullptr;
}

uint8_t RegistroModulos::buscarPorTipo(TipoModulo tipo,
                                        InfoModulo** saida,
                                        uint8_t maxSaida)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < _total && n < maxSaida; i++) {
        if (_modulos[i].tipo == tipo) saida[n++] = &_modulos[i];
    }
    return n;
}

InfoModulo& RegistroModulos::obterPorIndice(uint8_t indice)
{
    // Proteção contra acesso fora dos limites
    if (indice >= _total) indice = 0;
    return _modulos[indice];
}

void RegistroModulos::salvar()
{
    _prefs.begin(_ns, false);
    _prefs.putUChar("total",  _total);
    _prefs.putUChar("proxId", _proxId);
    _prefs.end();

    for (uint8_t i = 0; i < _total; i++) _salvarModulo(i);

    debugVerbose("[Registro] " + String(_total) + " modulo(s) salvos no NVS.");
}

void RegistroModulos::carregar()
{
    _prefs.begin(_ns, true);
    _total  = _prefs.getUChar("total",  0);
    _proxId = _prefs.getUChar("proxId", 1);
    _prefs.end();

    if (_total > ESPNOW_MAX_MODULOS) _total = ESPNOW_MAX_MODULOS;

    for (uint8_t i = 0; i < _total; i++) _carregarModulo(i);
}

void RegistroModulos::_salvarModulo(uint8_t indice)
{
    char chave[12];
    snprintf(chave, sizeof(chave), "m%02d", indice);

    _prefs.begin(_ns, false);
    _prefs.putBytes(chave, &_modulos[indice], sizeof(InfoModulo));
    _prefs.putUChar("total",  _total);
    _prefs.putUChar("proxId", _proxId);
    _prefs.end();
}

void RegistroModulos::_carregarModulo(uint8_t indice)
{
    char chave[12];
    snprintf(chave, sizeof(chave), "m%02d", indice);

    _prefs.begin(_ns, true);
    size_t lido = _prefs.getBytes(chave, &_modulos[indice], sizeof(InfoModulo));
    _prefs.end();

    if (lido != sizeof(InfoModulo)) {
        memset(&_modulos[indice], 0, sizeof(InfoModulo));
        debugAviso("[Registro] Falha ao carregar modulo " + String(indice));
    }
}

uint8_t RegistroModulos::_gerarId()
{
    uint8_t id = _proxId++;
    if (_proxId > ESPNOW_MAX_MODULOS) _proxId = 1;
    return id;
}

String RegistroModulos::macParaString(const uint8_t mac[6])
{
    char buf[18];
    snprintf(buf, sizeof(buf),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

const char* RegistroModulos::nomeTipo(TipoModulo tipo)
{
    switch (tipo) {
        case TipoModulo::AR_CONDICIONADO: return "AR_CONDICIONADO";
        case TipoModulo::PROJETOR_IR:     return "PROJETOR_IR";
        case TipoModulo::TV_LG_IR:        return "TV_LG_IR";
        case TipoModulo::TELA_RF433:      return "TELA_RF433";
        case TipoModulo::LUZES_RELE:      return "LUZES_RELE";
        default:                          return "DESCONHECIDO";
    }
}

// =============================================================================
// EspNowCentral — inicialização
// =============================================================================

bool EspNowCentral::begin(const char* salaId, const char* nsNVS)
{
    _instancia = this;
    strncpy(_salaId, salaId, ESPNOW_MAX_SALA_ID - 1);

    WiFi.mode(WIFI_STA);
    WiFi.macAddress(_mac);

    if (esp_now_init() != ESP_OK) {
        debugErro("[EspNowCentral] Falha em esp_now_init()!");
        return false;
    }

    esp_now_register_recv_cb(_cbRecvEstatico);
    esp_now_register_send_cb(_cbSendEstatico);

    _registro.begin(nsNVS);

    // Registra todos os módulos já conhecidos como peers ESP-NOW
    for (uint8_t i = 0; i < _registro.total(); i++) {
        _adicionarPeer(_registro.obterPorIndice(i).mac);
    }

    debugInfo("[EspNowCentral] Iniciado — sala: " + String(_salaId));
    debugInfo("[EspNowCentral] MAC: " + mac());
    debugInfo("[EspNowCentral] Canal: " + String(canal()));
    debugInfo("[EspNowCentral] Modulos registrados: " + String(_registro.total()));

    return true;
}

// =============================================================================
// EspNowCentral — loop
// =============================================================================

void EspNowCentral::atualizar()
{
    _processarBuffer();
    _verificarTimeoutDescoberta();

    if (_intervaloHeartbeat > 0 &&
        millis() - _ultimoHeartbeat >= _intervaloHeartbeat) {
        _ultimoHeartbeat = millis();
        enviarHeartbeat();
        _verificarModulosOffline();
    }
}

// =============================================================================
// EspNowCentral — descoberta
// =============================================================================

void EspNowCentral::iniciarDescoberta(uint32_t timeoutMs)
{
    _descobertaAtiva    = true;
    _inicioDescoberta   = millis();
    _timeoutDescoberta  = timeoutMs;

    // Adiciona broadcast como peer temporário
    uint8_t broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    if (!esp_now_is_peer_exist(broadcast)) {
        esp_now_peer_info_t peer{};
        memcpy(peer.peer_addr, broadcast, 6);
        peer.channel = 0;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    }

    MsgPing ping;
    _preencherCabecalho(ping.cabecalho, TipoMsg::PING);
    _enviar(broadcast, reinterpret_cast<const uint8_t*>(&ping), sizeof(ping));

    debugInfo("[EspNowCentral] Descoberta iniciada (" +
              String(timeoutMs) + " ms)...");
}

bool EspNowCentral::descobertaAtiva() const { return _descobertaAtiva; }

void EspNowCentral::_verificarTimeoutDescoberta()
{
    if (!_descobertaAtiva) return;
    if (millis() - _inicioDescoberta >= _timeoutDescoberta) {
        _descobertaAtiva = false;
        debugInfo("[EspNowCentral] Descoberta encerrada.");
    }
}

// =============================================================================
// EspNowCentral — gerenciamento de módulos
// =============================================================================

bool EspNowCentral::registrarModulo(const InfoModulo& modulo)
{
    if (!_registro.adicionar(modulo)) return false;
    _adicionarPeer(modulo.mac);
    return true;
}

bool EspNowCentral::removerModulo(uint8_t id)
{
    InfoModulo* m = _registro.buscarPorId(id);
    if (!m) return false;

    esp_now_del_peer(m->mac);
    return _registro.remover(id);
}

void EspNowCentral::limparModulos()
{
    for (uint8_t i = 0; i < _registro.total(); i++) {
        esp_now_del_peer(_registro.obterPorIndice(i).mac);
    }
    _registro.limpar();
}

// =============================================================================
// EspNowCentral — heartbeat
// =============================================================================

void EspNowCentral::configurarHeartbeat(uint32_t intervaloMs)
{
    _intervaloHeartbeat = intervaloMs;
    debugVerbose("[EspNowCentral] Heartbeat: " +
                 (intervaloMs ? String(intervaloMs) + " ms" : String("desabilitado")));
}

void EspNowCentral::enviarHeartbeat()
{
    if (_registro.total() == 0) return;

    MsgHeartbeat hb;
    _preencherCabecalho(hb.cabecalho, TipoMsg::PING_HEARTBEAT);

    uint8_t enviados = 0;
    for (uint8_t i = 0; i < _registro.total(); i++) {
        if (_enviar(_registro.obterPorIndice(i).mac,
                    reinterpret_cast<const uint8_t*>(&hb), sizeof(hb))) {
            enviados++;
        }
    }
    debugVerbose("[EspNowCentral] Heartbeat enviado para " +
                 String(enviados) + " modulos.");
}

void EspNowCentral::_verificarModulosOffline()
{
    if (!_cbOffline || _intervaloHeartbeat == 0) return;

    uint32_t limite = _intervaloHeartbeat * 3;
    for (uint8_t i = 0; i < _registro.total(); i++) {
        InfoModulo& m = _registro.obterPorIndice(i);
        bool eriaOnline = m.online;

        if (millis() - m.ultimoContato > limite) {
            if (m.online) {
                m.online = false;
                debugAviso("[EspNowCentral] Modulo offline: " + String(m.label));
                if (_cbOffline) _cbOffline(m);
            }
        }
    }
}

// =============================================================================
// EspNowCentral — utilitários
// =============================================================================

uint8_t EspNowCentral::canal() const
{
    return WiFi.channel();
}

// =============================================================================
// EspNowCentral — processamento de buffer (ISR → loop)
// =============================================================================

void EspNowCentral::_processarBuffer()
{
    if (!_temDados) return;

    uint8_t  buf[250];
    int      tam;
    uint8_t  macOrigem[6];

    noInterrupts();
    tam = _tamanhoRx;
    memcpy(buf, _bufferRx, tam);
    memcpy(macOrigem, _macRx, 6);
    _temDados = false;
    interrupts();

    if (tam < (int)sizeof(CabecalhoMsg)) return;

    CabecalhoMsg cab;
    memcpy(&cab, buf, sizeof(cab));

    if (cab.versao != ESPNOW_VERSAO_PROTOCOLO) {
        debugAviso("[EspNowCentral] Versao de protocolo incompativel: " +
                   String(cab.versao));
        return;
    }

    // Atualiza timestamp de contato do módulo
    InfoModulo* m = _registro.buscarPorMac(macOrigem);
    if (m) {
        m->online        = true;
        m->ultimoContato = millis();
    }

    switch (cab.tipo) {
        case TipoMsg::PONG: {
            if (tam < (int)sizeof(MsgPong)) return;
            MsgPong pong;
            memcpy(&pong, buf, sizeof(pong));
            _processarPong(macOrigem, pong);
            break;
        }
        case TipoMsg::STATUS: {
            if (tam < (int)sizeof(MsgStatus)) return;
            MsgStatus status;
            memcpy(&status, buf, sizeof(status));
            _processarStatus(macOrigem, status);
            break;
        }
        case TipoMsg::PONG_HEARTBEAT: {
            _processarHeartbeatPong(macOrigem);
            break;
        }
        default:
            debugVerbose("[EspNowCentral] TipoMsg desconhecido: 0x" +
                         String((uint8_t)cab.tipo, HEX));
            break;
    }
}

void EspNowCentral::_processarPong(const uint8_t* macOrigem, const MsgPong& pong)
{
    debugInfo("[EspNowCentral] PONG de " +
              RegistroModulos::macParaString(macOrigem) +
              " — " + String(pong.label) +
              " (" + RegistroModulos::nomeTipo(pong.tipo) + ")");

    // Monta InfoModulo para o callback
    InfoModulo info{};
    memcpy(info.mac, macOrigem, 6);
    info.tipo           = pong.tipo;
    info.id             = 0; // será atribuído em registrarModulo()
    info.versaoFirmware = pong.versaoFirmware;
    info.online         = true;
    info.ultimoContato  = millis();
    strncpy(info.label, pong.label, ESPNOW_MAX_LABEL - 1);

    // Verifica se já está cadastrado
    bool jaRegistrado = (_registro.buscarPorMac(macOrigem) != nullptr);
    if (jaRegistrado) {
        debugVerbose("[EspNowCentral] Modulo ja registrado, atualizando contato.");
        InfoModulo* m = _registro.buscarPorMac(macOrigem);
        m->online        = true;
        m->ultimoContato = millis();
        return;
    }

    if (_cbEncontrado) _cbEncontrado(info);
}

void EspNowCentral::_processarStatus(const uint8_t* macOrigem, const MsgStatus& status)
{
    InfoModulo* m = _registro.buscarPorMac(macOrigem);
    if (!m) {
        debugAviso("[EspNowCentral] STATUS de modulo nao registrado: " +
                   RegistroModulos::macParaString(macOrigem));
        return;
    }

    debugVerbose("[EspNowCentral] STATUS de " + String(m->label) +
                 " — RSSI: " + String(status.rssi));

    if (_cbStatus) _cbStatus(*m, status);
}

void EspNowCentral::_processarHeartbeatPong(const uint8_t* macOrigem)
{
    InfoModulo* m = _registro.buscarPorMac(macOrigem);
    if (m) {
        m->online        = true;
        m->ultimoContato = millis();
        debugVerbose("[EspNowCentral] Heartbeat PONG de " + String(m->label));
    }
}

// =============================================================================
// EspNowCentral — internos de envio
// =============================================================================

bool EspNowCentral::_adicionarPeer(const uint8_t mac[6])
{
    if (esp_now_is_peer_exist(mac)) return true;

    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;

    bool ok = (esp_now_add_peer(&peer) == ESP_OK);
    if (!ok) {
        debugErro("[EspNowCentral] Falha ao adicionar peer: " +
                  RegistroModulos::macParaString(mac));
    }
    return ok;
}

bool EspNowCentral::_enviar(const uint8_t mac[6],
                              const uint8_t* dados, uint8_t tamanho)
{
    esp_err_t r = esp_now_send(mac, dados, tamanho);
    if (r != ESP_OK) {
        debugErro("[EspNowCentral] esp_now_send falhou (erro: " +
                  String(r) + ") para " + RegistroModulos::macParaString(mac));
        return false;
    }
    return true;
}

// =============================================================================
// EspNowCentral — callbacks estáticos (ISR-safe)
// =============================================================================

void EspNowCentral::_cbRecvEstatico(const esp_now_recv_info_t* info,
                                     const uint8_t* dados, int tamanho)
{
    if (!_instancia || _instancia->_temDados) return;

    int copia = (tamanho > 250) ? 250 : tamanho;
    memcpy((void*)_instancia->_bufferRx, dados, copia);
    memcpy(_instancia->_macRx, info->src_addr, 6);
    _instancia->_tamanhoRx = copia;
    _instancia->_temDados  = true;
}

void EspNowCentral::_cbSendEstatico(const uint8_t* mac,
                                     esp_now_send_status_t status)
{
    if (!_instancia) return;
    bool ok = (status == ESP_NOW_SEND_SUCCESS);

    if (!ok) {
        debugAviso("[EspNowCentral] Falha de entrega para: " +
                   RegistroModulos::macParaString(mac));
    }
    if (_instancia->_cbEnvio) _instancia->_cbEnvio(mac, ok);
}

// =============================================================================
// EspNowModulo — inicialização
// =============================================================================

bool EspNowModulo::begin(TipoModulo tipo, const char* label, uint8_t versaoFirmware)
{
    _instancia = this;
    _tipo      = tipo;
    strncpy(_label, label, ESPNOW_MAX_LABEL - 1);
    _versaoFirmware = versaoFirmware;

    WiFi.mode(WIFI_STA);
    WiFi.macAddress(_mac);

    if (esp_now_init() != ESP_OK) {
        debugErro("[EspNowModulo] Falha em esp_now_init()!");
        return false;
    }

    esp_now_register_recv_cb(_cbRecvEstatico);
    esp_now_register_send_cb(_cbSendEstatico);

    debugInfo("[EspNowModulo] Iniciado — label: " + String(_label) +
              " | tipo: " + RegistroModulos::nomeTipo(_tipo));
    debugInfo("[EspNowModulo] MAC: " + mac());
    debugInfo("[EspNowModulo] Aguardando PING do central...");

    return true;
}

// =============================================================================
// EspNowModulo — loop
// =============================================================================

void EspNowModulo::atualizar()
{
    _processarBuffer();
}

// =============================================================================
// EspNowModulo — comunicação
// =============================================================================

bool EspNowModulo::reportarStatus(const uint8_t payloadEstado[8])
{
    if (!_centralRegistrado) {
        debugAviso("[EspNowModulo] reportarStatus: central nao registrado.");
        return false;
    }

    MsgStatus status;
    _preencherCabecalho(status.cabecalho, TipoMsg::STATUS, _id);
    status.tipo = _tipo;
    memcpy(status.payloadEstado, payloadEstado, 8);
    status.rssi = WiFi.RSSI();

    return _enviar(_macCentral,
                   reinterpret_cast<const uint8_t*>(&status),
                   sizeof(status));
}

String EspNowModulo::macCentral() const
{
    if (!_centralRegistrado) return "";
    return RegistroModulos::macParaString(_macCentral);
}

// =============================================================================
// EspNowModulo — processamento de buffer
// =============================================================================

void EspNowModulo::_processarBuffer()
{
    if (!_temDados) return;

    uint8_t buf[250];
    int     tam;
    uint8_t macOrigem[6];

    noInterrupts();
    tam = _tamanhoRx;
    memcpy(buf, _bufferRx, tam);
    memcpy(macOrigem, _macRx, 6);
    _temDados = false;
    interrupts();

    if (tam < (int)sizeof(CabecalhoMsg)) return;

    CabecalhoMsg cab;
    memcpy(&cab, buf, sizeof(cab));

    if (cab.versao != ESPNOW_VERSAO_PROTOCOLO) {
        debugAviso("[EspNowModulo] Versao incompativel: " + String(cab.versao));
        return;
    }

    switch (cab.tipo) {
        case TipoMsg::PING:
            _registrarCentral(macOrigem);
            _responderPing(macOrigem);
            break;

        case TipoMsg::PING_HEARTBEAT:
            _responderHeartbeat(macOrigem);
            break;

        // Todos os tipos de comando são repassados ao callback
        case TipoMsg::CMD_AC:
        case TipoMsg::CMD_PROJETOR:
        case TipoMsg::CMD_TV:
        case TipoMsg::CMD_TELA:
        case TipoMsg::CMD_LUZES:
            if (_cbComando) _cbComando(buf, (uint8_t)tam);
            break;

        default:
            debugVerbose("[EspNowModulo] TipoMsg desconhecido: 0x" +
                         String((uint8_t)cab.tipo, HEX));
            break;
    }
}

void EspNowModulo::_responderPing(const uint8_t* macCentral)
{
    MsgPong pong;
    _preencherCabecalho(pong.cabecalho, TipoMsg::PONG, _id);
    pong.tipo           = _tipo;
    pong.versaoFirmware = _versaoFirmware;
    strncpy(pong.label, _label, ESPNOW_MAX_LABEL - 1);

    _enviar(macCentral,
            reinterpret_cast<const uint8_t*>(&pong),
            sizeof(pong));

    debugInfo("[EspNowModulo] PONG enviado ao central.");
}

void EspNowModulo::_responderHeartbeat(const uint8_t* macCentral)
{
    MsgHeartbeat hb;
    _preencherCabecalho(hb.cabecalho, TipoMsg::PONG_HEARTBEAT, _id);

    _enviar(macCentral,
            reinterpret_cast<const uint8_t*>(&hb),
            sizeof(hb));

    debugVerbose("[EspNowModulo] Heartbeat PONG enviado.");
}

void EspNowModulo::_registrarCentral(const uint8_t* mac)
{
    if (_centralRegistrado && _macsIguais(_macCentral, mac)) return;

    memcpy(_macCentral, mac, 6);
    _centralRegistrado = true;
    _adicionarPeer(mac);

    debugInfo("[EspNowModulo] Central registrado: " +
              RegistroModulos::macParaString(mac));
}

// =============================================================================
// EspNowModulo — internos de envio
// =============================================================================

bool EspNowModulo::_adicionarPeer(const uint8_t mac[6])
{
    if (esp_now_is_peer_exist(mac)) return true;

    esp_now_peer_info_t peer{};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;

    bool ok = (esp_now_add_peer(&peer) == ESP_OK);
    if (!ok) {
        debugErro("[EspNowModulo] Falha ao adicionar peer: " +
                  RegistroModulos::macParaString(mac));
    }
    return ok;
}

bool EspNowModulo::_enviar(const uint8_t mac[6],
                             const uint8_t* dados, uint8_t tamanho)
{
    esp_err_t r = esp_now_send(mac, dados, tamanho);
    if (r != ESP_OK) {
        debugErro("[EspNowModulo] esp_now_send falhou (erro: " + String(r) + ")");
        return false;
    }
    return true;
}

// =============================================================================
// EspNowModulo — callbacks estáticos (ISR-safe)
// =============================================================================

void EspNowModulo::_cbRecvEstatico(const esp_now_recv_info_t* info,
                                    const uint8_t* dados, int tamanho)
{
    if (!_instancia || _instancia->_temDados) return;

    int copia = (tamanho > 250) ? 250 : tamanho;
    memcpy((void*)_instancia->_bufferRx, dados, copia);
    memcpy(_instancia->_macRx, info->src_addr, 6);
    _instancia->_tamanhoRx = copia;
    _instancia->_temDados  = true;
}

void EspNowModulo::_cbSendEstatico(const uint8_t* mac,
                                    esp_now_send_status_t status)
{
    if (!_instancia) return;
    bool ok = (status == ESP_NOW_SEND_SUCCESS);
    if (!ok) {
        debugAviso("[EspNowModulo] Falha de entrega ao central.");
    }
    if (_instancia->_cbEnvio) _instancia->_cbEnvio(mac, ok);
}
