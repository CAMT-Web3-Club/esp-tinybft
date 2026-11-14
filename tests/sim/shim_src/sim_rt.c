/*
 * Host simulator runtime: FreeRTOS (tasks, delays, ticks, event groups,
 * semaphores, queues) on pthreads, esp_timer, task watchdog, esp_log,
 * esp_system, esp_random, esp_mac and the task lock ("GIL").
 *
 * See shim/sim_rt.h for the simulator-specific API.
 */
#define _GNU_SOURCE
#include "sim_rt.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_mac.h"
#include "esp_efuse.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

/* ======================================================================== *
 * Process start, environment
 * ======================================================================== */

static int64_t g_boot_mono_ns;     /* CLOCK_MONOTONIC at process start   */
static int64_t g_run_t0_ns;        /* CLOCK_REALTIME of the run start    */
static int     g_node_id = -1;
static char    g_role[16] = "node";
static int     g_incarnation;
static uint64_t g_seed = 1;

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int64_t real_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int sim_rt_env_int(const char *name, int dflt)
{
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    return (int)strtol(v, nullptr, 0);
}

double sim_rt_env_double(const char *name, double dflt)
{
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    return strtod(v, nullptr);
}

static void log_config_init(void);
static void rng_reseed(void);

__attribute__((constructor)) static void sim_rt_init(void)
{
    g_boot_mono_ns = mono_ns();
    const char *t0 = getenv("SIM_T0_NS");
    g_run_t0_ns = (t0 && *t0) ? strtoll(t0, nullptr, 10) : real_ns();
    const char *seed = getenv("SIM_SEED");
    if (seed && *seed) g_seed = strtoull(seed, nullptr, 0);
    g_incarnation = sim_rt_env_int("SIM_INCARNATION", 0);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    log_config_init();
    rng_reseed();
}

int64_t sim_rt_boot_us(void)
{
    return (mono_ns() - g_boot_mono_ns) / 1000;
}

double sim_rt_run_s(void)
{
    return (double)(real_ns() - g_run_t0_ns) / 1e9;
}

int64_t esp_timer_get_time(void)
{
    return sim_rt_boot_us();
}

void sim_rt_set_node(int node_id, const char *role)
{
    g_node_id = node_id;
    if (role) snprintf(g_role, sizeof(g_role), "%s", role);
    rng_reseed();
}

int sim_rt_node_id(void) { return g_node_id; }
int sim_rt_incarnation(void) { return g_incarnation; }

/* ======================================================================== *
 * PRNG: xoshiro256** seeded with splitmix64
 * ======================================================================== */

static pthread_mutex_t g_rng_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_rng[SIM_RNG_COUNT][4];

