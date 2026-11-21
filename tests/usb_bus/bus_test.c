#include "aether_usb_bus.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
static void *claim(void *context) {
    const aether_usb_bus_owner_t owner = *(aether_usb_bus_owner_t *)context;
    return aether_usb_bus_claim(owner) == ESP_OK ? (void *)1 : NULL;
}
int main(void) {
    assert(aether_usb_bus_get_owner()==AETHER_USB_BUS_NONE);
    assert(aether_usb_bus_claim(AETHER_USB_BUS_NONE)==ESP_ERR_INVALID_ARG);
    assert(aether_usb_bus_release(AETHER_USB_BUS_NONE)==ESP_ERR_INVALID_ARG);
    for(unsigned i=0;i<500;++i) {
        aether_usb_bus_owner_t a=AETHER_USB_BUS_MSC,b=AETHER_USB_BUS_HOST;
        pthread_t x,y;void *rx,*ry;
        assert(!pthread_create(&x,NULL,claim,&a));assert(!pthread_create(&y,NULL,claim,&b));
        assert(!pthread_join(x,&rx));assert(!pthread_join(y,&ry));assert((rx!=NULL)+(ry!=NULL)==1);
        const aether_usb_bus_owner_t winner=rx ? a : b,loser=rx ? b : a;
        assert(aether_usb_bus_get_owner()==winner);
        assert(aether_usb_bus_claim(loser)==ESP_ERR_INVALID_STATE);
        assert(aether_usb_bus_release(loser)==ESP_ERR_INVALID_STATE);
        assert(aether_usb_bus_get_owner()==winner);
        assert(aether_usb_bus_release(winner)==ESP_OK);
        assert(aether_usb_bus_get_owner()==AETHER_USB_BUS_NONE);
    }
    puts("PASS 500 simultaneous MSC/Host reservations have exactly one owner; wrong-owner release never frees the physical port");
    return 0;
}
