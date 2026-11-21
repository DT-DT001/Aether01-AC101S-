#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct app_ui {
 int media_control_lock;uint32_t page_generation;
 bool user_key_exit_armed,user_key_exit_pending;
 uint32_t user_key_target_generation,user_key_requested_generation;
 int64_t user_key_armed_at_us;
 bool blocked;unsigned homes;
} app_ui_t;
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
static int64_t fake_now=10;
static int64_t esp_timer_get_time(void){return fake_now;}
static bool app_ui_input_blocked(const app_ui_t *ui){return ui->blocked;}
static void app_ui_show_home(app_ui_t *ui){++ui->homes;++ui->page_generation;}
#include "user_key_selected.inc"
int main(void){
 app_ui_t u={.page_generation=7};
 app_ui_queue_user_key(&u,fake_now);service_user_key(&u);assert(!u.homes);
 app_ui_set_user_key_exit(&u,true);app_ui_queue_user_key(&u,fake_now);app_ui_queue_user_key(&u,fake_now);
 assert(u.user_key_exit_pending&&!u.homes);service_user_key(&u);assert(u.homes==1);
 service_user_key(&u);assert(u.homes==1);
 app_ui_set_user_key_exit(&u,true);app_ui_queue_user_key(&u,fake_now);++u.page_generation;
 service_user_key(&u);assert(u.homes==1);
 app_ui_set_user_key_exit(&u,true);u.blocked=true;app_ui_queue_user_key(&u,fake_now);
 assert(!u.user_key_exit_pending);u.blocked=false;
 app_ui_queue_user_key(&u,fake_now);u.blocked=true;service_user_key(&u);assert(u.homes==1);
 u.blocked=false;app_ui_set_user_key_exit(&u,true);app_ui_queue_user_key(&u,fake_now);
 app_ui_set_user_key_exit(&u,false);service_user_key(&u);assert(u.homes==1);
 fake_now=1000;app_ui_set_user_key_exit(&u,true);
 app_ui_queue_user_key(&u,999);service_user_key(&u);assert(u.homes==1&&u.user_key_exit_armed);
 app_ui_queue_user_key(&u,1000);service_user_key(&u);assert(u.homes==2);
 app_ui_queue_user_key(NULL,fake_now);app_ui_set_user_key_exit(NULL,true);
 puts("PASS actual UI user-key publication/navigation: one-shot/loading exit, page epoch rejects stale events, shutdown gating and cancellation");
}