static uint64_t splitmix64(uint64_t *x)
{
    uint64_t z = (*x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void rng_reseed(void)
{
    pthread_mutex_lock(&g_rng_lock);
    for (int s = 0; s < SIM_RNG_COUNT; s++) {
        uint64_t x = g_seed ^ ((uint64_t)(g_node_id + 1) * 0x100000001b3ULL)
                   ^ ((uint64_t)(g_incarnation + 1) << 40)
                   ^ ((uint64_t)(s + 1) * 0xd6e8feb86659fd93ULL);
        for (int i = 0; i < 4; i++) g_rng[s][i] = splitmix64(&x);
    }
    pthread_mutex_unlock(&g_rng_lock);
}

static inline uint64_t rotl64(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

uint64_t sim_rt_rand64(int stream)
{
    if (stream < 0 || stream >= SIM_RNG_COUNT) stream = 0;
    pthread_mutex_lock(&g_rng_lock);
    uint64_t *s = g_rng[stream];
    uint64_t result = rotl64(s[1] * 5, 7) * 9;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl64(s[3], 45);
    pthread_mutex_unlock(&g_rng_lock);
    return result;
}

double sim_rt_rand_unit(int stream)
{
    return (double)(sim_rt_rand64(stream) >> 11) * 0x1.0p-53;
}

uint32_t esp_random(void)
{
    return (uint32_t)(sim_rt_rand64(SIM_RNG_CRYPTO) >> 32);
}

void esp_fill_random(void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    while (len) {
        uint64_t r = sim_rt_rand64(SIM_RNG_CRYPTO);
        size_t n = len < 8 ? len : 8;
        memcpy(p, &r, n);
        p += n;
        len -= n;
    }
}

/* ======================================================================== *
 * Logging
 * ======================================================================== */

#define SIM_LOG_MAX_TAGS 32
static esp_log_level_t g_log_default = ESP_LOG_WARN;
static struct { char tag[32]; esp_log_level_t level; } g_log_tags[SIM_LOG_MAX_TAGS];
static int  g_log_ntags;
static bool g_log_events = true;
static pthread_mutex_t g_log_cfg_lock = PTHREAD_MUTEX_INITIALIZER;

/* Protocol events: always printed (format-string substrings). */
static const char *const k_event_patterns[] = {
    /* sequence numbers passed over without executing them */
    "skipped by view-change",
    "execute: no pre-prepare",
    "commit digest mismatch",
    "exec_cb rejected",
    "skipping exec",
    "advancing window head",
    /* view changes */
    "view-change timeout",
    "sent view-change",
    "sent new-view",
    "installed new view",
    "installed view",
    "ask for a view change: joining",
    "replicas still work in view",
    "re-sent new-view",
    "is behind (executed",
    "asking for missing messages",
    "waiting for its pre-prepare",
    "waiting for the right one",
    "conflicts with the New_view",
    "conflicting pre-prepare",
    "re-sending the request",
    "re-sending reply",
    "lacks our key",
    "sync view to",
    "status view catch-up",
    "pp: higher view",
    "new primary: seqno reset",
    "new-view: prepared seqno",
    /* state transfer / checkpoints */
    "view catch-up triggered",
    "start_fetch",
    "state fetch completed",
    "fetch timeout",
    "stable ckpt digest mismatch",
    "local state differs from the quorum",
    "state transfer complete",
    "fetching stable checkpoint",
    "does not match its certified digest",
    "is stable at the quorum",
    "marked stable seqno",
    /* keys / auth */
    "key barrier complete",
    "installed HMAC in-key",
    "verify_mac_in: FAILED",
    "signature verification failed",
    "no valid RSA signature",
    /* overload */
    "request queue full",
    "out of window",
    /* lifecycle */
    "replica %d initialised",
    "client %d initialised",
    "recv_reply timeout",
    nullptr,
};

static esp_log_level_t parse_level(const char *s)
{
    if (!s || !*s) return ESP_LOG_WARN;
    switch (s[0]) {
    case 'N': case 'n': return ESP_LOG_NONE;
    case 'E': case 'e': return ESP_LOG_ERROR;
    case 'W': case 'w': return ESP_LOG_WARN;
    case 'I': case 'i': return ESP_LOG_INFO;
    case 'D': case 'd': return ESP_LOG_DEBUG;
    case 'V': case 'v': return ESP_LOG_VERBOSE;
    default: break;
    }
    int v = atoi(s);
    if (v < 0) v = 0;
    if (v > 5) v = 5;
    return (esp_log_level_t)v;
}

void esp_log_level_set(const char *tag, esp_log_level_t level)
{
    pthread_mutex_lock(&g_log_cfg_lock);
    if (!tag || strcmp(tag, "*") == 0) {
        g_log_default = level;
        g_log_ntags = 0;   /* ESP-IDF: "*" resets every tag */
    } else {
        int i;
        for (i = 0; i < g_log_ntags; i++) {
            if (strcmp(g_log_tags[i].tag, tag) == 0) break;
        }
        if (i == g_log_ntags && g_log_ntags < SIM_LOG_MAX_TAGS) {
            snprintf(g_log_tags[i].tag, sizeof(g_log_tags[i].tag), "%s", tag);
            g_log_ntags++;
        }
        if (i < SIM_LOG_MAX_TAGS) g_log_tags[i].level = level;
    }
    pthread_mutex_unlock(&g_log_cfg_lock);
}

static void log_config_init(void)
{
    g_log_events = sim_rt_env_int("SIM_LOG_EVENTS", 1) != 0;
    const char *spec = getenv("SIM_LOG");
    if (!spec || !*spec) return;
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", spec);
    char *save = nullptr;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(nullptr, ",", &save)) {
        char *eq = strchr(tok, '=');
        if (eq) {
            *eq = '\0';
            esp_log_level_set(tok, parse_level(eq + 1));
        } else {
            g_log_default = parse_level(tok);
        }
    }
}

static esp_log_level_t level_for(const char *tag)
{
    for (int i = 0; i < g_log_ntags; i++) {
        if (strcmp(g_log_tags[i].tag, tag) == 0) return g_log_tags[i].level;
    }
    return g_log_default;
}

static signed char classify(const char *fmt)
{
    for (int i = 0; k_event_patterns[i]; i++) {
        if (strstr(fmt, k_event_patterns[i])) return 1;
    }
    return 0;
}

uint32_t esp_log_timestamp(void)
{
    return (uint32_t)(sim_rt_boot_us() / 1000);
}

static void emit_line(char letter, const char *tag, const char *fmt, va_list ap)
{
    char msg[1536];
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(msg)) n = (int)sizeof(msg) - 1;
    while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) msg[--n] = '\0';
    flockfile(stdout);
    fprintf(stdout, "[%9.3f] %c (%u) %s: %s\n", sim_rt_run_s(), letter,
            esp_log_timestamp(), tag, msg);
    funlockfile(stdout);
}

