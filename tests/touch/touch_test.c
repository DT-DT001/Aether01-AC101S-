#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "../../main/app_touch.c"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %u: %s\n", __LINE__, #c); return 1; } } while (0)

static int64_t now_us;
static unsigned read_calls, get_calls, irq_calls, wake_calls, task_wakes, unlocks, resets;
static esp_err_t read_error, get_error, irq_error;
static bool lock_allowed, create_allowed;
static uint8_t point_count;
static uint32_t source_sequence;
static uint16_t point_x, point_y;
static lv_indev_t fake_indev;
static lv_display_t display;
static struct esp_lcd_touch touch;
static app_power_t power;
static void (*registered_irq)(esp_lcd_touch_handle_t);
static void (*wake_worker)(void *);
static void *wake_worker_context;
static bool task_allowed;
static unsigned notify_count, worker_creates, isr_notifies, isr_yields;
static jmp_buf wake_worker_exit;

BaseType_t xTaskCreate(void (*entry)(void *), const char *name, uint32_t stack,
                       void *context, unsigned priority, TaskHandle_t *out)
{
    (void)name; (void)stack; (void)priority;
    ++worker_creates;
    if (!task_allowed) return pdFALSE;
    wake_worker = entry; wake_worker_context = context; *out = &wake_worker;
    return pdPASS;
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait)
{
    if (clear != pdTRUE || wait != portMAX_DELAY) return 0;
    if (!notify_count) longjmp(wake_worker_exit, 1);
    unsigned count = notify_count; notify_count = 0; return count;
}
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *yield)
{
    if (task == &wake_worker) { ++notify_count; ++isr_notifies; *yield = pdTRUE; }
}
void test_yield_from_isr(void) { ++isr_yields; }
int64_t esp_timer_get_time(void) { return now_us; }
uint32_t cst9220_report_sequence(esp_lcd_touch_handle_t handle)
{ return handle == &touch ? source_sequence : 0; }
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t handle)
{ (void)handle; ++read_calls; return read_error; }
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t handle, esp_lcd_touch_point_data_t *points,
                                 uint8_t *count, uint8_t maximum)
{
    (void)handle; (void)maximum;
    ++get_calls;
    if (get_error != ESP_OK) return get_error;
    points[0].x = point_x;
    points[0].y = point_y;
    *count = point_count;
    return ESP_OK;
}
esp_err_t esp_lcd_touch_register_interrupt_callback_with_data(esp_lcd_touch_handle_t handle,
                              void (*callback)(esp_lcd_touch_handle_t), void *context)
{
    ++irq_calls;
    if (irq_error == ESP_OK) { registered_irq = callback; handle->config.user_data = context; }
    return irq_error;
}
void *lv_indev_get_driver_data(lv_indev_t *indev) { return indev->driver_data; }
lv_display_t *lv_indev_get_display(lv_indev_t *indev) { return indev->display; }
int lv_display_get_horizontal_resolution(lv_display_t *target) { return target->width; }
int lv_display_get_vertical_resolution(lv_display_t *target) { return target->height; }
bool lvgl_port_lock(uint32_t timeout) { (void)timeout; return lock_allowed; }
void lvgl_port_unlock(void) { ++unlocks; }
void lvgl_port_task_wake(int event, void *context)
{ if (event == LVGL_PORT_EVENT_TOUCH && context == &fake_indev) ++task_wakes; }
lv_indev_t *lv_indev_create(void) { return create_allowed ? &fake_indev : NULL; }
void lv_indev_set_type(lv_indev_t *indev, int type) { indev->type = type; }
void lv_indev_enable(lv_indev_t *indev, bool enabled) { indev->enabled = enabled; }
void lv_indev_set_disp(lv_indev_t *indev, lv_display_t *target) { indev->display = target; }
void lv_indev_set_driver_data(lv_indev_t *indev, void *context) { indev->driver_data = context; }
void lv_indev_set_read_cb(lv_indev_t *indev, void (*callback)(lv_indev_t *, lv_indev_data_t *))
{ indev->read_cb = callback; }
void lv_indev_reset(lv_indev_t *indev, void *object)
{ if (indev == &fake_indev && object == NULL) ++resets; }
lv_timer_t *lv_indev_get_read_timer(lv_indev_t *indev) { return &indev->timer; }
void lv_timer_set_period(lv_timer_t *timer, uint32_t period) { timer->period = period; }
bool app_power_is_sleeping(const app_power_t *manager)
{ return manager && (manager->sleeping || manager->sleep_requested); }
esp_err_t app_power_wake(app_power_t *manager)
{ ++wake_calls; manager->wake_requested = true; manager->sleep_requested = false; return ESP_OK; }

