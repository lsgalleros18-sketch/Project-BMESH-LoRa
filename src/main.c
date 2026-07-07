#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/param.h>

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/spi_master.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"
#include "bems_common.h"
#include "bems_crypto.h"
#include "mesh_protocol.h"
#include "node_config.h"
#include "lora_radio.h"

#define AP_CHANNEL 6
#define AP_MAX_CONNECTIONS 4
#define DNS_PORT 53
#define HTTP_PORT 80
#define MAX_MESSAGES 16
#define PAYLOAD_LEN 140
#define PACKET_LEN 320
#define BOOT_BUTTON_GPIO 0
#define RGB_LED_GPIO 48
#define FACTORY_RESET_HOLD_MS 10000
#define RESET_WARNING_MS 5000
#define DUPLICATE_NODE_ID_WARNING_MS 60000
#define CONFIG_NAMESPACE "bems_config"
#define PACKET_COUNTER_KEY "packet_ctr"
#define HIGHEST_SEEN_ID_KEY "highest_seen"
#define SESSION_COOKIE_NAME "BMESH_SESSION"
#define SESSION_TOKEN_LEN 17

#define LORA_MISO_GPIO 5
#define LORA_DIO0_GPIO 16
#define LORA_SCK_GPIO 7
#define LORA_MOSI_GPIO 6
#define LORA_RST_GPIO 4
#define LORA_NSS_GPIO 8
#define LORA_SPI_HOST SPI2_HOST
#define LORA_FREQUENCY_HZ 433000000UL
#define LORA_MAX_PAYLOAD 255

#define REG_FIFO 0x00
#define REG_OP_MODE 0x01
#define REG_FRF_MSB 0x06
#define REG_FRF_MID 0x07
#define REG_FRF_LSB 0x08
#define REG_PA_CONFIG 0x09
#define REG_LNA 0x0C
#define REG_FIFO_ADDR_PTR 0x0D
#define REG_FIFO_TX_BASE_ADDR 0x0E
#define REG_FIFO_RX_BASE_ADDR 0x0F
#define REG_FIFO_RX_CURRENT_ADDR 0x10
#define REG_IRQ_FLAGS 0x12
#define REG_RX_NB_BYTES 0x13
#define REG_PKT_SNR_VALUE 0x19
#define REG_PKT_RSSI_VALUE 0x1A
#define REG_MODEM_CONFIG_1 0x1D
#define REG_MODEM_CONFIG_2 0x1E
#define REG_PREAMBLE_MSB 0x20
#define REG_PREAMBLE_LSB 0x21
#define REG_PAYLOAD_LENGTH 0x22
#define REG_MODEM_CONFIG_3 0x26
#define REG_SYNC_WORD 0x39
#define REG_DIO_MAPPING_1 0x40
#define REG_IRQ_FLAGS_1 0x3E
#define REG_VERSION 0x42

#define MODE_LONG_RANGE_MODE 0x80
#define MODE_SLEEP 0x00
#define MODE_STDBY 0x01
#define MODE_TX 0x03
#define MODE_RX_CONTINUOUS 0x05

#define IRQ_TX_DONE_MASK 0x08
#define IRQ_PAYLOAD_CRC_ERROR_MASK 0x20
#define IRQ_RX_DONE_MASK 0x40

#define IRQ1_CAD_DONE_MASK 0x04
#define IRQ1_CAD_DETECTED_MASK 0x01

#define LORA_BW_125_KHZ 0x70
#define LORA_CR_4_5 0x02
#define LORA_EXPLICIT_HEADER_MODE 0x00
#define LORA_SPREADING_FACTOR 7
#define LORA_TX_CONTINUOUS_MODE 0x00
#define LORA_RX_PAYLOAD_CRC_ON 0x04
#define LORA_LOW_DATA_RATE_OPTIMIZE_OFF 0x00
#define LORA_AGC_AUTO_ON 0x04
#define LORA_MODEM_CONFIG_1 (LORA_BW_125_KHZ | LORA_CR_4_5 | LORA_EXPLICIT_HEADER_MODE)
#define LORA_MODEM_CONFIG_2 ((LORA_SPREADING_FACTOR << 4) | LORA_TX_CONTINUOUS_MODE | LORA_RX_PAYLOAD_CRC_ON)
#define LORA_MODEM_CONFIG_3 (LORA_LOW_DATA_RATE_OPTIMIZE_OFF | LORA_AGC_AUTO_ON)

typedef struct {
    uint32_t id;
    char direction[8];
    char source[FIELD_LEN];
    char destination[FIELD_LEN];
    char type[FIELD_LEN];
    char priority[FIELD_LEN];
    char payload[PAYLOAD_LEN];
    char packet[PACKET_LEN];
    location_info_t origin_location;   // decoded from LOC= at store time
    char thread_key[FIELD_LEN];        // NEW — "ANNOUNCEMENTS" or the peer node_id
    char status[FIELD_LEN];
    uint32_t stored_epoch;
} emergency_message_t;

static void compute_thread_key(char *out, size_t out_size, const char *source, const char *destination);
static void json_escape_string(char *destination, size_t destination_size, const char *source);
static void update_message_status(uint32_t id, const char *source, const char *status);
static void mesh_control_rx_task(void *parameter);
static void retry_tracker_task(void *parameter);
static void lora_tx_worker_task(void *parameter);
static void time_sync_task(void *parameter);
static uint32_t current_epoch_seconds(void);
static void apply_time_sync(uint32_t epoch, uint8_t distance);
static void send_time_sync_packet(uint32_t epoch, uint8_t distance, uint8_t hops);
static void broadcast_time_sync_if_synced(void);
static void write_message_json_chunk(httpd_req_t *request, const emergency_message_t *message, bool first);

typedef struct {
    uint32_t id;
    char source[FIELD_LEN];
    char destination[FIELD_LEN];
    char priority[FIELD_LEN];
    uint8_t attempts;
    uint8_t max_attempts;
    uint8_t mode;
    TickType_t next_retry_tick;
    TickType_t retry_interval_ticks;
    bool active;
} retry_entry_t;

#define RETRY_MODE_ACK 1
#define RETRY_MODE_BROADCAST 2

typedef struct {
    char packet[PACKET_LEN];
} tx_queue_item_t;

static const char *TAG = "barangay_mesh";

static char node_id[FIELD_LEN];
static char ap_ssid[FIELD_LEN];
static emergency_message_t messages[MAX_MESSAGES];
static retry_entry_t retry_entries[MAX_MESSAGES];
static size_t message_count;
static size_t seen_packet_count;
static uint32_t packet_counter;
static uint32_t highest_seen_id;
static TickType_t last_send_tick;
static int64_t epoch_offset_sec;
static bool time_synced;
static uint8_t time_sync_distance;
static TickType_t last_time_sync_broadcast_tick;
static httpd_handle_t http_server;
static rmt_channel_handle_t rgb_led_channel;
static rmt_encoder_handle_t rgb_led_encoder;
static bool rgb_led_ready;
static bool duplicate_node_id_warning;
static TickType_t duplicate_node_id_warning_tick;
static QueueHandle_t lora_tx_queue;
static SemaphoreHandle_t data_mutex;

typedef struct {
    bool active;
    char token[SESSION_TOKEN_LEN];
    char ip[40];
    TickType_t last_seen;
} session_record_t;

typedef struct {
    bool active;
    char ip[40];
    uint8_t failures;
    TickType_t lock_until;
} login_lockout_t;

#define MAX_SESSIONS 4
#define MAX_LOCKOUTS 4
#ifndef SESSION_IDLE_TIMEOUT_MS
#define SESSION_IDLE_TIMEOUT_MS 900000
#endif
#define LOGIN_BASE_LOCK_MS 30000

static session_record_t sessions[MAX_SESSIONS];
static login_lockout_t lockouts[MAX_LOCKOUTS];

#define node_config (*node_config_get())

static void copy_field(char *destination, size_t destination_size, const char *source);
static void save_packet_counter(void);
static void update_highest_seen_id(uint32_t id);
static void data_lock(void)
{
    if (data_mutex != NULL) {
        xSemaphoreTake(data_mutex, portMAX_DELAY);
    }
}

static void data_unlock(void)
{
    if (data_mutex != NULL) {
        xSemaphoreGive(data_mutex);
    }
}

static void set_duplicate_node_id_warning(void)
{
    duplicate_node_id_warning = true;
    duplicate_node_id_warning_tick = xTaskGetTickCount();
}

static bool duplicate_node_id_warning_active(void)
{
    if (!duplicate_node_id_warning) {
        return false;
    }

    if ((xTaskGetTickCount() - duplicate_node_id_warning_tick) > pdMS_TO_TICKS(DUPLICATE_NODE_ID_WARNING_MS)) {
        duplicate_node_id_warning = false;
        return false;
    }

    return true;
}

static bool queue_lora_transmit(const char *packet)
{
    tx_queue_item_t item = {0};

    if (lora_tx_queue == NULL) {
        return lora_transmit(packet);
    }

    copy_field(item.packet, sizeof(item.packet), packet);
    if (xQueueSend(lora_tx_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "LoRa TX queue full; dropping packet");
        return false;
    }

    return true;
}

static void rgb_led_init(void)
{
    rmt_tx_channel_config_t channel_config = {
        .gpio_num = RGB_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    rmt_bytes_encoder_config_t encoder_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = 3,
            .level1 = 0,
            .duration1 = 9,
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = 9,
            .level1 = 0,
            .duration1 = 3,
        },
        .flags.msb_first = 1,
    };

    if (rmt_new_tx_channel(&channel_config, &rgb_led_channel) != ESP_OK) {
        ESP_LOGW(TAG, "RGB LED init failed on GPIO%d", RGB_LED_GPIO);
        return;
    }

    if (rmt_new_bytes_encoder(&encoder_config, &rgb_led_encoder) != ESP_OK) {
        ESP_LOGW(TAG, "RGB LED encoder init failed");
        return;
    }

    if (rmt_enable(rgb_led_channel) != ESP_OK) {
        ESP_LOGW(TAG, "RGB LED channel enable failed");
        return;
    }

    rgb_led_ready = true;
}