void sim_log_vwrite(signed char *cache, esp_log_level_t level, const char *tag,
                    const char *fmt, va_list ap)
{
    bool event = false;
    if (g_log_events) {
        signed char c = cache ? *cache : -1;
        if (c < 0) {
            c = classify(fmt);
            if (cache) *cache = c;
        }
        event = (c > 0);
    }
    if (!event && level > level_for(tag)) return;
    static const char letters[] = "NEWIDV";
    emit_line(letters[level <= ESP_LOG_VERBOSE ? level : ESP_LOG_VERBOSE], tag, fmt, ap);
}

void sim_log_write(signed char *cache, esp_log_level_t level, const char *tag,
                   const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sim_log_vwrite(cache, level, tag, fmt, ap);
    va_end(ap);
}

void sim_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit_line('S', g_role, fmt, ap);
    va_end(ap);
}

void sim_log_flush(void)
{
    fflush(stdout);
    fflush(stderr);
}

/* ======================================================================== *
 * Task lock (GIL)
 * ======================================================================== */

static pthread_mutex_t g_gil = PTHREAD_MUTEX_INITIALIZER;
static thread_local bool t_gil_held;

void sim_gil_lock(void)
{
    pthread_mutex_lock(&g_gil);
    t_gil_held = true;
}

void sim_gil_unlock(void)
{
    t_gil_held = false;
    pthread_mutex_unlock(&g_gil);
}

bool sim_gil_held(void) { return t_gil_held; }

bool sim_blocking_begin(void)
{
    bool held = t_gil_held;
    if (held) sim_gil_unlock();
    return held;
}

void sim_blocking_end(bool was_held)
{
    if (was_held) sim_gil_lock();
}

/* ======================================================================== *
 * Critical sections (portENTER_CRITICAL)
 * ======================================================================== */

static pthread_mutex_t g_crit;
static pthread_once_t  g_crit_once = PTHREAD_ONCE_INIT;

static void crit_init(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_crit, &a);
    pthread_mutexattr_destroy(&a);
}

void sim_port_enter_critical(portMUX_TYPE *mux)
{
    (void)mux;
    pthread_once(&g_crit_once, crit_init);
    pthread_mutex_lock(&g_crit);
}

void sim_port_exit_critical(portMUX_TYPE *mux)
{
    (void)mux;
    pthread_mutex_unlock(&g_crit);
}

/* ======================================================================== *
 * Condition-variable helpers on CLOCK_MONOTONIC
 * ======================================================================== */

static void cond_init_mono(pthread_cond_t *c)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(c, &a);
    pthread_condattr_destroy(&a);
}

static struct timespec mono_deadline_us(int64_t us_from_now)
{
    int64_t ns = mono_ns() + us_from_now * 1000LL;
    struct timespec ts = { .tv_sec = ns / 1000000000LL, .tv_nsec = ns % 1000000000LL };
    return ts;
}

static int64_t ticks_to_us(TickType_t ticks)
{
    return (int64_t)ticks * 1000000LL / configTICK_RATE_HZ;
}

/* ======================================================================== *
 * Tasks
 * ======================================================================== */

struct sim_task {
    pthread_t       thread;
    TaskFunction_t  fn;
    void           *arg;
    char            name[32];
    /* task watchdog */
    bool            wdt_subscribed;
    int64_t         wdt_last_reset_us;
    struct sim_task *next;
};

static pthread_mutex_t   g_tasks_lock = PTHREAD_MUTEX_INITIALIZER;
static struct sim_task  *g_tasks;
static struct sim_task   g_main_task = { .name = "main" };
static thread_local struct sim_task *t_self;

static struct sim_task *self_task(void)
{
    return t_self ? t_self : &g_main_task;
}

