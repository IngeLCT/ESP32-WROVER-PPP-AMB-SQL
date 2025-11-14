#pragma once
#include "esp_err.h"
#include "esp_modem_api.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int tx_io, rx_io, rts_io, cts_io, dtr_io, rst_io, pwrkey_io, board_power_io;
    bool rst_active_low;
    int  rst_pulse_ms;
    const char *apn;       // ej: "internet.itelcel.com"
    const char *sim_pin;   // opcional
    bool use_cmux;         // true para CMUX (déjalo en false a menos que lo necesites)
} modem_ppp_config_t;

/** Info de UE/celda extraída de +CPSI (LTE) */
typedef struct {
    int      mcc;      // 100..999
    int      mnc;      // 0..999
    uint32_t tac;      // TAC (acepta "0x.." → entero)
    uint32_t cell_id;  // E-UTRAN Cell ID (ECI, 28 bits)
    bool     valid;
} modem_ue_info_t;

/** Arranca PPP y BLOQUEA hasta obtener IP (o timeout_ms) */
esp_err_t modem_ppp_start_blocking(const modem_ppp_config_t *cfg,
                                   int timeout_ms,
                                   esp_modem_dce_t **out_dce);

/** Última UE info válida (+CPSI) */
bool modem_get_ue_info(modem_ue_info_t *out);

/** Geolocaliza con UnwiredLabs usando los valores de +CPSI ya parseados.
 *  Escribe city/state (si existen en la respuesta) y devuelve ESP_OK/ESP_FAIL/errores HTTP.
 *  Requiere: PPP activo (conectividad) y UNWIREDLABS_TOKEN definido (Privado.h).
 */
esp_err_t modem_unwiredlabs_city_state(char *city, size_t city_len,
                                       char *state, size_t state_len);

#ifdef __cplusplus
}
#endif
