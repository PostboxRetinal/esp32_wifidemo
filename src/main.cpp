#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sta_list.h"
#include "wifi_config.h"
#include "oui_lookup.h"
#include <vector>

struct PendingMacLookup
{
    String macStr;
    uint8_t macRaw[6];
};

static std::vector<PendingMacLookup> pendingMacLookups;
static std::vector<String> seenClientMacs;

const uint8_t RGB_LED_PIN = RGB_BUILTIN;
const uint8_t RGB_LED_LEVEL = RGB_BRIGHTNESS;
const unsigned long STA_DISCONNECT_LOG_INTERVAL_MS = 30000;

static bool staDisconnectReasonSeen = false;
static uint8_t lastStaDisconnectReason = 0;
static unsigned long lastStaDisconnectLogTime = 0;
static unsigned int suppressedStaDisconnects = 0;

/**
 * @brief Dirección IP de AP (hardcoded desde wifi_config.h).
 */
const IPAddress AP_IP(AP_IP_OCTETS[0], AP_IP_OCTETS[1], AP_IP_OCTETS[2], AP_IP_OCTETS[3]);

/**
 * @brief Gateway de AP.
 */
const IPAddress AP_GATEWAY(AP_GATEWAY_OCTETS[0], AP_GATEWAY_OCTETS[1], AP_GATEWAY_OCTETS[2], AP_GATEWAY_OCTETS[3]);

/**
 * @brief Máscara de subred de AP.
 */
const IPAddress AP_SUBNET(AP_SUBNET_OCTETS[0], AP_SUBNET_OCTETS[1], AP_SUBNET_OCTETS[2], AP_SUBNET_OCTETS[3]);

/**
 * @brief Convierte dirección MAC a cadena de texto (hex).
 * @param mac Dirección MAC de 6 bytes.
 * @return String con formato XX:XX:XX:XX:XX:XX.
 */
