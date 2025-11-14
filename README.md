# ESP32-WROVER-PPP-AMB

Proyecto para **ESP32-WROVER** (ESP-IDF) orientado a **medición ambiental** con **conectividad celular (PPP sobre módem 4G LTE)** y **envío de datos a Firebase Realtime Database**.  
Incluye **geolocalización aproximada por celdas** usando **Unwired Labs** (a partir de la información de la red celular). Licencia **MIT**.

---

## ¿Qué hace?

- **Mide variables ambientales** (p. ej., PM1.0/2.5/4.0/10, VOC, NOx, CO₂, temperatura, humedad) y arma un **payload JSON** con las lecturas.  
- Establece **conectividad IP** vía **PPP** usando un **módem 4G LTE (T-A7670X/SIM7600)** y la librería **`esp_modem`** de ESP-IDF.
- **Envía mediciones a Firebase** mediante **HTTP/REST** (componente `esp_firebase`).  
- **Obtiene ubicación aproximada** (ciudad/lat-lon) consultando **Unwired Labs** con parámetros de celda obtenidos por **AT** desde el módem.  
- Permite **configurar claves** y ajustes sensibles en `Privado.h` (no versionado).  
- **Intervalo de envío configurable** y **promedio local configurable**: el dispositivo acumula **N muestras** y **envía solo el promedio** para reducir ruido/uso de red.

---

## Requisitos

- **ESP-IDF 5.x** (recomendado 5.5.1) y **Python 3.11.x** para el toolchain.
- **Target**: `esp32`.  
- **Hardware**: ESP32-WROVER + **módem 4G LTE T-A7670X/SIM7600** con SIM de datos.  
- **Firebase Realtime Database** operativo (URL, API Key, y si aplica: email/password).  
- **Token de Unwired Labs** para geolocalización por celdas.  
- En `sdkconfig` debe estar habilitado **PPP** (ej.: `CONFIG_LWIP_PPP_SUPPORT=y`).

---

## Funcionalidades

### 1) Conectividad celular (PPP + esp_modem)
- **Secuencia de encendido** del módem (POWERON/RST/PWRKEY/DTR), **creación del DTE/DCE**, alta de **PPP** y espera a `IP_EVENT_PPP_GOT_IP`.  
- Interfaz **PPP** marcada como **default** y **fallback de DNS** si el APN no los entrega (para evitar errores `getaddrinfo()`), de acuerdo con el flujo documentado en el repo.
- Operación estable en modo **DATA** (sin CMUX). *(CMUX puede evaluarse después; el proyecto contempla fallback.)*

### 2) Geolocalización por celdas (Unwired Labs)
- Obtiene del módem los **parámetros de celda** por **AT** (p. ej., MCC, MNC, LAC/TAC y CID).  
- Llama al endpoint de **Unwired Labs** para resolver **ubicación aproximada** (útil para etiquetar mediciones o enriquecer el JSON, p. ej. en la clave `ciudad`).  
- El módulo de apoyo (**`unwiredlabs.c/h`**) está presente en `main/` y forma parte del flujo actual del proyecto.

### 3) Medición ambiental (sensors)
- Lectura periódica de sensores y construcción de un **JSON** con las claves de medición.  
- **Promedio local**: se acumulan **N muestras** (configurable) y se **envía el promedio** (reduce picos y ancho de banda).  
- **Intervalo de envío** configurable.

### 4) Envío de datos a Firebase (components/esp_firebase)
- Cliente **REST** para **Firebase Realtime Database** con autenticación (API Key y, si aplica, email/password) y operaciones **push/set/remove**.  
- Uso de **bundle de certificados** de ESP-IDF para **TLS** cuando corresponda.  
- Estructura de **rutas** y **payloads** pensada para series temporales.

### 5) Configuración y credenciales (Privado.h)
- **No versionado**. Contiene **APN**, **credenciales de Firebase** y **token de Unwired Labs**.  
- Evita exponer datos sensibles en logs o control de versiones.

---

## Licencia

Distribuido bajo **MIT**. Consulta el archivo `LICENSE` en el repositorio.