static void *task_trampoline(void *p)
{
    struct sim_task *t = (struct sim_task *)p;
    t_self = t;
    char tn[16];
    snprintf(tn, sizeof(tn), "%.15s", t->name);
    pthread_setname_np(pthread_self(), tn);
    sim_gil_lock();
    t->fn(t->arg);
    sim_gil_unlock();
    /* ESP-IDF's task wrapper: a task function must not return. */
    sim_printf("FreeRTOS Task \"%s\" should not return, Aborting now!", t->name);
    sim_log_flush();
    _exit(SIM_EXIT_TASK_RETURN);
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name,
                                   configSTACK_DEPTH_TYPE stack_depth,
                                   void *arg, UBaseType_t priority,
                                   TaskHandle_t *created, BaseType_t core)
{
    (void)stack_depth; (void)priority; (void)core;
    struct sim_task *t = (struct sim_task *)calloc(1, sizeof(*t));
    if (!t) return pdFAIL;
    t->fn = fn;
    t->arg = arg;
    snprintf(t->name, sizeof(t->name), "%s", name ? name : "task");
    pthread_mutex_lock(&g_tasks_lock);
    t->next = g_tasks;
    g_tasks = t;
    pthread_mutex_unlock(&g_tasks_lock);
    if (pthread_create(&t->thread, nullptr, task_trampoline, t) != 0) {
        return pdFAIL;
    }
    pthread_detach(t->thread);
    if (created) *created = t;
    return pdPASS;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name,
                       configSTACK_DEPTH_TYPE stack_depth, void *arg,
                       UBaseType_t priority, TaskHandle_t *created)
{
    return xTaskCreatePinnedToCore(fn, name, stack_depth, arg, priority,
                                   created, 0);
}

void vTaskDelete(TaskHandle_t task)
{
    if (task == nullptr || task == t_self) {
        if (t_gil_held) sim_gil_unlock();
        pthread_exit(nullptr);
    }
    /* Deleting another task is not supported by the simulator. */
}

TaskHandle_t xTaskGetCurrentTaskHandle(void) { return self_task(); }

const char *pcTaskGetName(TaskHandle_t task)
{
    return task ? task->name : self_task()->name;
}

void sim_task_yield(void)
{
    bool h = sim_blocking_begin();
    sched_yield();
    sim_blocking_end(h);
}

void vTaskDelay(TickType_t ticks)
{
    bool h = sim_blocking_begin();
    if (ticks == 0) {
        sched_yield();
    } else {
        int64_t us = ticks_to_us(ticks);
        struct timespec ts = { .tv_sec = us / 1000000, .tv_nsec = (us % 1000000) * 1000 };
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
        }
    }
    sim_blocking_end(h);
}

TickType_t xTaskGetTickCount(void)
{
    return (TickType_t)((uint64_t)sim_rt_boot_us() * configTICK_RATE_HZ / 1000000ULL);
}

void sim_rt_busy_ms(int ms)
{
    if (ms <= 0) return;
    int64_t end = mono_ns() + (int64_t)ms * 1000000LL;
    while (mono_ns() < end) {
    }
}

/* ======================================================================== *
 * Event groups
 * ======================================================================== */

struct sim_event_group {
    pthread_mutex_t m;
    pthread_cond_t  c;
    EventBits_t     bits;
};

EventGroupHandle_t xEventGroupCreate(void)
{
    struct sim_event_group *g = (struct sim_event_group *)calloc(1, sizeof(*g));
    if (!g) return nullptr;
    pthread_mutex_init(&g->m, nullptr);
    cond_init_mono(&g->c);
    return g;
}

EventGroupHandle_t xEventGroupCreateStatic(StaticEventGroup_t *buffer)
{
    (void)buffer;
    return xEventGroupCreate();
}

void vEventGroupDelete(EventGroupHandle_t g)
{
    if (!g) return;
    /* Intentionally leaked: a timer callback may still hold the pointer. */
}

EventBits_t xEventGroupSetBits(EventGroupHandle_t g, const EventBits_t bits)
{
    pthread_mutex_lock(&g->m);
    g->bits |= bits;
    EventBits_t v = g->bits;
    pthread_cond_broadcast(&g->c);
    pthread_mutex_unlock(&g->m);
    return v;
}

EventBits_t xEventGroupClearBits(EventGroupHandle_t g, const EventBits_t bits)
{
    pthread_mutex_lock(&g->m);
    EventBits_t v = g->bits;
    g->bits &= ~bits;
    pthread_mutex_unlock(&g->m);
    return v;
}

EventBits_t xEventGroupGetBits(EventGroupHandle_t g)
{
    pthread_mutex_lock(&g->m);
    EventBits_t v = g->bits;
    pthread_mutex_unlock(&g->m);
    return v;
}