String macToString(const uint8_t mac[6])
{
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

/**
 * @brief Busca la IP asignada al cliente identificado por MAC en la lista de estaciones AP.
 * @param mac MAC del cliente.
 * @return IP en formato string, o "desconocido".
 */
String getStationIpByMac(const uint8_t mac[6])
{
    wifi_sta_list_t wifi_sta_list;
    esp_netif_sta_list_t netif_sta_list;

    esp_wifi_ap_get_sta_list(&wifi_sta_list);
    if (esp_netif_get_sta_list(&wifi_sta_list, &netif_sta_list) != ESP_OK)
    {
        return "desconocido";
    }

    for (int i = 0; i < netif_sta_list.num; i++)
    {
        if (memcmp(mac, netif_sta_list.sta[i].mac, 6) == 0)
        {
            return IPAddress(netif_sta_list.sta[i].ip.addr).toString();
        }
    }
    return "desconocido";
}

/**
 * @brief Apaga el LED RGB integrado.
 */
void rgbLedOff()
{
    neopixelWrite(RGB_LED_PIN, 0, 0, 0);
}

/**
 * @brief Enciende el LED RGB integrado con el color indicado.
 */
void rgbLedOn(uint8_t red, uint8_t green, uint8_t blue)
{
    neopixelWrite(RGB_LED_PIN, red, green, blue);
}

/**
 * @brief Parpadea el LED RGB n veces y lo deja apagado al final.
 *
 * @param times Cantidad de pulsos.
 * @param ms Duración de cada estado (ms).
 */
void blinkRgbLed(uint8_t red, uint8_t green, uint8_t blue, int times, int ms = 80)
{
    for (int i = 0; i < times; ++i)
    {
        rgbLedOn(red, green, blue);
        delay(ms);
        rgbLedOff();
        delay(ms);
    }
    rgbLedOff();
}

/**
 * @brief Registra una MAC vista y reporta si ya era conocida en este arranque.
 */
bool registerSeenClient(const String &mac)
{
    for (const String &seenMac : seenClientMacs)
    {
        if (seenMac == mac)
        {
            return true;
        }
    }

    seenClientMacs.push_back(mac);
    return false;
}

/**
 * @brief Nombre corto para motivos comunes de desconexión STA.
 */
const char *wifiReasonName(uint8_t reason)
{
    switch (reason)
    {
    case WIFI_REASON_BEACON_TIMEOUT:
        return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:
        return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:
        return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:
        return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL:
        return "CONNECTION_FAIL";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief Reduce el ruido de reconexiones STA repetidas con el mismo motivo.
 */
void logStaDisconnect(uint8_t reason)
{
    unsigned long now = millis();
    bool reasonChanged = !staDisconnectReasonSeen || reason != lastStaDisconnectReason;

    if (reasonChanged)
    {
        suppressedStaDisconnects = 0;
    }

    if (reasonChanged || (now - lastStaDisconnectLogTime >= STA_DISCONNECT_LOG_INTERVAL_MS))
    {
        unsigned int totalEvents = suppressedStaDisconnects + 1;
        if (totalEvents > 1)
        {
            Serial.printf("STA desconectado upstream, reason=%d (%s), eventos=%u\n",
                          reason, wifiReasonName(reason), totalEvents);
        }
        else
        {
            Serial.printf("STA desconectado upstream, reason=%d (%s)\n",
                          reason, wifiReasonName(reason));
        }

        staDisconnectReasonSeen = true;
        lastStaDisconnectReason = reason;
        lastStaDisconnectLogTime = now;
        suppressedStaDisconnects = 0;
        return;
    }

    ++suppressedStaDisconnects;
}

/**
 * @brief Reinicia el resumen de desconexiones STA cuando el upstream conecta.
 */
void resetStaDisconnectLog()
{
    staDisconnectReasonSeen = false;
    lastStaDisconnectReason = 0;
    lastStaDisconnectLogTime = 0;
    suppressedStaDisconnects = 0;
}

/**
 * @brief Manejador de eventos WiFi/AP+STA.
 *
 * - WIFI_EVENT_AP_STACONNECTED / WIFI_EVENT_AP_STADISCONNECTED: cliente AP local.
 * - WIFI_EVENT_STA_DISCONNECTED: STA upstream desconectado.
 * - IP_EVENT_STA_GOT_IP: STA upstream obtuvo IP de red.
 *
 * @param arg Puntero a datos de contexto (no usado).
 * @param event_base Base del evento: WIFI_EVENT o IP_EVENT.
 * @param event_id Id del evento.
 * @param event_data Datos específicos del evento.
 */
void onWiFiEvent(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT)
    {
        if (event_id == WIFI_EVENT_AP_STACONNECTED)
        {
            auto *evt = (wifi_event_ap_staconnected_t *)event_data;
            String mac = macToString(evt->mac);
            bool clientKnown = registerSeenClient(mac);
            String ip = getStationIpByMac(evt->mac);
            const char *vendor = lookupVendorCached(evt->mac, mac.c_str());
            Serial.printf("Cliente conectado: MAC=%s (%s), AID=%d, IP=%s, tipo=%s, total=%d\n",
                          mac.c_str(), vendor, evt->aid, ip.c_str(), clientKnown ? "reconexion" : "nuevo", WiFi.softAPgetStationNum());
            blinkRgbLed(0, clientKnown ? RGB_LED_LEVEL : 0, clientKnown ? 0 : RGB_LED_LEVEL, 2);
            PendingMacLookup pending;
            pending.macStr = mac;
            memcpy(pending.macRaw, evt->mac, 6);
            pendingMacLookups.push_back(pending);
        }
        else if (event_id == WIFI_EVENT_AP_STADISCONNECTED)
        {
            auto *evt = (wifi_event_ap_stadisconnected_t *)event_data;
            String mac = macToString(evt->mac);
            const char *vendor = lookupVendorCached(evt->mac, mac.c_str());
            Serial.printf("Cliente desconectado: MAC=%s (%s), AID=%d, total=%d\n",
                          mac.c_str(), vendor, evt->aid, WiFi.softAPgetStationNum());
            blinkRgbLed(RGB_LED_LEVEL, 0, 0, 1);
        }
        else if (event_id == WIFI_EVENT_STA_DISCONNECTED)
        {
            auto *evt = (wifi_event_sta_disconnected_t *)event_data;
            logStaDisconnect(evt->reason);
        }
    }
    else if (event_base == IP_EVENT)
    {
        if (event_id == IP_EVENT_STA_GOT_IP)
        {
            auto *evt = (ip_event_got_ip_t *)event_data;
            resetStaDisconnectLog();
            Serial.printf("STA conectado a upstream: IP=%s, GW=%s, mask=%s\n",
                          IPAddress(evt->ip_info.ip.addr).toString().c_str(),
                          IPAddress(evt->ip_info.gw.addr).toString().c_str(),
                          IPAddress(evt->ip_info.netmask.addr).toString().c_str());
        }
    }
}

/**
 * @brief Registra controladores de evento para AP y STA (upstream).
 *
 * - Eventos AP: WIFI_EVENT_AP_STACONNECTED, WIFI_EVENT_AP_STADISCONNECTED.
 * - Eventos STA: WIFI_EVENT_STA_DISCONNECTED, IP_EVENT_STA_GOT_IP.
 */
void registerWiFiEventHandlers()
{
    esp_event_handler_instance_t instance;
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &onWiFiEvent, NULL, &instance);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, &onWiFiEvent, NULL, &instance);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &onWiFiEvent, NULL, &instance);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &onWiFiEvent, NULL, &instance);
}