static void reset(void)
{
    memset(&s_touch, 0, sizeof(s_touch));
    atomic_store(&s_touch_registered, false);
    memset(&fake_indev, 0, sizeof(fake_indev));
    memset(&power, 0, sizeof(power));
    memset(&touch, 0, sizeof(touch));
    display = (lv_display_t){480, 480};
    now_us = 0;
    read_calls = get_calls = irq_calls = wake_calls = task_wakes = unlocks = resets = 0;
    read_error = get_error = irq_error = ESP_OK;
    lock_allowed = create_allowed = true;
    point_count = 1;
    source_sequence = 1;
    point_x = 100; point_y = 120;
    touch.config.int_gpio_num = 7;
    registered_irq = NULL;
    wake_worker = NULL; wake_worker_context = NULL; task_allowed = true;
    notify_count = worker_creates = isr_notifies = isr_yields = 0;
}

static lv_indev_data_t sample(void)
{
    lv_indev_data_t data = {.state = LV_INDEV_STATE_PRESSED, .point = {22, 33}};
    fake_indev.read_cb(&fake_indev, &data);
    return data;
}

static int test_initialization(void)
{
    reset();
    CHECK(!app_touch_add(NULL, &touch, &power));
    CHECK(!app_touch_add(&display, NULL, &power));
    lock_allowed = false;
    CHECK(!app_touch_add(&display, &touch, &power) && !unlocks);
    lock_allowed = true;
    create_allowed = false;
    CHECK(!app_touch_add(&display, &touch, &power) && unlocks == 1);
    create_allowed = true;
    irq_error = ESP_FAIL;
    CHECK(app_touch_add(&display, &touch, &power) == &fake_indev);
    CHECK(fake_indev.timer.period == 8 && fake_indev.type == LV_INDEV_TYPE_POINTER);
    CHECK(irq_calls == 1 && !registered_irq);
    CHECK(sample().state == LV_INDEV_STATE_PRESSED); /* Polling survives IRQ failure. */
    CHECK(!app_touch_add(&display, &touch, &power)); /* Only one fitted controller. */
    reset();
    CHECK(app_touch_add(&display, &touch, &power));
    CHECK(registered_irq);
    registered_irq(&touch);
    CHECK(task_wakes == 1);
    CHECK(worker_creates == 0 && isr_notifies == 0 && isr_yields == 0);
    reset();
    touch.config.int_gpio_num = GPIO_NUM_NC;
    CHECK(app_touch_add(&display, &touch, &power));
    CHECK(!irq_calls && sample().state == LV_INDEV_STATE_PRESSED);
    puts("PASS initialization, IRQ fallback and polling/IRQ input");
    return 0;
}

static int test_independent_wake(void)
{
    reset();
    CHECK(app_touch_add(&display, &touch, &power));
    CHECK(!wake_worker && worker_creates == 0);
    power.sleeping = true;
    registered_irq(&touch);
    CHECK(wake_calls == 0 && notify_count == 0 && read_calls == 0);
    lock_allowed = false;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && s_touch.consume_gesture);
    registered_irq(&touch);
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && !wake_calls);
    power.sleeping = false; /* Physical key completed the wake. */
    CHECK(sample().state == LV_INDEV_STATE_RELEASED);
    point_count = 0;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && !s_touch.consume_gesture);
    point_count = 1;
    CHECK(sample().state == LV_INDEV_STATE_PRESSED);
    puts("PASS screen-off IRQ/polling never requests wake or creates wake worker; held finger consumed across physical-key wake");
    return 0;
}