EventBits_t xEventGroupWaitBits(EventGroupHandle_t g,
                                const EventBits_t wait_bits,
                                const BaseType_t clear_on_exit,
                                const BaseType_t wait_all,
                                TickType_t ticks)
{
    bool h = (ticks != 0) ? sim_blocking_begin() : false;
    struct timespec dl = mono_deadline_us(ticks == portMAX_DELAY ? 0 : ticks_to_us(ticks));
    pthread_mutex_lock(&g->m);
    EventBits_t ret;
    for (;;) {
        bool ok = wait_all ? ((g->bits & wait_bits) == wait_bits)
                           : ((g->bits & wait_bits) != 0);
        if (ok) {
            ret = g->bits;
            if (clear_on_exit) g->bits &= ~wait_bits;
            break;
        }
        if (ticks == 0) {
            ret = g->bits;
            break;
        }
        int rc = (ticks == portMAX_DELAY)
               ? pthread_cond_wait(&g->c, &g->m)
               : pthread_cond_timedwait(&g->c, &g->m, &dl);
        if (rc == ETIMEDOUT) {
            ret = g->bits;
            break;
        }
    }
    pthread_mutex_unlock(&g->m);
    sim_blocking_end(h);
    return ret;
}

/* ======================================================================== *
 * Semaphores
 * ======================================================================== */

struct sim_sem {
    pthread_mutex_t m;
    pthread_cond_t  c;
    unsigned        count;
    unsigned        max;
};

SemaphoreHandle_t sim_sem_create(unsigned max_count, unsigned initial)
{
    struct sim_sem *s = (struct sim_sem *)calloc(1, sizeof(*s));
    if (!s) return nullptr;
    pthread_mutex_init(&s->m, nullptr);
    cond_init_mono(&s->c);
    s->max = max_count ? max_count : 1;
    s->count = initial > s->max ? s->max : initial;
    return s;
}

void vSemaphoreDelete(SemaphoreHandle_t s)
{
    (void)s;   /* leaked on purpose (see vEventGroupDelete) */
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    bool h = (ticks != 0) ? sim_blocking_begin() : false;
    struct timespec dl = mono_deadline_us(ticks == portMAX_DELAY ? 0 : ticks_to_us(ticks));
    BaseType_t ret = pdFALSE;
    pthread_mutex_lock(&s->m);
    for (;;) {
        if (s->count > 0) {
            s->count--;
            ret = pdTRUE;
            break;
        }
        if (ticks == 0) break;
        int rc = (ticks == portMAX_DELAY)
               ? pthread_cond_wait(&s->c, &s->m)
               : pthread_cond_timedwait(&s->c, &s->m, &dl);
        if (rc == ETIMEDOUT && s->count == 0) break;
    }
    pthread_mutex_unlock(&s->m);
    sim_blocking_end(h);
    return ret;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    BaseType_t ret = pdFALSE;
    pthread_mutex_lock(&s->m);
    if (s->count < s->max) {
        s->count++;
        ret = pdTRUE;
        pthread_cond_signal(&s->c);
    }
    pthread_mutex_unlock(&s->m);
    return ret;
}

UBaseType_t uxSemaphoreGetCount(SemaphoreHandle_t s)
{
    pthread_mutex_lock(&s->m);
    UBaseType_t v = s->count;
    pthread_mutex_unlock(&s->m);
    return v;
}

/* ======================================================================== *
 * Queues
 * ======================================================================== */

struct sim_queue {
    pthread_mutex_t m;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    uint8_t        *items;
    UBaseType_t     len, size, head, count;
};

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    struct sim_queue *q = (struct sim_queue *)calloc(1, sizeof(*q));
    if (!q) return nullptr;
    q->items = (uint8_t *)calloc(length ? length : 1, item_size ? item_size : 1);
    if (!q->items) { free(q); return nullptr; }
    pthread_mutex_init(&q->m, nullptr);
    cond_init_mono(&q->not_empty);
    cond_init_mono(&q->not_full);
    q->len = length;
    q->size = item_size;
    return q;
}

