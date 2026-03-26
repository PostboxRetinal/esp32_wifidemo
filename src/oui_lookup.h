#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "wifi_config.h"

/**
 * @brief Entrada de tabla OUI para resolución de fabricante.
 *
 * La OUI es el primer tercio de la dirección MAC (3 bytes).
 */
struct OuiEntry {
    uint8_t oui[3];
    const char* vendor;
};

/**
 * @brief Tabla local de OUIs para marcas celulares comunes.
 */
static const OuiEntry oui_table[] = {
    {{0x3C, 0x5A, 0x37}, "Apple"},
    {{0xE4, 0x5F, 0x01}, "Samsung"},
    {{0xBC, 0xDD, 0xC2}, "Xiaomi"},
    {{0xF8, 0x1A, 0x67}, "Huawei"},
    {{0x44, 0x65, 0x0D}, "Amazon"},
    {{0xCC, 0x3A, 0x61}, "LG"},
    {{0x00, 0x25, 0x9C}, "Google"},
    {{0x5C, 0x51, 0x88}, "OnePlus"},
    {{0xD8, 0xA0, 0x1D}, "Motorola"},
    {{0x78, 0x4F, 0x43}, "Sony"},
    {{0x00, 0x0F, 0xB5}, "HTC"},
    {{0x2C, 0x01, 0x00}, "Nokia"},
};

/**
 * @brief Resuelve el nombre de fabricante mediante OUI local.
 *
 * @param mac MAC de 6 bytes.
 * @return Nombre del fabricante encontrado o "Desconocido".
 */
inline const char* lookupVendorByMac(const uint8_t mac[6]) {
    for (const auto &entry : oui_table) {
        if (mac[0] == entry.oui[0] && mac[1] == entry.oui[1] && mac[2] == entry.oui[2]) {
            return entry.vendor;
        }
    }
    return "Desconocido";
}

/**
 * @brief Normaliza una dirección MAC para query a la API remota.
 *
 * Remueve delimitadores ":" y "-", y devuelve minúsculas.
 *
 * @param mac MAC en formato "XX:XX:XX:XX:XX:XX" o análogo.
 * @return MAC normalizada ("xxxxxxxxxxxx").
 */
static String normalizeMacForApi(const String& mac) {
    String s = mac;
    s.replace(":", "");
    s.replace("-", "");
    s.toLowerCase();
    return s;
}

static const size_t MAC_VENDOR_CACHE_SIZE = 24;

struct MacVendorCacheEntry {
    String mac;
    String vendor;
};

/**
 * @brief Busca un vendor cached para una MAC normalizada.
 *
 * @param normalizedMac MAC normalizada (12 hex sin separadores).
 * @return Vendor encontrado o String vacío si no existe.
 */
inline String lookupVendorFromCache(const String &normalizedMac)
{
    static MacVendorCacheEntry cache[MAC_VENDOR_CACHE_SIZE];
    for (size_t i = 0; i < MAC_VENDOR_CACHE_SIZE; i++) {
        if (cache[i].mac.length() > 0 && cache[i].mac == normalizedMac) {
            return cache[i].vendor;
        }
    }
    return String();
}

/**
 * @brief Resuelve fabricante con cache local + runtime, sin invocar red.
 *
 * @param mac MAC de 6 bytes.
 * @param macStr MAC en formato "XX:XX:XX:XX:XX:XX".
 * @return vendor si local o cache disponible, o "Desconocido".
 */
inline const char* lookupVendorCached(const uint8_t mac[6], const char* macStr)
{
    const char* local = lookupVendorByMac(mac);
    if (strcmp(local, "Desconocido") != 0) {
        return local;
    }

    String normalizedMac = normalizeMacForApi(String(macStr));
    String cached = lookupVendorFromCache(normalizedMac);
    if (cached.length() > 0) {
        static String cachedVendor;
        cachedVendor = cached;
        return cachedVendor.c_str();
    }

    return "Desconocido";
}

/**
 * @brief Inserta (o actualiza) un registro en la caché LRU circular de vendor.
 *
 * @param normalizedMac MAC normalizada (12 hex, sin separadores).
 * @param vendor Nombre del fabricante o código de error de la consulta.
 */
