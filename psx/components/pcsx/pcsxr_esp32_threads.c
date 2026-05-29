#include "pcsxr_esp32_threads.h"
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

int pcsxr_sthread_core_count = 2;

struct sthread {
    TaskHandle_t handle;
};

struct slock {
    SemaphoreHandle_t mutex;
};

struct scond {
    SemaphoreHandle_t sem;
    volatile int waiters;
};

void pcsxr_sthread_init(void)
{
}

static void *thread_entry(void *arg)
{
    void (*func)(void *) = arg;
    func(NULL);
    vTaskDelete(NULL);
    return NULL;
}

static int thread_type_to_core(enum pcsxr_thread_type type)
{
    (void)type;
    return 1;
}

sthread_t *sthread_create(void (*thread_func)(void *), void *userdata)
{
    (void)userdata;
    struct sthread *t = heap_caps_calloc(1, sizeof(*t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!t) return NULL;

    if (xTaskCreatePinnedToCore(thread_entry, "pcsxr", 16384,
            (void *)thread_func, tskIDLE_PRIORITY + 2, &t->handle, 1) != pdPASS) {
        heap_caps_free(t);
        return NULL;
    }
    return t;
}

sthread_t *pcsxr_sthread_create(void (*thread_func)(void *),
    enum pcsxr_thread_type type)
{
    struct sthread *t = heap_caps_calloc(1, sizeof(*t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!t) return NULL;

    if (xTaskCreatePinnedToCore(thread_entry, "pcsxr", 16384,
            (void *)thread_func, tskIDLE_PRIORITY + 2, &t->handle,
            thread_type_to_core(type)) != pdPASS) {
        heap_caps_free(t);
        return NULL;
    }
    return t;
}

void sthread_join(sthread_t *thread)
{
    if (thread && thread->handle) {
        while (eTaskGetState(thread->handle) != eDeleted)
            vTaskDelay(1);
    }
}

void sthread_detach(sthread_t *thread)
{
    (void)thread;
}

int sthread_detach_(sthread_t *thread)
{
    (void)thread;
    return 0;
}

slock_t *slock_new(void)
{
    struct slock *lock = heap_caps_calloc(1, sizeof(*lock), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!lock) return NULL;
    lock->mutex = xSemaphoreCreateRecursiveMutex();
    if (!lock->mutex) {
        heap_caps_free(lock);
        return NULL;
    }
    return (slock_t *)lock;
}

void slock_free(slock_t *lock)
{
    struct slock *l = (struct slock *)lock;
    if (l) {
        if (l->mutex) vSemaphoreDelete(l->mutex);
        heap_caps_free(l);
    }
}

void slock_lock(slock_t *lock)
{
    struct slock *l = (struct slock *)lock;
    if (l && l->mutex)
        xSemaphoreTakeRecursive(l->mutex, portMAX_DELAY);
}

bool slock_try_lock(slock_t *lock)
{
    struct slock *l = (struct slock *)lock;
    if (!l || !l->mutex) return false;
    return xSemaphoreTakeRecursive(l->mutex, 0) == pdTRUE;
}

void slock_unlock(slock_t *lock)
{
    struct slock *l = (struct slock *)lock;
    if (l && l->mutex)
        xSemaphoreGiveRecursive(l->mutex);
}

scond_t *scond_new(void)
{
    struct scond *cond = heap_caps_calloc(1, sizeof(*cond), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!cond) return NULL;
    cond->sem = xSemaphoreCreateBinary();
    if (!cond->sem) {
        heap_caps_free(cond);
        return NULL;
    }
    return (scond_t *)cond;
}

void scond_free(scond_t *cond)
{
    struct scond *c = (struct scond *)cond;
    if (c) {
        if (c->sem) vSemaphoreDelete(c->sem);
        heap_caps_free(c);
    }
}

void scond_wait(scond_t *cond, slock_t *lock)
{
    struct scond *c = (struct scond *)cond;
    if (!c) return;
    c->waiters++;
    slock_unlock(lock);
    xSemaphoreTake(c->sem, portMAX_DELAY);
    slock_lock(lock);
    c->waiters--;
}

void scond_signal(scond_t *cond)
{
    struct scond *c = (struct scond *)cond;
    if (c && c->waiters > 0)
        xSemaphoreGive(c->sem);
}