static int test_exclusive_input_and_fallback(void)
{
    reset();
    CHECK(app_touch_add(&display, &touch, &power));
    registered_irq(&touch);
    CHECK(wake_calls == 0 && !s_touch.consume_gesture);
    app_touch_set_enabled(&fake_indev, false);
    CHECK(!fake_indev.enabled);
    power.sleeping = true;
    registered_irq(&touch);
    app_touch_consume_wake_gesture();
    CHECK(wake_calls == 0 && !s_touch.consume_gesture);
    app_touch_set_enabled(&fake_indev, true);
    CHECK(fake_indev.enabled);
    power.sleeping = false;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && s_touch.consume_gesture);
    point_count = 0;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && !s_touch.consume_gesture);
    point_count = 1;
    CHECK(sample().state == LV_INDEV_STATE_PRESSED);
    app_touch_consume_wake_gesture();
    CHECK(sample().state == LV_INDEV_STATE_RELEASED);
    point_count = 0; sample();
    point_count = 1; CHECK(sample().state == LV_INDEV_STATE_PRESSED);
    puts("PASS NES exclusivity, restore and explicit-key wake consume held touch without requesting panel wake");
    return 0;
}

static int test_errors(void)
{
    reset();
    CHECK(app_touch_add(&display, &touch, &power));
    CHECK(sample().state == LV_INDEV_STATE_PRESSED);
    read_error = ESP_ERR_TIMEOUT;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && read_calls == 2 && get_calls == 1);
    CHECK(resets == 1); /* Reset prior press rather than synthesize a click on failure. */
    now_us = 249999;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && read_calls == 2);
    now_us = 250000;
    read_error = ESP_OK;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && read_calls == 3 && s_touch.transport_blocked);
    get_error = ESP_FAIL;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && get_calls == 3);
    CHECK(resets == 1); /* No second logical press to cancel. */
    now_us = 499999;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && read_calls == 4);
    now_us = 500000;
    get_error = ESP_OK;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && read_calls == 5 && s_touch.transport_blocked);
    point_count = 0;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && !s_touch.transport_blocked);
    point_count = 1;
    CHECK(sample().state == LV_INDEV_STATE_PRESSED);
    puts("PASS read/get-data failure cancels gesture, backs off 250ms, and waits for release before new input");
    return 0;
}

static int test_wake_gesture(void)
{
    reset();
    CHECK(app_touch_add(&display, &touch, &power));
    power.sleep_requested = true; /* Queued sleep is already wake-only. */
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && wake_calls == 0);
    CHECK(!power.wake_requested && power.sleep_requested && s_touch.consume_gesture);
    power.sleep_requested = false; /* An explicit key wake is applied externally. */
    CHECK(sample().state == LV_INDEV_STATE_RELEASED); /* Still held after becoming awake. */
    point_x = 480;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED);
    CHECK(s_touch.consume_gesture); /* Out-of-bounds is not a physical release. */
    point_x = 100;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED);
    read_error = ESP_FAIL;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && s_touch.consume_gesture);
    now_us += 250000;
    read_error = ESP_OK;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED);
    point_count = 0;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && !s_touch.consume_gesture);
    point_count = 1;
    CHECK(sample().state == LV_INDEV_STATE_PRESSED);
    power.sleeping = true;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && wake_calls == 0);
    power.sleeping = false;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED);
    puts("PASS touch cannot reverse queued/active sleep; gesture consumed until confirmed release");
    return 0;
}

static int test_coordinate_bounds(void)
{
    reset();
    CHECK(app_touch_add(&display, &touch, &power));
    point_x = 479; point_y = 479;
    lv_indev_data_t data = sample();
    CHECK(data.state == LV_INDEV_STATE_PRESSED && data.point.x == 479 && data.point.y == 479);
    point_x = 480;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED);
    CHECK(resets == 1); /* A corrupt coordinate must not click the old position. */
    point_x = 0; point_y = 480;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED);
    CHECK(resets == 1);
    point_x = UINT16_MAX; point_y = 0;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED);
    CHECK(resets == 1);
    point_x = 0;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && s_touch.transport_blocked);
    point_count = 0;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && !s_touch.transport_blocked);
    point_count = 1;
    data = sample();
    CHECK(data.state == LV_INDEV_STATE_PRESSED && data.point.x == 0 && data.point.y == 0);
    puts("PASS coordinate bounds and origin");
    return 0;
}