inline void addVendorToCache(const String &normalizedMac, const String &vendor)
{
    static MacVendorCacheEntry cache[MAC_VENDOR_CACHE_SIZE];
    static size_t cachePos = 0;

    for (size_t i = 0; i < MAC_VENDOR_CACHE_SIZE; i++) {
        if (cache[i].mac == normalizedMac) {
            cache[i].vendor = vendor;
            return;
        }
    }

    cache[cachePos].mac = normalizedMac;
    cache[cachePos].vendor = vendor;
    cachePos = (cachePos + 1) % MAC_VENDOR_CACHE_SIZE;
}

inline String lookupVendorRemote(const char* macStr) {
    if (WiFi.status() != WL_CONNECTED) return "NoRed";
    if (WiFi.localIP() == IPAddress(0, 0, 0, 0)) return "NoIP";

    String normalizedMac = normalizeMacForApi(String(macStr));
    if (normalizedMac.length() != 12) return "NoMAC";

    String cached = lookupVendorFromCache(normalizedMac);
    if (cached.length() > 0) {
        return cached;
    }

    WiFiClientSecure* client = new WiFiClientSecure;
    if (!client) return "ErrorMem";
    client->setInsecure(); // sólo para consulta de OUI, no se transmitirá info sensible ni se mantendrá conexión persistente

    HTTPClient http;
    String url = String("https://api.maclookup.app/v2/macs/") + normalizedMac;
    http.begin(*client, url);
    http.addHeader("Accept", "application/json");

    if (strlen(MACLOOKUP_API_KEY) > 0) {
        http.addHeader("X-Api-Key", MACLOOKUP_API_KEY);
    }

    http.setTimeout(5000);

    const int maxRetries = 3;
    int code = 0;
    String payload;

    for (int attempt = 0; attempt < maxRetries; attempt++) {
        code = http.GET();
        if (code > 0) {
            payload = http.getString();
            break;
        }
        if (code == HTTPC_ERROR_READ_TIMEOUT && attempt < maxRetries - 1) {
            Serial.printf("maclookup read timeout, retry %d/%d\n", attempt + 1, maxRetries - 1);
            delay(800);
            continue;
        }
        break;
    }

    if (code != HTTP_CODE_OK) {
        http.end();
        client->stop();
        delete client;

        if (code == HTTPC_ERROR_CONNECTION_REFUSED || code == HTTPC_ERROR_SEND_HEADER_FAILED || code == HTTPC_ERROR_SEND_PAYLOAD_FAILED || code == HTTPC_ERROR_NOT_CONNECTED || code == HTTPC_ERROR_CONNECTION_LOST || code == HTTPC_ERROR_NO_STREAM) {
            Serial.printf("maclookup connect fail %d\n", code);
            return "ErrorConn";
        }
        if (code == HTTPC_ERROR_READ_TIMEOUT) {
            Serial.printf("maclookup read timeout %d\n", code);
            return "ErrorTimeout";
        }
        if (code == HTTP_CODE_NOT_FOUND) {
            addVendorToCache(normalizedMac, "NoEncontrado");
            return "NoEncontrado";
        }
        if (code == 429) return "RateLimit";
        if (code == HTTP_CODE_UNAUTHORIZED || code == HTTP_CODE_FORBIDDEN) return "NoAuth";

        Serial.printf("maclookup HTTP err %d body=%s\n", code, payload.c_str());
        return "ErrorHTTP";
    }

    http.end();
    client->stop();
    delete client;

    StaticJsonDocument<512> doc;
    auto err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("maclookup parse err %s\n", err.c_str());
        return "ErrorJSON";
    }

    if (doc.containsKey("company")) {
        String name = doc["company"].as<String>();
        if (name.length() > 0) {
            addVendorToCache(normalizedMac, name);
            return name;
        }
    }

    addVendorToCache(normalizedMac, "Desconocido");
    return "Desconocido";
}

/**
 * @brief Resuelve fabricante de MAC usando lookup local y fallback remoto.
 *
 * - Local: tabla OUI.
 * - Remoto: API maclookup.app (cuando está disponible).
 *
 * @param mac MAC de 6 bytes.
 * @param macStr MAC con formato de texto.
 * @return Nombre del fabricante o "Desconocido".
 */
inline const char* resolveVendorFromMac(const uint8_t mac[6], const char* macStr) {
    const char* local = lookupVendorByMac(mac);
    if (strcmp(local, "Desconocido") != 0) {
        return local;
    }

    String remote = lookupVendorRemote(macStr);
    if (remote != "Desconocido" && remote != "ErrorHTTP" && remote != "ErrorJSON" && remote != "NoRed") {
        static String lastVendor;
        lastVendor = remote;
        return lastVendor.c_str();
    }

    return "Desconocido";
}
