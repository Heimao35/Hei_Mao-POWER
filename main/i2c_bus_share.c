/**
 * @file i2c_bus_share.c
 */
#include "i2c_bus_share.h"

#include "freertos/semphr.h"

static SemaphoreHandle_t s_mu;

void i2c_bus_share_init(void)
{
    if (s_mu) {
        return;
    }
    s_mu = xSemaphoreCreateMutex();
}

bool i2c_bus_share_lock(TickType_t timeout_ticks)
{
    if (!s_mu) {
        return false;
    }
    return xSemaphoreTake(s_mu, timeout_ticks) == pdTRUE;
}

void i2c_bus_share_unlock(void)
{
    if (s_mu) {
        xSemaphoreGive(s_mu);
    }
}