static int test_stale_report(void)
{
    reset();
    CHECK(app_touch_add(&display, &touch, &power));
    CHECK(sample().state == LV_INDEV_STATE_PRESSED);
    for (unsigned i = 0; i < 128; ++i) {
        now_us += 8000;
        CHECK(sample().state == LV_INDEV_STATE_PRESSED && !resets);
    }
    /* Identical, fresh snapshots keep a stationary hold alive. */
    for (unsigned i = 0; i < 5; ++i) {
        ++source_sequence;
        now_us += 800000;
        CHECK(sample().state == LV_INDEV_STATE_PRESSED && !resets);
    }
    now_us += TOUCH_STALE_US - 1;
    CHECK(sample().state == LV_INDEV_STATE_PRESSED);
    ++now_us;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && resets == 1);
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && resets == 1);
    /* No explicit lift ever made it to the driver. A new real report can
     * restart input, but cached reads after the cancellation cannot. */
    ++source_sequence;
    CHECK(sample().state == LV_INDEV_STATE_PRESSED);
    point_count = 0;
    ++source_sequence;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && resets == 1);

    reset();
    CHECK(app_touch_add(&display, &touch, &power));
    CHECK(sample().state == LV_INDEV_STATE_PRESSED);
    now_us = 16000;
    registered_irq(&touch);
    CHECK(sample().state == LV_INDEV_STATE_PRESSED);
    /* A later real report satisfies the IRQ without canceling a hold. */
    now_us += TOUCH_UNMATCHED_IRQ_US - 1;
    ++source_sequence;
    CHECK(sample().state == LV_INDEV_STATE_PRESSED && !resets);
    registered_irq(&touch);
    CHECK(sample().state == LV_INDEV_STATE_PRESSED);
    now_us += TOUCH_UNMATCHED_IRQ_US;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && resets == 1);
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && resets == 1);
    point_count = 0;
    ++source_sequence;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED);
    puts("PASS fresh stationary hold, lost release with/without IRQ, cached suppression and a new gesture");

    reset();
    CHECK(app_touch_add(&display, &touch, &power));
    CHECK(sample().state == LV_INDEV_STATE_PRESSED);
    point_x += TOUCH_MOTION_DISTANCE;
    ++source_sequence;
    CHECK(sample().state == LV_INDEV_STATE_PRESSED && s_touch.moving);
    registered_irq(&touch);
    CHECK(sample().state == LV_INDEV_STATE_PRESSED);
    now_us += 90000;
    CHECK(sample().state == LV_INDEV_STATE_PRESSED && !resets);
    point_x += 2;
    ++source_sequence;
    CHECK(sample().point.x == point_x && !resets);
    registered_irq(&touch);
    CHECK(sample().state == LV_INDEV_STATE_PRESSED);
    now_us += TOUCH_MOVING_IRQ_US;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED && resets == 1);
    puts("PASS moving gesture survives delayed report and stale cached movement still cancels");
    return 0;
}

static int test_shutdown_input_pause(void)
{
    reset();
    CHECK(!app_touch_set_shutdown_paused(true));
    CHECK(app_touch_add(&display, &touch, &power));
    CHECK(sample().state == LV_INDEV_STATE_PRESSED);
    CHECK(app_touch_set_shutdown_paused(true) && !fake_indev.enabled && resets == 1);
    CHECK(!app_touch_set_shutdown_paused(true) && !fake_indev.enabled && resets == 1);
    (void)app_touch_set_shutdown_paused(false);
    CHECK(fake_indev.enabled && sample().state == LV_INDEV_STATE_RELEASED);
    point_count = 0;
    ++source_sequence;
    CHECK(sample().state == LV_INDEV_STATE_RELEASED);
    point_count = 1;
    ++source_sequence;
    CHECK(sample().state == LV_INDEV_STATE_PRESSED);
    puts("PASS shutdown input pause preserves ownership and consumes held contact during rollback");
    return 0;
}

int main(void)
{
    return test_initialization() || test_errors() || test_wake_gesture() || test_coordinate_bounds() ||
           test_independent_wake() || test_exclusive_input_and_fallback() || test_stale_report() ||
           test_shutdown_input_pause();
}