void vQueueDelete(QueueHandle_t q) { (void)q; }

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
    bool h = (ticks != 0) ? sim_blocking_begin() : false;
    struct timespec dl = mono_deadline_us(ticks == portMAX_DELAY ? 0 : ticks_to_us(ticks));
    BaseType_t ret = pdFALSE;
    pthread_mutex_lock(&q->m);
    for (;;) {
        if (q->count < q->len) {
            UBaseType_t tail = (q->head + q->count) % q->len;
            memcpy(q->items + (size_t)tail * q->size, item, q->size);
            q->count++;
            pthread_cond_signal(&q->not_empty);
            ret = pdTRUE;
            break;
        }
        if (ticks == 0) break;
        int rc = (ticks == portMAX_DELAY)
               ? pthread_cond_wait(&q->not_full, &q->m)
               : pthread_cond_timedwait(&q->not_full, &q->m, &dl);
        if (rc == ETIMEDOUT && q->count >= q->len) break;
    }
    pthread_mutex_unlock(&q->m);
    sim_blocking_end(h);
    return ret;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks)
{
    bool h = (ticks != 0) ? sim_blocking_begin() : false;
    struct timespec dl = mono_deadline_us(ticks == portMAX_DELAY ? 0 : ticks_to_us(ticks));
    BaseType_t ret = pdFALSE;
    pthread_mutex_lock(&q->m);
    for (;;) {
        if (q->count > 0) {
            memcpy(item, q->items + (size_t)q->head * q->size, q->size);
            q->head = (q->head + 1) % q->len;
            q->count--;
            pthread_cond_signal(&q->not_full);
            ret = pdTRUE;
            break;
        }
        if (ticks == 0) break;
        int rc = (ticks == portMAX_DELAY)
               ? pthread_cond_wait(&q->not_empty, &q->m)
               : pthread_cond_timedwait(&q->not_empty, &q->m, &dl);
        if (rc == ETIMEDOUT && q->count == 0) break;
    }
    pthread_mutex_unlock(&q->m);
    sim_blocking_end(h);
    return ret;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q)
{
    pthread_mutex_lock(&q->m);
    UBaseType_t v = q->count;
    pthread_mutex_unlock(&q->m);
    return v;
}

BaseType_t xQueueReset(QueueHandle_t q)
{
    pthread_mutex_lock(&q->m);
    q->head = q->count = 0;
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->m);
    return pdPASS;
}

/* ======================================================================== *
 * esp_timer
 * ======================================================================== */

struct esp_timer {
    esp_timer_cb_t  cb;
    void           *arg;
    char            name[32];
    bool            armed;
    bool            periodic;
    bool            deleted;
    bool            skip_unhandled;
    uint64_t        period_us;
    int64_t         alarm_us;
    struct esp_timer *next;   /* registry of every timer ever created */
};

static pthread_mutex_t   g_tmr_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    g_tmr_cond;
static pthread_once_t    g_tmr_once = PTHREAD_ONCE_INIT;
static struct esp_timer *g_timers;

static void *timer_thread(void *unused)
{
    (void)unused;
    pthread_setname_np(pthread_self(), "esp_timer");
    pthread_mutex_lock(&g_tmr_lock);
    for (;;) {
        struct esp_timer *next = nullptr;
        for (struct esp_timer *t = g_timers; t; t = t->next) {
            if (t->armed && !t->deleted && (!next || t->alarm_us < next->alarm_us)) {
                next = t;
            }
        }
        if (!next) {
            pthread_cond_wait(&g_tmr_cond, &g_tmr_lock);
            continue;
        }
        int64_t now = sim_rt_boot_us();
        if (next->alarm_us > now) {
            struct timespec dl = mono_deadline_us(next->alarm_us - now);
            pthread_cond_timedwait(&g_tmr_cond, &g_tmr_lock, &dl);
            continue;
        }
        if (next->periodic) {
            next->alarm_us += (int64_t)next->period_us;
            if (next->skip_unhandled && next->alarm_us <= now) {
                next->alarm_us = now + (int64_t)next->period_us;
            }
        } else {
            next->armed = false;
        }
        esp_timer_cb_t cb = next->cb;
        void *arg = next->arg;
        pthread_mutex_unlock(&g_tmr_lock);
        if (cb) cb(arg);
        pthread_mutex_lock(&g_tmr_lock);
    }
    return nullptr;
}

