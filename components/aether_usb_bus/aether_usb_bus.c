#include "aether_usb_bus.h"
#include <stdatomic.h>

static atomic_uint s_owner;

esp_err_t aether_usb_bus_claim(aether_usb_bus_owner_t owner)
{
    if (owner != AETHER_USB_BUS_MSC && owner != AETHER_USB_BUS_HOST)
        return ESP_ERR_INVALID_ARG;
    unsigned expected = AETHER_USB_BUS_NONE;
    return atomic_compare_exchange_strong(&s_owner, &expected, (unsigned)owner) ?
        ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t aether_usb_bus_release(aether_usb_bus_owner_t owner)
{
    if (owner != AETHER_USB_BUS_MSC && owner != AETHER_USB_BUS_HOST)
        return ESP_ERR_INVALID_ARG;
    unsigned expected = (unsigned)owner;
    return atomic_compare_exchange_strong(&s_owner, &expected, AETHER_USB_BUS_NONE) ?
        ESP_OK : ESP_ERR_INVALID_STATE;
}

aether_usb_bus_owner_t aether_usb_bus_get_owner(void)
{
    return (aether_usb_bus_owner_t)atomic_load(&s_owner);
}