static void rgb_led_set(uint8_t red, uint8_t green, uint8_t blue)
{
    uint8_t grb_data[3] = {green, red, blue};
    rmt_transmit_config_t transmit_config = {
        .loop_count = 0,
    };

    if (!rgb_led_ready) {
        return;
    }

    rmt_transmit(rgb_led_channel, rgb_led_encoder, grb_data, sizeof(grb_data), &transmit_config);
    rmt_tx_wait_all_done(rgb_led_channel, pdMS_TO_TICKS(100));
}

static void rgb_led_blink_green(int count)
{
    for (int i = 0; i < count; i++) {
        rgb_led_set(0, 48, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
        rgb_led_set(0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

static const char INDEX_HTML[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Barangay Mesh</title><style>"
    ":root{font-family:Arial,sans-serif;color:#17202a;background:#f5f7f9}"
    "body{margin:0}.top{background:#b91c1c;color:white;padding:16px}.wrap{max-width:760px;margin:auto;padding:16px}"
    ".card{background:white;border:1px solid #d8dee4;border-radius:8px;padding:16px;margin:12px 0}"
    "label{display:block;font-weight:700;margin-top:12px}input,select,textarea,button{box-sizing:border-box;width:100%;font:inherit;padding:12px;margin-top:6px;border-radius:6px;border:1px solid #b8c0cc}"
    "textarea{min-height:94px}button{background:#b91c1c;color:white;border:0;font-weight:700;margin-top:16px}"
    ".row{display:grid;grid-template-columns:1fr 1fr;gap:10px}.muted{color:#5f6b7a;font-size:14px}.msg{border-top:1px solid #e5e7eb;padding:10px 0;word-break:break-word}"
    ".messenger{display:grid;grid-template-columns:280px 1fr;gap:12px}.thread-list{border-right:1px solid #eef2f7;padding-right:8px}.thread-item{padding:10px 12px;border-radius:8px;cursor:pointer;border:1px solid transparent;margin-bottom:8px;background:#f8fafc}.thread-item.active{border-color:#b91c1c;background:#fff1f2}.thread-item small{display:block;color:#5f6b7a;margin-top:4px}.thread-view{min-height:220px}.bubble{background:#fff;border:1px solid #dbe2ea;border-radius:12px;padding:10px 12px;margin:10px 0}.bubble .meta{font-size:12px;color:#5f6b7a;margin-bottom:6px}.location{font-size:12px;color:#475569;margin-top:6px}"
    "@media(max-width:620px){.row{grid-template-columns:1fr}}"
    "@media(max-width:860px){.messenger{grid-template-columns:1fr}.thread-list{border-right:0;padding-right:0}}"
    "</style></head><body><div class=top><div class=wrap><h2>Barangay Emergency Mesh</h2>"
    "<div id=status>Loading status...</div></div></div><main class=wrap>"
    "<section class=card><h3>Send Message</h3><button id=langBtn type=button>Tagalog</button><form method=post action=/send>"
    "<div class=row><label>Your name<input name=sender_name maxlength=31 placeholder='Optional name for accountability'></label><label>Quick templates<select id=template><option value=''>Choose a template</option><option value='Need medical evacuation|MEDICAL|HIGH'>Need medical evacuation</option><option value='Road impassable|EVACUATION|HIGH'>Road impassable</option><option value='All clear|TEST|LOW'>All clear</option></select></label></div>"
    "<fieldset style='border:0;padding:0;margin:0 0 8px 0'><legend style='font-weight:700'>Message scope</legend>"
    "<label><input type=radio name=scope value=announcement checked> Announcement (all nodes)</label>"
    "<label><input type=radio name=scope value=direct> Direct message (specific node)</label></fieldset>"
    "<div id=destWrap style='display:none'><label>Destination Node ID<input id=destination name=destination maxlength=31 placeholder='Example: BRGY001'></label></div>"
    "<input type=hidden id=announcementDest name=destination value=ALL>"
    "<div class=row><label>Emergency Type<select name=type><option>FLOOD</option><option>FIRE</option><option>MEDICAL</option><option>SECURITY</option><option>EVACUATION</option><option>TEST</option></select></label>"
    "<label>Priority<select name=priority><option>HIGH</option><option>NORMAL</option><option>LOW</option></select></label></div>"
    "<label>Message<textarea id=payload name=payload maxlength=159 placeholder='Short emergency message'></textarea></label>"
    "<button type=submit>Queue / Transmit Message</button></form><p class=muted>The portal sends through the SX1278 using GPIO 5/7/6/8/4/16 at 433 MHz.</p></section>"
    "<section class=card><h3>Messages</h3><div class=messenger><div class=thread-list><div class=muted>Loading threads...</div></div><div class=thread-view><div class=muted>Loading messages...</div></div></div></section>"
    "<section class=card><h3>Mesh Health</h3><div id=health class=muted>Loading node roster...</div></section>"
    "<section class=card><h3>Portal</h3><p class=muted id=warningBox></p><p class=muted>Connect to this Wi-Fi when offline, then open http://192.168.4.1. Android/iOS captive checks are redirected here automatically.</p>"
    "<form method=post action=/settime><label>Set time<input name=epoch id=epochInput readonly></label><button type=submit>Sync Clock</button></form>"
    "<form method=post action=/sync><button type=submit>Sync Messages from Mesh</button></form>"
    "<form method=post action=/reset onsubmit='return confirm(\"Factory reset this node and run setup again?\")'><button type=submit>Factory Reset Node</button></form></section>"
    "</main><script>"
    "const scopeRadios=[...document.querySelectorAll('input[name=scope]')];const destWrap=document.getElementById('destWrap');const dest=document.getElementById('destination');const announcementDest=document.getElementById('announcementDest');const template=document.getElementById('template');const payload=document.getElementById('payload');const langBtn=document.getElementById('langBtn');"
    "function syncScope(){let direct=scopeRadios.some(r=>r.checked&&r.value==='direct');destWrap.style.display=direct?'block':'none';dest.required=direct;dest.disabled=!direct;announcementDest.disabled=direct;if(!direct){announcementDest.value='ALL';}}"
    "template.addEventListener('change',()=>{if(!template.value)return;let parts=template.value.split('|');payload.value=parts[0];document.querySelector('select[name=type]').value=parts[1]||'TEST';document.querySelector('select[name=priority]').value=parts[2]||'NORMAL';template.value='';});"
    "scopeRadios.forEach(r=>r.addEventListener('change',syncScope));syncScope();"
    "let activeThread='ANNOUNCEMENTS';let cachedThreads={};"
    "function escapeHtml(v){return String(v).replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('>','&gt;').replaceAll('\"','&quot;');}"
    "function locText(x){return [x.location?.sitio||'',x.location?.barangay||'',x.location?.municipality||''].filter(Boolean).join(' \xE2\x86\x92 ');}"
    "function renderThreads(){let list=document.querySelector('.thread-list');let entries=Object.values(cachedThreads).sort((a,b)=>{if(a.thread_key==='ANNOUNCEMENTS')return -1;if(b.thread_key==='ANNOUNCEMENTS')return 1;return (b.lastSeen||0)-(a.lastSeen||0);});if(!entries.length){list.innerHTML='<div class=muted>No messages yet.</div>';return;}list.innerHTML=entries.map(t=>'<div class=thread-item'+(t.thread_key===activeThread?' active':'')+' data-thread=\"'+escapeHtml(t.thread_key)+'\"><b>'+escapeHtml(t.label)+'</b><small>'+t.messages.length+' message(s)</small></div>').join('');list.querySelectorAll('.thread-item').forEach(el=>el.addEventListener('click',()=>{activeThread=el.dataset.thread;renderThreads();renderThreadView();}));}"
    "function fmtEpoch(e){return e?new Date(e*1000).toLocaleString():'time unknown';}"
    "function renderThreadView(){let view=document.querySelector('.thread-view');let thread=cachedThreads[activeThread]||Object.values(cachedThreads)[0];if(!thread){view.innerHTML='<div class=muted>No messages yet.</div>';return;}view.innerHTML='<h4>'+escapeHtml(thread.label)+'</h4>'+thread.messages.map(x=>'<div class=bubble><div class=meta>'+escapeHtml(x.direction)+' #'+x.id+' | '+escapeHtml(x.type)+' | '+escapeHtml(x.priority)+' | '+escapeHtml(x.thread_key)+' | Status: '+escapeHtml(x.status||'UNKNOWN')+' | '+escapeHtml(fmtEpoch(x.stored_epoch))+'</div><div><b>From:</b> '+escapeHtml(x.source)+'<br><b>To:</b> '+escapeHtml(x.destination)+'<br><b>Payload:</b> '+escapeHtml(x.payload)+'</div><div class=location><b>Location:</b> '+escapeHtml(locText(x))+'</div>'+((x.thread_key==='ANNOUNCEMENTS')?'<div class=location><b>Sender:</b> '+escapeHtml(x.source)+'</div>':'')+'</div>').join('');}"
    "const strings={en:{scope:'Message scope',announcement:'Announcement (all nodes)',direct:'Direct message (specific node)',messages:'Messages',health:'Mesh Health',portal:'Portal',send:'Queue / Transmit Message',toggle:'Tagalog'},tl:{scope:'Layunin ng mensahe',announcement:'Anunsyo (lahat ng node)',direct:'Direktang mensahe (tiyak na node)',messages:'Mga Mensahe',health:'Kalagayan ng Mesh',portal:'Portal',send:'Ipadala ang Mensahe',toggle:'English'}};let lang='en';function applyLang(){let s=strings[lang];document.querySelector('legend').textContent=s.scope;document.querySelectorAll('label')[2].childNodes[1].textContent=' '+s.announcement;document.querySelectorAll('label')[3].childNodes[1].textContent=' '+s.direct;document.querySelectorAll('section.card h3')[1].textContent=s.messages;document.querySelectorAll('section.card h3')[2].textContent=s.health;document.querySelectorAll('section.card h3')[3].textContent=s.portal;document.querySelector('button[type=submit]').textContent=s.send;langBtn.textContent=s.toggle;}langBtn.addEventListener('click',()=>{lang=lang==='en'?'tl':'en';applyLang();});applyLang();"
    "document.getElementById('epochInput').value=Math.floor(Date.now()/1000);"
    "async function load(){let s=await fetch('/api/status').then(r=>r.json());"
    "document.getElementById('status').innerHTML='Node <b>'+s.node+'</b> | '+s.name+' | '+s.location+' | Relay <b>'+s.relay+'</b> | AP <b>'+s.ssid+'</b> | Clients <b>'+s.clients+'</b> | Time <b>'+(s.time_synced?(new Date(s.epoch*1000).toLocaleString()):'unknown')+'</b>';"
    "document.getElementById('warningBox').textContent=s.duplicate_warning?'Possible duplicate node ID on the mesh':'';"
    "let m=await fetch('/api/messages').then(r=>r.json());cachedThreads={};let peers={};m.forEach(x=>{let key=x.thread_key||'UNKNOWN';if(!cachedThreads[key])cachedThreads[key]={thread_key:key,label:key==='ANNOUNCEMENTS'?'Announcements':key,messages:[],lastSeen:0};cachedThreads[key].messages.push(x);cachedThreads[key].lastSeen=Math.max(cachedThreads[key].lastSeen,x.id||0);if(key==='ANNOUNCEMENTS')cachedThreads[key].label='Announcements';else if(!cachedThreads[key].label||cachedThreads[key].label===key)cachedThreads[key].label=key;if(x.source){let p=peers[x.source]||{id:x.source,lastSeen:0,location:''};p.lastSeen=Math.max(p.lastSeen,x.id||0);p.location=locText(x);peers[x.source]=p;}});if(!cachedThreads[activeThread]&&Object.keys(cachedThreads).length){activeThread=Object.keys(cachedThreads)[0];}renderThreads();renderThreadView();let health=document.getElementById('health');let roster=Object.values(peers).sort((a,b)=>b.lastSeen-a.lastSeen);health.innerHTML=roster.length?roster.map(p=>'<div class=msg><b>'+escapeHtml(p.id)+'</b><br><span class=muted>Last seen #'+p.lastSeen+'</span><br>'+escapeHtml(p.location||'Unknown')+'</div>').join(''):'No peers yet.';}"
    "load();setInterval(load,4000);</script></body></html>";

static const char SETUP_HTML[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Setup Barangay Mesh</title><style>"
    ":root{font-family:Arial,sans-serif;color:#17202a;background:#f5f7f9}"
    "body{margin:0}.top{background:#1f6f5b;color:white;padding:16px}.wrap{max-width:720px;margin:auto;padding:16px}"
    ".card{background:white;border:1px solid #d8dee4;border-radius:8px;padding:16px;margin:12px 0}"
    "label{display:block;font-weight:700;margin-top:12px}input,button{box-sizing:border-box;width:100%;font:inherit;padding:12px;margin-top:6px;border-radius:6px;border:1px solid #b8c0cc}"
    "button{background:#1f6f5b;color:white;border:0;font-weight:700;margin-top:16px}.muted{color:#5f6b7a;font-size:14px}"
    "</style></head><body><div class=top><div class=wrap><h2>First-Time Node Setup</h2>"
    "<p>Configure this universal mesh node once. All nodes still send, receive, and relay.</p></div></div>"
    "<main class=wrap><section class=card><form method=post action=/setup>"
    "<label>Node ID<input name=node_id maxlength=31 placeholder='Example: BRGY001, HH023, RELAY04' required></label>"
    "<label>Node Name<input name=node_name maxlength=31 placeholder='Example: Barangay Hall or House 23' required></label>"
    "<label>Sitio / Landmark<input name=sitio maxlength=23 placeholder='Example: Purok 3, Chapel Roof'></label>"
    "<label>Barangay<input name=barangay maxlength=23 placeholder='Example: San Isidro' required></label>"
    "<label>Municipality<input name=municipality maxlength=23 placeholder='Example: Cabuyao'></label>"
    "<label>Default Destination<input name=default_destination maxlength=31 value='BRGY001' placeholder='Example: ALL or BRGY001'></label>"
    "<label>Web PIN<input name=web_pin maxlength=31 placeholder='Generated on first boot' required></label>"
    "<label>Network Key<input name=network_key maxlength=31 placeholder='Generated on first boot or enter pairing key' required></label>"
    "<button type=submit>Save Setup and Reboot</button></form>"
    "<p class=muted>Factory reset later by holding BOOT for 10 seconds during startup.</p></section></main></body></html>";

static const char LOGIN_HTML[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Barangay Mesh Login</title><style>"
    ":root{font-family:Arial,sans-serif;color:#17202a;background:#f5f7f9}"
    "body{margin:0}.top{background:#b91c1c;color:white;padding:16px}.wrap{max-width:420px;margin:auto;padding:16px}"
    ".card{background:white;border:1px solid #d8dee4;border-radius:8px;padding:16px;margin:12px 0}"
    "label{display:block;font-weight:700;margin-top:12px}input,button{box-sizing:border-box;width:100%;font:inherit;padding:12px;margin-top:6px;border-radius:6px;border:1px solid #b8c0cc}"
    "button{background:#b91c1c;color:white;border:0;font-weight:700;margin-top:16px}.muted{color:#5f6b7a;font-size:14px}"
    "</style></head><body><div class=top><div class=wrap><h2>Barangay Emergency Mesh</h2></div></div>"
    "<main class=wrap><section class=card><form method=post action=/login>"
    "<label>Portal PIN<input name=pin maxlength=31 type=password required></label>"
    "<button type=submit>Unlock Portal</button></form><p class=muted>Use the shared PIN configured for this node.</p></section></main></body></html>";

static emergency_message_t *next_message_slot(void)
{
    emergency_message_t *message;

    if (message_count < MAX_MESSAGES) {
        message = &messages[message_count++];
    } else {
        memmove(&messages[0], &messages[1], sizeof(messages[0]) * (MAX_MESSAGES - 1));
        message = &messages[MAX_MESSAGES - 1];
    }

    memset(message, 0, sizeof(*message));
    return message;
}

static void send_ack_packet(const mesh_packet_t *parsed)
{
    char ack_packet[PACKET_LEN];
    char encoded_location[SITIO_LEN + BARANGAY_LEN + MUNICIPALITY_LEN + 2];

    location_encode(&node_config.location, encoded_location, sizeof(encoded_location));

    snprintf(ack_packet, sizeof(ack_packet), "BEMS|%lu|%.*s|%.*s|ACK|NORMAL|HOPS=0|RELAY=%u|LOC=%s|ACK for %lu",
             (unsigned long)parsed->id,
             31,
             node_id,
             31,
             parsed->source,
             1,
             encoded_location,
             (unsigned long)parsed->id);

    ESP_LOGI(TAG, "Sending ACK to %s for packet %lu", parsed->source, (unsigned long)parsed->id);
    queue_lora_transmit(ack_packet);
}

static uint32_t sync_last_id_from_payload(const char *payload)
{
    if (strncmp(payload, "last_id=", 8) == 0) {
        return (uint32_t)strtoul(payload + 8, NULL, 10);
    }

    return 0;
}

static uint32_t ack_id_from_payload(const char *payload)
{
    if (strncmp(payload, "ACK for ", 8) == 0) {
        return (uint32_t)strtoul(payload + 8, NULL, 10);
    }

    return 0;
}

static bool is_control_packet_type(const char *type)
{
    return strcmp(type, "ACK") == 0 || strcmp(type, "SYNC_REQ") == 0 || strcmp(type, "SYNC_RESP") == 0 || strcmp(type, "TIME_SYNC") == 0;
}

static uint32_t current_epoch_seconds(void)
{
    return (uint32_t)(epoch_offset_sec + (int64_t)(esp_timer_get_time() / 1000000LL));
}

static void apply_time_sync(uint32_t epoch, uint8_t distance)
{
    epoch_offset_sec = (int64_t)epoch - (int64_t)(esp_timer_get_time() / 1000000LL);
    time_synced = true;
    time_sync_distance = distance;
    last_time_sync_broadcast_tick = 0;
}

static uint32_t time_sync_epoch_from_payload(const char *payload)
{
    if (strncmp(payload, "epoch=", 6) == 0) {
        return (uint32_t)strtoul(payload + 6, NULL, 10);
    }
    return 0;
}

static uint8_t time_sync_dist_from_payload(const char *payload)
{
    const char *dist_ptr = strstr(payload, "~dist=");
    if (dist_ptr != NULL) {
        return (uint8_t)strtoul(dist_ptr + 6, NULL, 10);
    }
    return 0;
}

static void send_time_sync_packet(uint32_t epoch, uint8_t distance, uint8_t hops)
{
    char packet[PACKET_LEN];
    char encoded_location[SITIO_LEN + BARANGAY_LEN + MUNICIPALITY_LEN + 2];
    uint32_t message_id;

    data_lock();
    message_id = ++packet_counter;
    save_packet_counter();
    data_unlock();

    location_encode(&node_config.location, encoded_location, sizeof(encoded_location));

    snprintf(packet, sizeof(packet), "BEMS|%lu|%.*s|ALL|TIME_SYNC|NORMAL|HOPS=%u|RELAY=1|LOC=%s|epoch=%lu~dist=%u",
             (unsigned long)message_id,
             31,
             node_id,
             hops,
             encoded_location,
             (unsigned long)epoch,
             distance);
    lora_transmit(packet);
}

static void broadcast_time_sync_if_synced(void)
{
    if (!time_synced) {
        return;
    }

    if ((xTaskGetTickCount() - last_time_sync_broadcast_tick) >= pdMS_TO_TICKS(15 * 60 * 1000)) {
        send_time_sync_packet(current_epoch_seconds(), time_sync_distance, 2);
        last_time_sync_broadcast_tick = xTaskGetTickCount();
    }
}

static void write_message_json_chunk(httpd_req_t *request, const emergency_message_t *message, bool first)
{
    char escaped_direction[FIELD_LEN * 2];
    char escaped_source[FIELD_LEN * 2];
    char escaped_destination[FIELD_LEN * 2];
    char escaped_type[FIELD_LEN * 2];
    char escaped_priority[FIELD_LEN * 2];
    char escaped_payload[PAYLOAD_LEN * 2];
    char escaped_packet[PACKET_LEN * 2];
    char escaped_thread_key[FIELD_LEN * 2];
    char escaped_status[FIELD_LEN * 2];
    char escaped_sitio[SITIO_LEN * 2];
    char escaped_barangay[BARANGAY_LEN * 2];
    char escaped_municipality[MUNICIPALITY_LEN * 2];

    json_escape_string(escaped_direction, sizeof(escaped_direction), message->direction);
    json_escape_string(escaped_source, sizeof(escaped_source), message->source);
    json_escape_string(escaped_destination, sizeof(escaped_destination), message->destination);
    json_escape_string(escaped_type, sizeof(escaped_type), message->type);
    json_escape_string(escaped_priority, sizeof(escaped_priority), message->priority);
    json_escape_string(escaped_payload, sizeof(escaped_payload), message->payload);
    json_escape_string(escaped_packet, sizeof(escaped_packet), message->packet);
    json_escape_string(escaped_thread_key, sizeof(escaped_thread_key), message->thread_key);
    json_escape_string(escaped_status, sizeof(escaped_status), message->status);
    json_escape_string(escaped_sitio, sizeof(escaped_sitio), message->origin_location.sitio);
    json_escape_string(escaped_barangay, sizeof(escaped_barangay), message->origin_location.barangay);
    json_escape_string(escaped_municipality, sizeof(escaped_municipality), message->origin_location.municipality);

    httpd_resp_send_chunk(request, first ? "" : ",", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, "{\"id\":", HTTPD_RESP_USE_STRLEN);
    char id_chunk[24];
    snprintf(id_chunk, sizeof(id_chunk), "%lu", (unsigned long)message->id);
    httpd_resp_send_chunk(request, id_chunk, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, ",\"direction\":\"", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, escaped_direction, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, "\",\"source\":\"", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, escaped_source, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, "\",\"destination\":\"", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, escaped_destination, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, "\",\"type\":\"", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, escaped_type, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, "\",\"priority\":\"", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, escaped_priority, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, "\",\"payload\":\"", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, escaped_payload, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, "\",\"packet\":\"", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, escaped_packet, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, "\",\"thread_key\":\"", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, escaped_thread_key, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, "\",\"status\":\"", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, escaped_status, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, "\",\"stored_epoch\":", HTTPD_RESP_USE_STRLEN);
    char epoch_chunk[24];
    snprintf(epoch_chunk, sizeof(epoch_chunk), "%lu", (unsigned long)message->stored_epoch);
    httpd_resp_send_chunk(request, epoch_chunk, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, ",\"location\":{\"sitio\":\"", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, escaped_sitio, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, "\",\"barangay\":\"", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, escaped_barangay, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, "\",\"municipality\":\"", HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, escaped_municipality, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, "\"}}", HTTPD_RESP_USE_STRLEN);
}

static bool is_private_destination_for_other_node(const char *destination, const char *requester)
{
    return strcmp(destination, "ALL") != 0 && strcmp(destination, requester) != 0;
}

static void send_sync_responses(const mesh_packet_t *request)
{
    emergency_message_t *snapshot = malloc(sizeof(*snapshot) * MAX_MESSAGES);
    size_t snapshot_count = 0;
    uint32_t last_id = sync_last_id_from_payload(request->payload);
    char encoded_location[SITIO_LEN + BARANGAY_LEN + MUNICIPALITY_LEN + 2];

    if (snapshot == NULL) {
        ESP_LOGW(TAG, "Unable to allocate SYNC_RESP snapshot");
        return;
    }

    location_encode(&node_config.location, encoded_location, sizeof(encoded_location));

    data_lock();
    for (size_t i = 0; i < message_count && snapshot_count < MAX_MESSAGES; i++) {
        const emergency_message_t *message = &messages[i];

        if (strcmp(message->direction, "RX") != 0 && strcmp(message->direction, "TX") != 0) {
            continue;
        }
        if (message->id <= last_id) {
            continue;
        }
        if (strcmp(message->destination, "ALL") != 0 && strcmp(message->destination, request->source) != 0) {
            continue;
        }
        if (is_private_destination_for_other_node(message->destination, request->source)) {
            continue;
        }

        snapshot[snapshot_count++] = *message;
    }
    data_unlock();

    for (size_t i = 0; i < snapshot_count; i++) {
        char sync_response[PACKET_LEN];
        const char *original_packet = strstr(snapshot[i].packet, "BEMS|");
        int prefix_len;
        size_t original_len;

        vTaskDelay(pdMS_TO_TICKS(200 + (esp_random() % 1001)));
        if (original_packet == NULL) {
            original_packet = snapshot[i].packet;
        }

        prefix_len = snprintf(sync_response, sizeof(sync_response), "BEMS|%lu|%.*s|%.*s|SYNC_RESP|NORMAL|HOPS=0|RELAY=0|LOC=%s|",
                              (unsigned long)snapshot[i].id,
                              31,
                              node_id,
                              31,
                              request->source,
                              encoded_location);
        original_len = strlen(original_packet);

        if (prefix_len < 0 || (size_t)prefix_len >= sizeof(sync_response) || (size_t)prefix_len + original_len > BEMS_MAX_PLAINTEXT) {
            ESP_LOGW(TAG, "Skipping oversized SYNC_RESP for packet %lu", (unsigned long)snapshot[i].id);
            continue;
        }
        copy_field(sync_response + prefix_len, sizeof(sync_response) - (size_t)prefix_len, original_packet);

        ESP_LOGI(TAG, "SYNC_RESP to %s for packet %lu", request->source, (unsigned long)snapshot[i].id);
        queue_lora_transmit(sync_response);
    }

    free(snapshot);
}

static void send_sync_request(uint32_t last_id)
{
    char sync_request[PACKET_LEN];
    char encoded_location[SITIO_LEN + BARANGAY_LEN + MUNICIPALITY_LEN + 2];
    uint32_t request_id;

    data_lock();
    request_id = ++packet_counter;
    save_packet_counter();
    data_unlock();

    location_encode(&node_config.location, encoded_location, sizeof(encoded_location));

    snprintf(sync_request, sizeof(sync_request), "BEMS|%lu|%.*s|ALL|SYNC_REQ|NORMAL|HOPS=1|RELAY=0|LOC=%s|last_id=%lu",
             (unsigned long)request_id,
             31,
             node_id,
             encoded_location,
             (unsigned long)last_id);

    ESP_LOGI(TAG, "Broadcasting SYNC_REQ with last_id=%lu", (unsigned long)last_id);
    queue_lora_transmit(sync_request);
}

static void send_boot_sync_request(void)
{
    send_sync_request(highest_seen_id);
}

static void send_manual_sync_request(void)
{
    send_sync_request(0);
}

static void boot_sync_task(void *parameter)
{
    vTaskDelay(pdMS_TO_TICKS(1500));

    if (node_config.configured) {
        send_boot_sync_request();
    }

    vTaskDelete(NULL);
}

static void store_received_packet(const char *packet, const mesh_packet_t *parsed, int rssi, int snr)
{
    data_lock();
    emergency_message_t *message = next_message_slot();
    bool counter_changed = false;

    if (parsed->valid) {
        message->id = parsed->id;
    } else {
        message->id = ++packet_counter;
        counter_changed = true;
    }
    copy_field(message->direction, sizeof(message->direction), "RX");
    copy_field(message->source, sizeof(message->source), parsed->valid ? parsed->source : "UNKNOWN");
    copy_field(message->destination, sizeof(message->destination), parsed->valid ? parsed->destination : "UNKNOWN");
    copy_field(message->type, sizeof(message->type), parsed->valid ? parsed->type : "RECEIVED");
    copy_field(message->priority, sizeof(message->priority), parsed->valid ? parsed->priority : "NORMAL");
    copy_field(message->payload, sizeof(message->payload), parsed->valid ? parsed->payload : packet);
    if (parsed->valid) {
        message->origin_location = parsed->location;
        compute_thread_key(message->thread_key, sizeof(message->thread_key), parsed->source, parsed->destination);
    } else {
        copy_field(message->thread_key, sizeof(message->thread_key), "UNKNOWN");
    }
    copy_field(message->status, sizeof(message->status), parsed->valid ? "RECEIVED" : "RECEIVED");
    message->stored_epoch = time_synced ? current_epoch_seconds() : 0;
    snprintf(message->packet, sizeof(message->packet), "RSSI=%d SNR=%d | %.*s", rssi, snr, 250, packet);
    if (parsed->valid && !is_control_packet_type(parsed->type)) {
        update_highest_seen_id(message->id);
    }
    if (counter_changed) {
        save_packet_counter();
    }
    data_unlock();
}

static void copy_field(char *destination, size_t destination_size, const char *source)
{
    size_t write_index = 0;

    if (destination_size == 0) {
        return;
    }

    while (*source != '\0' && write_index < destination_size - 1) {
        unsigned char character = (unsigned char)*source++;
        if (character >= 32 && character <= 126) {
            destination[write_index++] = (char)character;
        }
    }

    destination[write_index] = '\0';
}

static void compute_thread_key(char *out, size_t out_size, const char *source, const char *destination)
{
    if (out_size == 0) {
        return;
    }

    if (strcmp(destination, "ALL") == 0) {
        copy_field(out, out_size, "ANNOUNCEMENTS");
        return;
    }

    copy_field(out, out_size, source);
}

static void update_message_status(uint32_t id, const char *source, const char *status)
{
    data_lock();
    for (size_t i = 0; i < message_count; i++) {
        emergency_message_t *message = &messages[i];
        if (message->id == id && strcmp(message->source, source) == 0) {
            copy_field(message->status, sizeof(message->status), status);
            break;
        }
    }
    data_unlock();
}

static bool message_requires_delivery_ack(const char *destination, const char *priority)
{
    return strcmp(destination, "ALL") != 0 && strcmp(priority, "LOW") != 0;
}

static void retry_tracker_add_by_value(uint32_t id, const char *source, const char *destination, const char *priority)
{
    uint8_t max_attempts = 0;
    TickType_t retry_interval = 0;
    uint8_t mode = 0;

    if (strcmp(destination, "ALL") == 0) {
        mode = RETRY_MODE_BROADCAST;
        max_attempts = 3;
        retry_interval = pdMS_TO_TICKS(400);
    } else if (strcmp(priority, "HIGH") == 0) {
        mode = RETRY_MODE_ACK;
        max_attempts = 4;
        retry_interval = pdMS_TO_TICKS(3000);
    } else if (strcmp(priority, "NORMAL") == 0) {
        mode = RETRY_MODE_ACK;
        max_attempts = 2;
        retry_interval = pdMS_TO_TICKS(8000);
    } else {
        return;
    }

    data_lock();
    for (size_t i = 0; i < MAX_MESSAGES; i++) {
        retry_entry_t *entry = &retry_entries[i];
        if (!entry->active) {
            entry->id = id;
            copy_field(entry->source, sizeof(entry->source), source);
            copy_field(entry->destination, sizeof(entry->destination), destination);
            copy_field(entry->priority, sizeof(entry->priority), priority);
            entry->attempts = 1;
            entry->max_attempts = max_attempts;
            entry->mode = mode;
            entry->retry_interval_ticks = retry_interval;
            entry->next_retry_tick = xTaskGetTickCount() + retry_interval;
            entry->active = true;
            break;
        }
    }
    data_unlock();
}

static void retry_tracker_task(void *parameter)
{
    while (true) {
        TickType_t now = xTaskGetTickCount();
        for (size_t i = 0; i < MAX_MESSAGES; i++) {
            retry_entry_t *entry;
            uint32_t retry_id = 0;
            char retry_source[FIELD_LEN] = {0};
            char packet[PACKET_LEN] = {0};
            bool should_retry = false;
            bool should_jitter_repeat = false;
            uint32_t broadcast_retry_id = 0;
            char broadcast_retry_source[FIELD_LEN] = {0};
            char broadcast_packet[PACKET_LEN] = {0};

            data_lock();
            entry = &retry_entries[i];
            if (!entry->active || now < entry->next_retry_tick) {
                data_unlock();
                continue;
            }

            if (entry->mode == RETRY_MODE_BROADCAST) {
                for (size_t j = 0; j < message_count; j++) {
                    emergency_message_t *message = &messages[j];
                    if (message->id != entry->id || strcmp(message->source, entry->source) != 0) {
                        continue;
                    }
                    if (entry->attempts >= entry->max_attempts) {
                        copy_field(message->status, sizeof(message->status), "SENT (no delivery confirmation expected)");
                        entry->active = false;
                        break;
                    }
                    entry->attempts++;
                    entry->next_retry_tick = now + entry->retry_interval_ticks + pdMS_TO_TICKS(200 + (esp_random() % 1001));
                    copy_field(message->status, sizeof(message->status), "SENT (no delivery confirmation expected)");
                    broadcast_retry_id = entry->id;
                    copy_field(broadcast_retry_source, sizeof(broadcast_retry_source), entry->source);
                    copy_field(broadcast_packet, sizeof(broadcast_packet), message->packet);
                    should_jitter_repeat = true;
                    break;
                }
                data_unlock();

                if (should_jitter_repeat) {
                    if (!queue_lora_transmit(broadcast_packet)) {
                        data_lock();
                        for (size_t j = 0; j < message_count; j++) {
                            emergency_message_t *message = &messages[j];
                            if (message->id == broadcast_retry_id && strcmp(message->source, broadcast_retry_source) == 0) {
                                copy_field(message->status, sizeof(message->status), "FAILED");
                                break;
                            }
                        }
                        data_unlock();
                    }
                }
                continue;
            }

            for (size_t j = 0; j < message_count; j++) {
                emergency_message_t *message = &messages[j];
                if (message->id != entry->id || strcmp(message->source, entry->source) != 0) {
                    continue;
                }
                if (strcmp(message->status, "ACKED") == 0) {
                    entry->active = false;
                    break;
                }
                if (entry->attempts >= entry->max_attempts) {
                    copy_field(message->status, sizeof(message->status), "FAILED");
                    entry->active = false;
                    break;
                }
                entry->attempts++;
                entry->next_retry_tick = now + (entry->retry_interval_ticks * entry->attempts);
                copy_field(message->status, sizeof(message->status), "SENT");
                retry_id = entry->id;
                copy_field(retry_source, sizeof(retry_source), entry->source);
                copy_field(packet, sizeof(packet), message->packet);
                should_retry = true;
                break;
            }
            data_unlock();

            if (should_retry) {
                if (!queue_lora_transmit(packet)) {
                    data_lock();
                    for (size_t j = 0; j < message_count; j++) {
                        emergency_message_t *message = &messages[j];
                        if (message->id == retry_id && strcmp(message->source, retry_source) == 0) {
                            copy_field(message->status, sizeof(message->status), "FAILED");
                            break;
                        }
                    }
                    data_unlock();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void lora_tx_worker_task(void *parameter)
{
    tx_queue_item_t item;

    while (true) {
        if (lora_tx_queue == NULL) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (xQueueReceive(lora_tx_queue, &item, portMAX_DELAY) == pdTRUE) {
            lora_transmit(item.packet);
        }
    }
}

static void time_sync_task(void *parameter)
{
    while (true) {
        broadcast_time_sync_if_synced();
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

static void copy_node_id(char *destination, size_t destination_size, const char *source)
{
    size_t write_index = 0;

    if (destination_size == 0) {
        return;
    }

    while (*source != '\0' && write_index < destination_size - 1) {
        unsigned char character = (unsigned char)*source++;
        if (isalnum(character) || character == '-' || character == '_') {
            destination[write_index++] = (char)toupper(character);
        }
    }

    destination[write_index] = '\0';
}

static void nvs_get_string_or_default(nvs_handle_t handle, const char *key, char *value, size_t value_size, const char *fallback)
{
    esp_err_t result = nvs_get_str(handle, key, value, &value_size);

    if (result != ESP_OK || value[0] == '\0') {
        copy_field(value, value_size, fallback);
    }
}

static void load_packet_counter(void)
{
    nvs_handle_t handle;
    uint32_t stored_counter = 0;

    if (nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGI(TAG, "No saved packet counter; starting at %lu", (unsigned long)packet_counter);
        return;
    }

    if (nvs_get_u32(handle, PACKET_COUNTER_KEY, &stored_counter) == ESP_OK) {
        packet_counter = stored_counter;
        ESP_LOGI(TAG, "Packet counter restored: %lu", (unsigned long)packet_counter);
    } else {
        ESP_LOGI(TAG, "No saved packet counter; starting at %lu", (unsigned long)packet_counter);
    }

    nvs_close(handle);
}

static void save_packet_counter(void)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for packet counter: %s", esp_err_to_name(result));
        return;
    }

    result = nvs_set_u32(handle, PACKET_COUNTER_KEY, packet_counter);
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save packet counter: %s", esp_err_to_name(result));
    }

    nvs_close(handle);
}

static void load_highest_seen_id(void)
{
    nvs_handle_t handle;
    uint32_t stored_id = 0;

    if (nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGI(TAG, "No saved highest seen ID; starting at 0");
        return;
    }

    if (nvs_get_u32(handle, HIGHEST_SEEN_ID_KEY, &stored_id) == ESP_OK) {
        highest_seen_id = stored_id;
        ESP_LOGI(TAG, "Highest seen ID restored: %lu", (unsigned long)highest_seen_id);
    } else {
        ESP_LOGI(TAG, "No saved highest seen ID; starting at 0");
    }

    nvs_close(handle);
}

static void save_highest_seen_id(void)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for highest seen ID: %s", esp_err_to_name(result));
        return;
    }

    result = nvs_set_u32(handle, HIGHEST_SEEN_ID_KEY, highest_seen_id);
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save highest seen ID: %s", esp_err_to_name(result));
    }

    nvs_close(handle);
}

static void update_highest_seen_id(uint32_t id)
{
    if (id > highest_seen_id) {
        highest_seen_id = id;
        save_highest_seen_id();
    }
}

static void init_factory_reset_button(void)
{
    gpio_config_t boot_button_config = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&boot_button_config));
}

static void factory_reset_button_task(void *parameter)
{
    bool warning_blinked = false;
    int held_ms = 0;

    while (true) {
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            held_ms += 100;

            if (!warning_blinked && held_ms >= RESET_WARNING_MS) {
                ESP_LOGW(TAG, "BOOT held for 5 seconds. Keep holding for factory reset.");
                rgb_led_blink_green(1);
                warning_blinked = true;
            }

            if (held_ms >= FACTORY_RESET_HOLD_MS) {
                ESP_LOGW(TAG, "BOOT held for 10 seconds. Factory reset confirmed.");
                rgb_led_blink_green(3);
                node_config_erase();
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        } else {
            if (held_ms > 0 && held_ms < FACTORY_RESET_HOLD_MS) {
                ESP_LOGI(TAG, "Factory reset hold cancelled");
            }
            held_ms = 0;
            warning_blinked = false;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static int hex_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static void url_decode(char *value)
{
    char *read_ptr = value;
    char *write_ptr = value;

    while (*read_ptr != '\0') {
        if (*read_ptr == '+') {
            *write_ptr++ = ' ';
            read_ptr++;
        } else if (*read_ptr == '%' && isxdigit((unsigned char)read_ptr[1]) && isxdigit((unsigned char)read_ptr[2])) {
            *write_ptr++ = (char)((hex_value(read_ptr[1]) << 4) | hex_value(read_ptr[2]));
            read_ptr += 3;
        } else {
            *write_ptr++ = *read_ptr++;
        }
    }

    *write_ptr = '\0';
}

static bool form_value(const char *body, const char *key, char *output, size_t output_size)
{
    const size_t key_len = strlen(key);
    const char *cursor = body;

    while (cursor != NULL && *cursor != '\0') {
        if (strncmp(cursor, key, key_len) == 0 && cursor[key_len] == '=') {
            const char *value_start = cursor + key_len + 1;
            const char *value_end = strchr(value_start, '&');
            size_t value_len = value_end == NULL ? strlen(value_start) : (size_t)(value_end - value_start);
            size_t copy_len = MIN(value_len, output_size - 1);

            memcpy(output, value_start, copy_len);
            output[copy_len] = '\0';
            url_decode(output);
            return true;
        }

        cursor = strchr(cursor, '&');
        if (cursor != NULL) {
            cursor++;
        }
    }

    return false;
}

static void build_location_string(char *out, size_t out_size)
{
    location_encode(&node_config.location, out, out_size);
}

static void send_control_packet(const char *type, const char *destination, const char *payload)
{
    char packet[PACKET_LEN];
    char encoded_location[SITIO_LEN + BARANGAY_LEN + MUNICIPALITY_LEN + 2];
    uint32_t message_id;

    data_lock();
    message_id = ++packet_counter;
    save_packet_counter();
    data_unlock();

    build_location_string(encoded_location, sizeof(encoded_location));
    snprintf(packet, sizeof(packet), "BEMS|%lu|%.*s|%.*s|%s|NORMAL|HOPS=1|RELAY=0|LOC=%s|%s",
             (unsigned long)message_id,
             31,
             node_id,
             31,
             destination,
             type,
             encoded_location,
             payload != NULL ? payload : "");

    lora_transmit(packet);
}

static bool node_id_collision_detected(const char *proposed_id)
{
    char packet[PACKET_LEN];
    int rssi = 0;
    int snr = 0;
    int64_t start_us = esp_timer_get_time();

    send_control_packet("ID_CHECK", "ALL", proposed_id);

    while ((esp_timer_get_time() - start_us) < 2500000LL) {
        if (!lora_receive_plain_packet(packet, sizeof(packet), &rssi, &snr, 250)) {
            continue;
        }

        mesh_packet_t parsed;
        if (!parse_mesh_packet(packet, &parsed)) {
            continue;
        }

        if (strcmp(parsed.type, "ID_TAKEN") == 0 && strcmp(parsed.destination, proposed_id) == 0) {
            return true;
        }
    }

    return false;
}

static void mesh_control_rx_task(void *parameter)
{
    char packet[PACKET_LEN];
    int rssi = 0;
    int snr = 0;

    while (true) {
        if (!lora_receive_plain_packet(packet, sizeof(packet), &rssi, &snr, 1000)) {
            continue;
        }

        mesh_packet_t parsed;
        if (!parse_mesh_packet(packet, &parsed)) {
            continue;
        }

        if (strcmp(parsed.type, "ID_CHECK") == 0 && strcmp(parsed.destination, node_id) == 0 && strcmp(parsed.source, node_id) != 0) {
            char reply[PACKET_LEN];
            char encoded_location[SITIO_LEN + BARANGAY_LEN + MUNICIPALITY_LEN + 2];

            vTaskDelay(pdMS_TO_TICKS(200 + (esp_random() % 1001)));
            build_location_string(encoded_location, sizeof(encoded_location));
            snprintf(reply, sizeof(reply), "BEMS|%lu|%.*s|%.*s|ID_TAKEN|NORMAL|HOPS=0|RELAY=0|LOC=%s|%.*s",
                     (unsigned long)++packet_counter,
                     31,
                     node_id,
                     31,
                     parsed.source,
                     encoded_location,
                     31,
                     node_id);
            save_packet_counter();
            lora_transmit(reply);
        }
    }
}

static void json_escape_string(char *destination, size_t destination_size, const char *source)
{
    size_t write_index = 0;

    if (destination_size == 0) {
        return;
    }

    while (*source != '\0' && write_index < destination_size - 1) {
        unsigned char character = (unsigned char)*source++;

        if ((character == '"' || character == '\\') && write_index < destination_size - 2) {
            destination[write_index++] = '\\';
            destination[write_index++] = (char)character;
        } else if (character < 0x20) {
            if (write_index + 6 >= destination_size) {
                break;
            }
            destination[write_index++] = '\\';
            destination[write_index++] = 'u';
            destination[write_index++] = '0';
            destination[write_index++] = '0';
            destination[write_index++] = "0123456789ABCDEF"[character >> 4];
            destination[write_index++] = "0123456789ABCDEF"[character & 0x0F];
        } else if (character != '"' && character != '\\') {
            destination[write_index++] = (char)character;
        } else {
            break;
        }
    }

    destination[write_index] = '\0';
}

static int hops_for_priority(const char *priority)
{
    if (strcmp(priority, "HIGH") == 0) {
        return 5;
    }
    if (strcmp(priority, "LOW") == 0) {
        return 1;
    }

    return 3;
}

static void build_packet(emergency_message_t *message)
{
    char encoded_location[SITIO_LEN + BARANGAY_LEN + MUNICIPALITY_LEN + 2];
    size_t offset = 0;
    int written;

    location_encode(&node_config.location, encoded_location, sizeof(encoded_location));

    written = snprintf(message->packet + offset, sizeof(message->packet) - offset, "BEMS|%lu|%.*s|%.*s|%.*s|%.*s|HOPS=%d|RELAY=%u|LOC=%s|",
                       (unsigned long)message->id,
                       31,
                       node_id,
                       31,
                       message->destination,
                       31,
                       message->type,
                       31,
                       message->priority,
                       hops_for_priority(message->priority),
                       1,
                       encoded_location);
    if (written < 0 || (size_t)written >= sizeof(message->packet) - offset) {
        message->packet[0] = '\0';
        return;
    }
    offset += (size_t)written;

    written = snprintf(message->packet + offset, sizeof(message->packet) - offset, "%.*s", 120, message->payload);
    if (written < 0 || (size_t)written >= sizeof(message->packet) - offset) {
        message->packet[sizeof(message->packet) - 1] = '\0';
        return;
    }
}

static void queue_message(const char *destination, const char *type, const char *priority, const char *payload)
{
    char packet[PACKET_LEN];
    uint32_t queued_id;
    char queued_source[FIELD_LEN];
    char queued_destination[FIELD_LEN];
    char queued_priority[FIELD_LEN];

    data_lock();
    emergency_message_t *message = next_message_slot();

    message->id = ++packet_counter;
    copy_field(message->direction, sizeof(message->direction), "TX");
    copy_field(message->source, sizeof(message->source), node_id);
    copy_field(message->destination, sizeof(message->destination), destination);
    copy_field(message->type, sizeof(message->type), type);
    copy_field(message->priority, sizeof(message->priority), priority);
    copy_field(message->payload, sizeof(message->payload), payload);
    copy_field(message->status, sizeof(message->status), "PENDING");
    message->stored_epoch = time_synced ? current_epoch_seconds() : 0;
    build_packet(message);
    compute_thread_key(message->thread_key, sizeof(message->thread_key), message->source, message->destination);
    message->origin_location = node_config.location;
    queued_id = message->id;
    copy_field(queued_source, sizeof(queued_source), message->source);
    copy_field(queued_destination, sizeof(queued_destination), message->destination);
    copy_field(queued_priority, sizeof(queued_priority), message->priority);
    copy_field(packet, sizeof(packet), message->packet);
    save_packet_counter();
    data_unlock();

    ESP_LOGI(TAG, "LoRa TX pending: %s", packet);
    if (lora_transmit(packet)) {
        if (strcmp(queued_destination, "ALL") == 0 || !message_requires_delivery_ack(queued_destination, queued_priority)) {
            update_message_status(queued_id, queued_source, "SENT (no delivery confirmation expected)");
        } else {
            update_message_status(queued_id, queued_source, "SENT");
        }
        retry_tracker_add_by_value(queued_id, queued_source, queued_destination, queued_priority);
    } else {
        update_message_status(queued_id, queued_source, "FAILED");
    }
}

static esp_err_t send_redirect(httpd_req_t *request, const char *location)
{
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", location);
    httpd_resp_send(request, "", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void ipv4_to_string(const struct sockaddr_in *addr, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }

    snprintf(out, out_size, "%u.%u.%u.%u",
             (unsigned int)((ntohl(addr->sin_addr.s_addr) >> 24) & 0xFF),
             (unsigned int)((ntohl(addr->sin_addr.s_addr) >> 16) & 0xFF),
             (unsigned int)((ntohl(addr->sin_addr.s_addr) >> 8) & 0xFF),
             (unsigned int)(ntohl(addr->sin_addr.s_addr) & 0xFF));
}

static void request_client_id(httpd_req_t *request, char *out, size_t out_size)
{
    int sockfd = httpd_req_to_sockfd(request);
    struct sockaddr_in addr = {0};
    socklen_t len = sizeof(addr);

    if (getpeername(sockfd, (struct sockaddr *)&addr, &len) == 0) {
        ipv4_to_string(&addr, out, out_size);
        return;
    }

    snprintf(out, out_size, "sockfd:%d", sockfd);
}

static void random_session_token(char *out, size_t out_size)
{
    uint32_t random_a = esp_random();
    uint32_t random_b = esp_random();
    snprintf(out, out_size, "%08lX%08lX", (unsigned long)random_a, (unsigned long)random_b);
}

static session_record_t *session_find_by_token(const char *token)
{
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active && strcmp(sessions[i].token, token) == 0) {
            return &sessions[i];
        }
    }
    return NULL;
}

static session_record_t *session_alloc(void)
{
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].active) {
            return &sessions[i];
        }
    }
    return &sessions[0];
}

static void session_touch(session_record_t *session)
{
    session->last_seen = xTaskGetTickCount();
}

static bool cookie_matches_session(const char *cookie_header)
{
    const char *cursor = cookie_header;
    char token[128];

    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == ';') {
            cursor++;
        }

        size_t token_len = 0;
        while (cursor[token_len] != '\0' && cursor[token_len] != ';' && token_len < sizeof(token) - 1) {
            token[token_len] = cursor[token_len];
            token_len++;
        }
        token[token_len] = '\0';

        if (strncmp(token, SESSION_COOKIE_NAME "=", strlen(SESSION_COOKIE_NAME) + 1) == 0) {
            const char *value = token + strlen(SESSION_COOKIE_NAME) + 1;
            session_record_t *session = session_find_by_token(value);
            if (session != NULL) {
                session_touch(session);
                return true;
            }
        }

        cursor += token_len;
        while (*cursor == ';' || *cursor == ' ') {
            cursor++;
        }
    }

    return false;
}

static void purge_expired_sessions(void)
{
    TickType_t now = xTaskGetTickCount();
    for (size_t i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].active && (now - sessions[i].last_seen) > pdMS_TO_TICKS(SESSION_IDLE_TIMEOUT_MS)) {
            sessions[i].active = false;
        }
    }
}

static login_lockout_t *lockout_find_or_alloc(const char *client_id)
{
    for (size_t i = 0; i < MAX_LOCKOUTS; i++) {
        if (lockouts[i].active && strcmp(lockouts[i].ip, client_id) == 0) {
            return &lockouts[i];
        }
    }
    for (size_t i = 0; i < MAX_LOCKOUTS; i++) {
        if (!lockouts[i].active) {
            lockouts[i].active = true;
            copy_field(lockouts[i].ip, sizeof(lockouts[i].ip), client_id);
            return &lockouts[i];
        }
    }
    return &lockouts[0];
}

static bool login_is_locked(const char *client_id, TickType_t *remaining)
{
    login_lockout_t *lockout = lockout_find_or_alloc(client_id);
    TickType_t now = xTaskGetTickCount();

    if (lockout->lock_until != 0 && now < lockout->lock_until) {
        if (remaining != NULL) {
            *remaining = lockout->lock_until - now;
        }
        return true;
    }

    if (remaining != NULL) {
        *remaining = 0;
    }
    return false;
}

static void login_record_failure(const char *client_id)
{
    login_lockout_t *lockout = lockout_find_or_alloc(client_id);
    TickType_t lock_ms;

    lockout->failures++;
    if (lockout->failures < 3) {
        lockout->lock_until = 0;
        return;
    }

    if (lockout->failures == 3) {
        lock_ms = LOGIN_BASE_LOCK_MS;
    } else {
        lock_ms = LOGIN_BASE_LOCK_MS << (lockout->failures - 3);
    }
    lockout->lock_until = xTaskGetTickCount() + pdMS_TO_TICKS(lock_ms);
}

static void login_record_success(const char *client_id)
{
    login_lockout_t *lockout = lockout_find_or_alloc(client_id);
    lockout->failures = 0;
    lockout->lock_until = 0;
}

static void issue_session_for_client(const char *client_id, char *token_out, size_t token_size)
{
    session_record_t *session = session_alloc();
    random_session_token(session->token, sizeof(session->token));
    session->active = true;
    copy_field(session->ip, sizeof(session->ip), client_id);
    session_touch(session);
    copy_field(token_out, token_size, session->token);
}

static bool request_has_session(httpd_req_t *request)
{
    char cookie[128] = {0};

    if (!node_config.configured) {
        return true;
    }

    if (httpd_req_get_hdr_value_str(request, "Cookie", cookie, sizeof(cookie)) != ESP_OK) {
        return false;
    }

    return cookie_matches_session(cookie);
}

static esp_err_t require_session(httpd_req_t *request)
{
    if (request_has_session(request)) {
        return ESP_OK;
    }

    return send_redirect(request, "/");
}

static esp_err_t index_handler(httpd_req_t *request)
{
    purge_expired_sessions();
    httpd_resp_set_type(request, "text/html");

    if (!node_config.configured) {
        return httpd_resp_send(request, SETUP_HTML, HTTPD_RESP_USE_STRLEN);
    }
    if (!request_has_session(request)) {
        return httpd_resp_send(request, LOGIN_HTML, HTTPD_RESP_USE_STRLEN);
    }

    return httpd_resp_send(request, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t login_handler(httpd_req_t *request)
{
    char body[96] = {0};
    char pin[FIELD_LEN] = {0};
    char token[SESSION_TOKEN_LEN];
    char cookie[128];
    char client_id[40];
    int received = 0;
    TickType_t remaining = 0;

    request_client_id(request, client_id, sizeof(client_id));
    if (login_is_locked(client_id, &remaining)) {
        unsigned int wait_seconds = (unsigned int)pdTICKS_TO_MS(remaining) / 1000U;
        char message[96];
        httpd_resp_set_status(request, "429 Too Many Requests");
        httpd_resp_set_type(request, "text/plain");
        snprintf(message, sizeof(message), "Too many failed attempts. Please wait %u seconds.", wait_seconds);
        return httpd_resp_send(request, message, HTTPD_RESP_USE_STRLEN);
    }

    while (received < request->content_len && received < (int)sizeof(body) - 1) {
        int ret = httpd_req_recv(request, body + received, MIN(request->content_len - received, (int)sizeof(body) - 1 - received));
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }

    form_value(body, "pin", pin, sizeof(pin));
    if (strcmp(pin, node_config.web_pin) != 0) {
        login_record_failure(client_id);
        httpd_resp_set_status(request, "403 Forbidden");
        httpd_resp_set_type(request, "text/html");
        return httpd_resp_send(request, LOGIN_HTML, HTTPD_RESP_USE_STRLEN);
    }

    login_record_success(client_id);
    issue_session_for_client(client_id, token, sizeof(token));
    snprintf(cookie, sizeof(cookie), "%s=%s; Path=/; HttpOnly; SameSite=Lax", SESSION_COOKIE_NAME, token);
    httpd_resp_set_hdr(request, "Set-Cookie", cookie);
    return send_redirect(request, "/");
}

static esp_err_t setup_handler(httpd_req_t *request)
{
    char body[384] = {0};
    char raw_node_id[FIELD_LEN] = {0};
    node_config_t new_config = {0};
    int received = 0;

    while (received < request->content_len && received < (int)sizeof(body) - 1) {
        int ret = httpd_req_recv(request, body + received, MIN(request->content_len - received, (int)sizeof(body) - 1 - received));
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }

    form_value(body, "node_id", raw_node_id, sizeof(raw_node_id));
    copy_node_id(new_config.node_id, sizeof(new_config.node_id), raw_node_id);
    form_value(body, "node_name", new_config.node_name, sizeof(new_config.node_name));
    form_value(body, "sitio", new_config.location.sitio, sizeof(new_config.location.sitio));
    form_value(body, "barangay", new_config.location.barangay, sizeof(new_config.location.barangay));
    form_value(body, "municipality", new_config.location.municipality, sizeof(new_config.location.municipality));
    form_value(body, "default_destination", new_config.default_destination, sizeof(new_config.default_destination));
    form_value(body, "web_pin", new_config.web_pin, sizeof(new_config.web_pin));
    form_value(body, "network_key", new_config.network_key, sizeof(new_config.network_key));
    new_config.configured = true;
    if (new_config.node_id[0] == '\0') {
        copy_field(new_config.node_id, sizeof(new_config.node_id), node_id);
    }
    // Phase 7 will reuse this gate for the Node ID wizard field.
    if (node_id_collision_detected(new_config.node_id)) {
        httpd_resp_set_status(request, "409 Conflict");
        httpd_resp_set_type(request, "text/plain");
        return httpd_resp_send(request, "Node ID is already in use on the mesh", HTTPD_RESP_USE_STRLEN);
    }
    if (new_config.node_name[0] == '\0') {
        copy_field(new_config.node_name, sizeof(new_config.node_name), "Mesh Node");
    }
    if (new_config.location.barangay[0] == '\0') {
        copy_field(new_config.location.barangay, sizeof(new_config.location.barangay), "Unknown");
    }
    if (new_config.default_destination[0] == '\0') {
        copy_field(new_config.default_destination, sizeof(new_config.default_destination), "BRGY001");
    }
    if (new_config.web_pin[0] == '\0') {
        copy_field(new_config.web_pin, sizeof(new_config.web_pin), node_config_get_web_pin());
    }
    if (new_config.network_key[0] == '\0') {
        copy_field(new_config.network_key, sizeof(new_config.network_key), node_config_get_network_key());
    }

    ESP_ERROR_CHECK(node_config_save(&new_config));
    httpd_resp_set_type(request, "text/html");
    httpd_resp_send(request, "<!doctype html><html><body><h2>Setup saved.</h2><p>Node is restarting.</p></body></html>", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t settime_handler(httpd_req_t *request)
{
    char body[128] = {0};
    char epoch_value[FIELD_LEN] = {0};
    uint32_t epoch = 0;
    int received = 0;
    esp_err_t session_result = require_session(request);

    if (session_result != ESP_OK) {
        return session_result;
    }

    while (received < request->content_len && received < (int)sizeof(body) - 1) {
        int ret = httpd_req_recv(request, body + received, MIN(request->content_len - received, (int)sizeof(body) - 1 - received));
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }

    form_value(body, "epoch", epoch_value, sizeof(epoch_value));
    epoch = (uint32_t)strtoul(epoch_value, NULL, 10);
    if (epoch == 0) {
        httpd_resp_set_status(request, "400 Bad Request");
        return httpd_resp_send(request, "Invalid epoch", HTTPD_RESP_USE_STRLEN);
    }

    apply_time_sync(epoch, 0);
    send_time_sync_packet(epoch, 0, 2);
    return send_redirect(request, "/");
}

static esp_err_t reset_handler(httpd_req_t *request)
{
    esp_err_t session_result = require_session(request);
    if (session_result != ESP_OK) {
        return session_result;
    }

    node_config_erase();
    httpd_resp_set_type(request, "text/html");
    httpd_resp_send(request, "<!doctype html><html><body><h2>Factory reset complete.</h2><p>Node is restarting into setup mode.</p></body></html>", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t send_handler(httpd_req_t *request)
{
    char body[384] = {0};
    char destination[FIELD_LEN];
    char sender_name[FIELD_LEN] = {0};
    char type[FIELD_LEN] = "TEST";
    char priority[FIELD_LEN] = "NORMAL";
    char payload[PAYLOAD_LEN] = "No message";
    int received = 0;
    esp_err_t session_result = require_session(request);

    if (session_result != ESP_OK) {
        return session_result;
    }

    while (received < request->content_len && received < (int)sizeof(body) - 1) {
        int ret = httpd_req_recv(request, body + received, MIN(request->content_len - received, (int)sizeof(body) - 1 - received));
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }

    copy_field(destination, sizeof(destination), node_config.default_destination);
    form_value(body, "sender_name", sender_name, sizeof(sender_name));
    form_value(body, "destination", destination, sizeof(destination));
    form_value(body, "type", type, sizeof(type));
    form_value(body, "priority", priority, sizeof(priority));
    form_value(body, "payload", payload, sizeof(payload));

    if ((xTaskGetTickCount() - last_send_tick) < pdMS_TO_TICKS(10000)) {
        httpd_resp_set_status(request, "429 Too Many Requests");
        httpd_resp_set_type(request, "text/html");
        return httpd_resp_send(request, "<!doctype html><html><body><h2>Slow down.</h2><p>Please wait before sending again.</p></body></html>", HTTPD_RESP_USE_STRLEN);
    }

    last_send_tick = xTaskGetTickCount();
    if (sender_name[0] != '\0') {
        char named_payload[PAYLOAD_LEN];
        size_t prefix_len;

        named_payload[0] = '\0';
        copy_field(named_payload, sizeof(named_payload), sender_name);
        prefix_len = strlen(named_payload);
        if (prefix_len < sizeof(named_payload) - 3) {
            named_payload[prefix_len++] = ':';
            named_payload[prefix_len++] = ' ';
            named_payload[prefix_len] = '\0';
            copy_field(named_payload + prefix_len, sizeof(named_payload) - prefix_len, payload);
        }
        copy_field(payload, sizeof(payload), named_payload);
    }

    queue_message(destination, type, priority, payload);
    return send_redirect(request, "/");
}

static esp_err_t sync_handler(httpd_req_t *request)
{
    esp_err_t session_result = require_session(request);

    if (session_result != ESP_OK) {
        return session_result;
    }

    send_manual_sync_request();
    return send_redirect(request, "/");
}

static esp_err_t status_handler(httpd_req_t *request)
{
    wifi_sta_list_t clients = {0};
    char response[512];
    char escaped_node[FIELD_LEN * 2];
    char escaped_name[FIELD_LEN * 2];
    char escaped_location[FIELD_LEN * 2];
    char escaped_ssid[FIELD_LEN * 2];
    char escaped_relay[8];
    bool duplicate_warning;
    size_t current_message_count;
    esp_err_t session_result = require_session(request);

    if (session_result != ESP_OK) {
        return session_result;
    }

    esp_wifi_ap_get_sta_list(&clients);
    data_lock();
    current_message_count = message_count;
    data_unlock();

    json_escape_string(escaped_node, sizeof(escaped_node), node_id);
    json_escape_string(escaped_name, sizeof(escaped_name), node_config.node_name);
    json_escape_string(escaped_location, sizeof(escaped_location), node_config.location.barangay);
    json_escape_string(escaped_ssid, sizeof(escaped_ssid), ap_ssid);
    json_escape_string(escaped_relay, sizeof(escaped_relay), "true");
    duplicate_warning = duplicate_node_id_warning_active();

    snprintf(response, sizeof(response),
             "{\"node\":\"%s\",\"name\":\"%s\",\"location\":\"%s\",\"ssid\":\"%s\",\"clients\":%u,\"messages\":%u,\"configured\":%s,\"relay\":\"%s\",\"duplicate_warning\":%s,\"time_synced\":%s,\"epoch\":%lu}",
             escaped_node,
             escaped_name,
             escaped_location,
             escaped_ssid,
             clients.num,
             (unsigned int)current_message_count,
             node_config.configured ? "true" : "false",
             escaped_relay,
             duplicate_warning ? "true" : "false",
             time_synced ? "true" : "false",
             (unsigned long)current_epoch_seconds());

    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t messages_handler(httpd_req_t *request)
{
    emergency_message_t message_copy;
    size_t snapshot_count = 0;
    esp_err_t session_result = require_session(request);

    if (session_result != ESP_OK) {
        return session_result;
    }

    httpd_resp_set_type(request, "application/json");
    httpd_resp_send_chunk(request, "[", 1);
    data_lock();
    snapshot_count = message_count;
    data_unlock();
    for (size_t i = 0; i < snapshot_count && i < MAX_MESSAGES; i++) {
        bool wrote = false;

        data_lock();
        if (message_count > i) {
            message_copy = messages[message_count - 1 - i];
            wrote = true;
        }
        data_unlock();

        if (wrote) {
            write_message_json_chunk(request, &message_copy, i == 0);
        }
    }
    httpd_resp_send_chunk(request, "]", 1);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t captive_handler(httpd_req_t *request)
{
    return send_redirect(request, "/");
}

static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_PORT;
    config.max_uri_handlers = 16;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 16384;

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_handler},
        {.uri = "/login", .method = HTTP_POST, .handler = login_handler},
        {.uri = "/setup", .method = HTTP_POST, .handler = setup_handler},
        {.uri = "/reset", .method = HTTP_POST, .handler = reset_handler},
        {.uri = "/settime", .method = HTTP_POST, .handler = settime_handler},
        {.uri = "/send", .method = HTTP_POST, .handler = send_handler},
        {.uri = "/sync", .method = HTTP_POST, .handler = sync_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/messages", .method = HTTP_GET, .handler = messages_handler},
        {.uri = "/generate_204", .method = HTTP_GET, .handler = captive_handler},
        {.uri = "/gen_204", .method = HTTP_GET, .handler = captive_handler},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_handler},
        {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_handler},
        {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_handler},
        {.uri = "/*", .method = HTTP_GET, .handler = captive_handler},
    };

    ESP_ERROR_CHECK(httpd_start(&http_server, &config));

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(http_server, &routes[i]));
    }
}

static void dns_task(void *parameter)
{
    const uint32_t ap_ip = inet_addr("192.168.4.1");
    uint8_t buffer[512];
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (sock < 0 || bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS server failed");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int len = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&client_addr, &client_len);

        if (len < 12) {
            continue;
        }

        int question_end = 12;
        while (question_end < len && buffer[question_end] != 0) {
            question_end += buffer[question_end] + 1;
        }

        if (question_end + 5 > len) {
            continue;
        }

        buffer[2] = 0x81;
        buffer[3] = 0x80;
        buffer[6] = 0x00;
        buffer[7] = 0x01;
        buffer[8] = 0x00;
        buffer[9] = 0x00;
        buffer[10] = 0x00;
        buffer[11] = 0x00;

        int answer = question_end + 5;
        if (answer + 16 > (int)sizeof(buffer)) {
            continue;
        }

        buffer[answer++] = 0xC0;
        buffer[answer++] = 0x0C;
        buffer[answer++] = 0x00;
        buffer[answer++] = 0x01;
        buffer[answer++] = 0x00;
        buffer[answer++] = 0x01;
        buffer[answer++] = 0x00;
        buffer[answer++] = 0x00;
        buffer[answer++] = 0x00;
        buffer[answer++] = 0x3C;
        buffer[answer++] = 0x00;
        buffer[answer++] = 0x04;
        memcpy(&buffer[answer], &ap_ip, 4);
        answer += 4;

        sendto(sock, buffer, answer, 0, (struct sockaddr *)&client_addr, client_len);
    }
}

static void init_identity(void)
{
    uint8_t mac[6];

    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    snprintf(node_id, sizeof(node_id), "NODE%02X%02X", mac[4], mac[5]);
    snprintf(ap_ssid, sizeof(ap_ssid), "BarangayMesh-SETUP-%02X%02X", mac[4], mac[5]);
    packet_counter = esp_random() & 0xFFFF;
}

static void start_wifi_ap(void)
{
    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_config = {0};

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));

    copy_field((char *)wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid), ap_ssid);
    wifi_config.ap.ssid_len = strlen(ap_ssid);
    wifi_config.ap.channel = AP_CHANNEL;
    wifi_config.ap.max_connection = AP_MAX_CONNECTIONS;
    // This AP passphrase is distinct from the portal PIN and protects Wi-Fi association.
    copy_field((char *)wifi_config.ap.password, sizeof(wifi_config.ap.password), node_config_get_ap_password());
    wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Captive portal AP started: %s", ap_ssid);
    ESP_LOGI(TAG, "Open http://192.168.4.1 after connecting");
}

void app_main(void)
{
    esp_err_t nvs_status = nvs_flash_init();
    if (nvs_status == ESP_ERR_NVS_NO_FREE_PAGES || nvs_status == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_status);
    }

    data_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(data_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    lora_tx_queue = xQueueCreate(8, sizeof(tx_queue_item_t));
    ESP_ERROR_CHECK(lora_tx_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    init_identity();
    load_packet_counter();
    load_highest_seen_id();
    rgb_led_init();
    init_factory_reset_button();
    node_config_load();
    lora_init();
    xTaskCreate(mesh_control_rx_task, "mesh_control_rx_task", 4096, NULL, 6, NULL);
    xTaskCreate(lora_tx_worker_task, "lora_tx_worker_task", 4096, NULL, 6, NULL);
    xTaskCreate(boot_sync_task, "boot_sync_task", 4096, NULL, 4, NULL);
    xTaskCreate(retry_tracker_task, "retry_tracker_task", 4096, NULL, 3, NULL);
    xTaskCreate(time_sync_task, "time_sync_task", 3072, NULL, 2, NULL);
    start_wifi_ap();
    start_http_server();
    xTaskCreate(factory_reset_button_task, "factory_reset_button_task", 3072, NULL, 7, NULL);
    xTaskCreate(dns_task, "dns_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Universal mesh node portal ready as %s", node_id);
}
