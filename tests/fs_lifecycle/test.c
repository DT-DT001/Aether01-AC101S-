#include "esp_vfs_fat_lifecycle.h"
#include "freertos/semphr.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

struct test_mutex { pthread_mutex_t mutex; };
static atomic_int created,deleted,inside,entries;
static atomic_bool fail_alloc,force_race;
static pthread_barrier_t creation_barrier;
static bool slot_used;
SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    if(atomic_exchange(&fail_alloc,false))return NULL;
    struct test_mutex *m=calloc(1,sizeof(*m));assert(m);
    pthread_mutexattr_t attr;assert(!pthread_mutexattr_init(&attr));
    assert(!pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_ERRORCHECK));
    assert(!pthread_mutex_init(&m->mutex,&attr));assert(!pthread_mutexattr_destroy(&attr));
    atomic_fetch_add(&created,1);
    if(atomic_load(&force_race))pthread_barrier_wait(&creation_barrier);
    return m;
}
void vSemaphoreDelete(SemaphoreHandle_t m) { assert(!pthread_mutex_destroy(&m->mutex));free(m);atomic_fetch_add(&deleted,1); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t m,TickType_t ticks) {
    if(!ticks)return pthread_mutex_trylock(&m->mutex)==0;
    struct timespec until;assert(!clock_gettime(CLOCK_REALTIME,&until));
    until.tv_sec+=ticks/1000;until.tv_nsec+=(long)(ticks%1000)*1000000;
    if(until.tv_nsec>=1000000000){until.tv_nsec-=1000000000;until.tv_sec++;}
    return pthread_mutex_timedlock(&m->mutex,&until)==0;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t m) { return pthread_mutex_unlock(&m->mutex)==0; }
static void *mount_worker(void *unused) {
    (void)unused;
    for(unsigned n=0;n<200;++n){
        assert(esp_vfs_fat_lifecycle_acquire(200)==ESP_OK);
        assert(atomic_fetch_add(&inside,1)==0);
        /* Same unreserved drive/context selection used by TF and USB: no
         * second lifecycle may inspect the empty slot before publication. */
        assert(!slot_used);usleep(100);slot_used=true;usleep(100);slot_used=false;
        atomic_fetch_add(&entries,1);assert(atomic_fetch_sub(&inside,1)==1);
        esp_vfs_fat_lifecycle_release();
    }
    return NULL;
}
static void *try_busy(void *unused) {
    (void)unused;assert(esp_vfs_fat_lifecycle_acquire(0)==ESP_ERR_TIMEOUT);
    assert(esp_vfs_fat_lifecycle_acquire(20)==ESP_ERR_TIMEOUT);return NULL;
}
int main(void) {
    atomic_store(&fail_alloc,true);assert(esp_vfs_fat_lifecycle_acquire(1)==ESP_ERR_NO_MEM);
    assert(!atomic_load(&entries));
    assert(!pthread_barrier_init(&creation_barrier,NULL,2));atomic_store(&force_race,true);
    pthread_t a,b;assert(!pthread_create(&a,NULL,mount_worker,NULL));assert(!pthread_create(&b,NULL,mount_worker,NULL));
    /* Both candidates have been created once; neither thread needs another. */
    assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));atomic_store(&force_race,false);
    assert(atomic_load(&created)==2&&atomic_load(&deleted)==1&&atomic_load(&entries)==400);
    assert(esp_vfs_fat_lifecycle_acquire(200)==ESP_OK);
    assert(!pthread_create(&a,NULL,try_busy,NULL));assert(!pthread_join(a,NULL));
    esp_vfs_fat_lifecycle_release();assert(esp_vfs_fat_lifecycle_acquire(0)==ESP_OK);esp_vfs_fat_lifecycle_release();
    assert(!pthread_barrier_destroy(&creation_barrier));
    puts("PASS actual shared FAT lifecycle mutex: allocation failure, raced first use,400 concurrent drive/context transactions, immediate/bounded busy refusal and recovery");
}