static void timer_start_thread(void)
{
    cond_init_mono(&g_tmr_cond);
    pthread_t th;
    pthread_create(&th, nullptr, timer_thread, nullptr);
    pthread_detach(th);
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args,
                           esp_timer_handle_t *out)
{
    if (!args || !args->callback || !out) return ESP_ERR_INVALID_ARG;
    pthread_once(&g_tmr_once, timer_start_thread);
    struct esp_timer *t = (struct esp_timer *)calloc(1, sizeof(*t));
    if (!t) return ESP_ERR_NO_MEM;
    t->cb = args->callback;
    t->arg = args->arg;
    t->skip_unhandled = args->skip_unhandled_events;
    snprintf(t->name, sizeof(t->name), "%s", args->name ? args->name : "");
    pthread_mutex_lock(&g_tmr_lock);
    t->next = g_timers;
    g_timers = t;
    pthread_mutex_unlock(&g_tmr_lock);
    *out = t;
    return ESP_OK;
}

static esp_err_t timer_arm(esp_timer_handle_t t, uint64_t us, bool periodic, bool restart)
{
    if (!t) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&g_tmr_lock);
    esp_err_t rc = ESP_OK;
    if (t->deleted) {
        rc = ESP_ERR_INVALID_ARG;
    } else if (t->armed && !restart) {
        rc = ESP_ERR_INVALID_STATE;     /* same as ESP-IDF */
    } else if (restart && !t->armed) {
        rc = ESP_ERR_INVALID_STATE;
    } else {
        if (restart) periodic = t->periodic;
        t->armed = true;
        t->periodic = periodic;
        t->period_us = us;
        t->alarm_us = sim_rt_boot_us() + (int64_t)us;
        pthread_cond_broadcast(&g_tmr_cond);
    }
    pthread_mutex_unlock(&g_tmr_lock);
    return rc;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t timeout_us)
{
    return timer_arm(t, timeout_us, false, false);
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t period)
{
    return timer_arm(t, period, true, false);
}

esp_err_t esp_timer_restart(esp_timer_handle_t t, uint64_t timeout_us)
{
    return timer_arm(t, timeout_us, false, true);
}

esp_err_t esp_timer_stop(esp_timer_handle_t t)
{
    if (!t) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&g_tmr_lock);
    esp_err_t rc = t->armed ? ESP_OK : ESP_ERR_INVALID_STATE;
    t->armed = false;
    pthread_cond_broadcast(&g_tmr_cond);
    pthread_mutex_unlock(&g_tmr_lock);
    return rc;
}

esp_err_t esp_timer_delete(esp_timer_handle_t t)
{
    if (!t) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&g_tmr_lock);
    esp_err_t rc = ESP_OK;
    if (t->armed) {
        rc = ESP_ERR_INVALID_STATE;
    } else {
        t->deleted = true;   /* memory kept: a dispatch may be in flight */
    }
    pthread_mutex_unlock(&g_tmr_lock);
    return rc;
}

bool esp_timer_is_active(esp_timer_handle_t t)
{
    if (!t) return false;
    pthread_mutex_lock(&g_tmr_lock);
    bool a = t->armed;
    pthread_mutex_unlock(&g_tmr_lock);
    return a;
}

void *sim_esp_timer_find_arg(const char *name)
{
    void *arg = nullptr;
    pthread_mutex_lock(&g_tmr_lock);
    for (struct esp_timer *t = g_timers; t; t = t->next) {   /* newest first */
        if (!t->deleted && strcmp(t->name, name) == 0) {
            arg = t->arg;
            break;
        }
    }
    pthread_mutex_unlock(&g_tmr_lock);
    return arg;
}

/* ======================================================================== *
 * Task watchdog
 * ======================================================================== */

static esp_timer_handle_t g_wdt_timer;
static pthread_mutex_t    g_wdt_lock = PTHREAD_MUTEX_INITIALIZER;

static void wdt_check(void *unused)
{
    (void)unused;
    static const char TAG[] = "task_wdt";
#if CONFIG_ESP_TASK_WDT_EN
    const int64_t timeout_us = (int64_t)CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000000LL;
#else
    const int64_t timeout_us = 10000000LL;
#endif
    int64_t now = sim_rt_boot_us();
    pthread_mutex_lock(&g_tasks_lock);
    pthread_mutex_lock(&g_wdt_lock);
    struct sim_task *list[2] = { g_tasks, &g_main_task };
    for (int l = 0; l < 2; l++) {
        for (struct sim_task *t = list[l]; t; t = (l == 0 ? t->next : nullptr)) {
            if (!t->wdt_subscribed) continue;
            if (now - t->wdt_last_reset_us > timeout_us) {
                ESP_LOGE(TAG, "Task watchdog got triggered. The following tasks/users "
                         "did not reset the watchdog in time:");
                ESP_LOGE(TAG, " - %s (CPU 0)", t->name);
                sim_printf("SIM_WDT task=%s stalled_ms=%lld", t->name,
                           (long long)((now - t->wdt_last_reset_us) / 1000));
                t->wdt_last_reset_us = now;   /* report once per period */
            }
        }
    }
    pthread_mutex_unlock(&g_wdt_lock);
    pthread_mutex_unlock(&g_tasks_lock);
}