/**
 * @brief Inicializa el modo punto de acceso (AP) y registra eventos.
 */
void initWiFiAP()
{
    WiFi.mode(WIFI_MODE_APSTA);
    bool configOk = WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
    if (!configOk)
    {
        Serial.println("Fallo en configuración del AP (softAPConfig).\n");
        return;
    }

    bool apOk = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, 4);
    if (!apOk)
    {
        Serial.println("Fallo al iniciar AP (softAP).\n");
        return;
    }

    String ip = WiFi.softAPIP().toString();
    Serial.printf("AP iniciado: SSID=%s, PASS=%s, CH=%d, IP=%s\n",
                  AP_SSID, AP_PASSWORD, AP_CHANNEL, ip.c_str());

    if (strlen(STA_SSID) > 0)
    {
        Serial.printf("Iniciando STA upstream: SSID=%s\n", STA_SSID);
        WiFi.begin(STA_SSID, STA_PASSWORD);
    }
    else
    {
        Serial.println("No se configuro STA upstream, solo AP activo.");
    }

    registerWiFiEventHandlers();
}

/**
 * @brief Configuración inicial de hardware y red en el arranque.
 */
void setup()
{
    pinMode(RGB_LED_PIN, OUTPUT);
    rgbLedOff();
    Serial.begin(115200);
    delay(100);
    initWiFiAP();
}

/**
 * @brief Bucle principal, imprime estado de AP periódicamente.
 */
void loop()
{
    static unsigned long lastStatus = 0;
    unsigned long now = millis();

    if (now - lastStatus >= 10000)
    {
        lastStatus = now;
        Serial.printf("AP en ejecución: clientes=%d, SSID=%s, IP=%s\n",
                      WiFi.softAPgetStationNum(),
                      AP_SSID,
                      WiFi.softAPIP().toString().c_str());
    }

    /**
     * @brief Procesar MAC lookup remoto en segundo plano para cada cliente conectado.
     *
     * Esto evita bloquear el evento de conexión y permite limitar la tasa de consultas
     * para no saturar la API externa cuando se conectan muchos clientes simultáneos.
     */
    static unsigned long lastLookupTime = 0;

    /**
     * @brief Intervalo mínimo entre consultas remotas (ms).
     */
    const unsigned long lookupIntervalMs = 2000; // 1 consulta cada 2s

    if (!pendingMacLookups.empty() && WiFi.status() == WL_CONNECTED && (now - lastLookupTime >= lookupIntervalMs))
    {
        lastLookupTime = now;
        PendingMacLookup pending = pendingMacLookups.front();
        pendingMacLookups.erase(pendingMacLookups.begin());

        String remVendor = lookupVendorRemote(pending.macStr.c_str());
        if (remVendor.length() > 0 && remVendor != "NoRed")
        {
            Serial.printf("MAC %s -> vendor remoto: %s\n", pending.macStr.c_str(), remVendor.c_str());
        }
        else
        {
            Serial.printf("MAC %s -> vendor remoto no disponible (%s)\n", pending.macStr.c_str(), remVendor.c_str());
        }
    }

    delay(100);
}
