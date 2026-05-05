#pragma once

// Los valores de configuración del AP están separados en este archivo para facilitar la actualización.
// Modifique estos valores directamente según sea necesario antes de compilar.

constexpr char AP_SSID[] = "AP-ESP32-S3";
constexpr char AP_PASSWORD[] = "016374856";
constexpr int AP_CHANNEL = 6;

// Configuración de IP estática para el AP (misma subred que softAPIP)
constexpr uint8_t AP_IP_OCTETS[4] = {10, 10, 192, 1};
constexpr uint8_t AP_GATEWAY_OCTETS[4] = {10, 10, 192, 1};
constexpr uint8_t AP_SUBNET_OCTETS[4] = {255, 255, 255, 0};

// Credenciales STA para upstream Internet (vacío significa no usar STA).
constexpr char STA_SSID[] = "bastianAP";
constexpr char STA_PASSWORD[] = "bastianRul3s*";

// API opcional para Lookup de MAC remota (recomendado, pero no obligatorio).
constexpr char MACLOOKUP_API_KEY[] = "";