esp_err_t esp_task_wdt_add(TaskHandle_t task)
{
    struct sim_task *t = task ? task : self_task();
    pthread_mutex_lock(&g_wdt_lock);
    if (!g_wdt_timer) {
        esp_timer_create_args_t a = {
            .callback = wdt_check, .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK, .name = "sim_task_wdt",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&a, &g_wdt_timer) == ESP_OK) {
            esp_timer_start_periodic(g_wdt_timer, 1000000);
        }
    }
    esp_err_t rc = t->wdt_subscribed ? ESP_ERR_INVALID_ARG : ESP_OK;
    t->wdt_subscribed = true;
    t->wdt_last_reset_us = sim_rt_boot_us();
    pthread_mutex_unlock(&g_wdt_lock);
    return rc;
}

esp_err_t esp_task_wdt_reset(void)
{
    struct sim_task *t = self_task();
    pthread_mutex_lock(&g_wdt_lock);
    bool sub = t->wdt_subscribed;
    if (sub) t->wdt_last_reset_us = sim_rt_boot_us();
    pthread_mutex_unlock(&g_wdt_lock);
    if (!sub) {
        /* ESP-IDF logs this through ESP_GOTO_ON_FALSE(..., "task not found") */
        ESP_LOGE("task_wdt", "esp_task_wdt_reset(): task not found");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t esp_task_wdt_delete(TaskHandle_t task)
{
    struct sim_task *t = task ? task : self_task();
    pthread_mutex_lock(&g_wdt_lock);
    esp_err_t rc = t->wdt_subscribed ? ESP_OK : ESP_ERR_NOT_FOUND;
    t->wdt_subscribed = false;
    pthread_mutex_unlock(&g_wdt_lock);
    return rc;
}

/* ======================================================================== *
 * esp_system / esp_err / esp_mac
 * ======================================================================== */

void esp_restart(void)
{
    sim_printf("SIM_RESTART reason=esp_restart");
    sim_log_flush();
    _exit(SIM_EXIT_RESTART);
}

void sim_error_check_failed(esp_err_t rc, const char *file, int line,
                            const char *expr)
{
    sim_printf("ESP_ERROR_CHECK failed: esp_err_t 0x%x (%s) at %s:%d (%s)",
               (unsigned)rc, esp_err_to_name(rc), file, line, expr);
    sim_log_flush();
    _exit(SIM_EXIT_ERROR_CHECK);
}

static const uint32_t SIM_HEAP_TOTAL = 200000;
static uint32_t g_heap_min = SIM_HEAP_TOTAL;

uint32_t esp_get_free_heap_size(void)
{
    struct mallinfo2 mi = mallinfo2();
    uint32_t used = (uint32_t)(mi.uordblks > SIM_HEAP_TOTAL ? SIM_HEAP_TOTAL : mi.uordblks);
    uint32_t free_b = SIM_HEAP_TOTAL - used;
    if (free_b < g_heap_min) g_heap_min = free_b;
    return free_b;
}

uint32_t esp_get_minimum_free_heap_size(void)
{
    (void)esp_get_free_heap_size();
    return g_heap_min;
}

esp_reset_reason_t esp_reset_reason(void)
{
    return g_incarnation > 0 ? ESP_RST_SW : ESP_RST_POWERON;
}

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK:                return "ESP_OK";
    case ESP_FAIL:              return "ESP_FAIL";
    case ESP_ERR_NO_MEM:        return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:   return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:  return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:     return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT:       return "ESP_ERR_TIMEOUT";
    default:                    return "UNKNOWN ERROR";
    }
}

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    if (!mac) return ESP_ERR_INVALID_ARG;
    uint8_t base[6] = { 0x02, 0x00, 0x00, 0x00, 0x00,
                        (uint8_t)(g_node_id < 0 ? 0xff : g_node_id) };
    memcpy(mac, base, 6);
    if (type == ESP_MAC_WIFI_SOFTAP) mac[5] += 1;
    return ESP_OK;
}

esp_err_t esp_base_mac_addr_get(uint8_t *mac)
{
    return esp_read_mac(mac, ESP_MAC_BASE);
}

esp_err_t esp_efuse_mac_get_default(uint8_t *mac)
{
    return esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
}
