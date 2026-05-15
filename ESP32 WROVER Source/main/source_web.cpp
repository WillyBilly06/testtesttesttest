#include "source_web.h"

#include <inttypes.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "cp_logo_png.hpp"
#include "source_audio_stats.h"
#include "source_config.h"
#include "source_udp_control.h"
#include "source_udp_stream.h"
#include "source_wifi.h"

static const char *TAG = "source_web";
static httpd_handle_t s_httpd;
static volatile uint32_t s_web_requests;
static volatile int64_t s_status_last_us;

static void web_count_request(void)
{
    s_web_requests++;
}

static void set_close_header(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Connection", "close");
}

static void set_dynamic_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    set_close_header(req);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; ++p) {
        if (*p == '+') {
            *o++ = ' ';
        } else if (*p == '%' && isxdigit((unsigned char)p[1]) &&
                   isxdigit((unsigned char)p[2])) {
            *o++ = (char)((hexval(p[1]) << 4) | hexval(p[2]));
            p += 2;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

static bool form_get(char *body, const char *key, char *out, size_t out_len)
{
    size_t key_len = strlen(key);
    char *save = NULL;
    for (char *tok = strtok_r(body, "&", &save); tok; tok = strtok_r(NULL, "&", &save)) {
        if (strncmp(tok, key, key_len) == 0 && tok[key_len] == '=') {
            snprintf(out, out_len, "%s", tok + key_len + 1);
            url_decode(out);
            return true;
        }
    }
    return false;
}

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t len)
{
    size_t want = req->content_len;
    if (want >= len) {
        want = len - 1;
    }
    size_t got = 0;
    while (got < want) {
        int r = httpd_req_recv(req, buf + got, want - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    buf[got] = '\0';
    return ESP_OK;
}

static esp_err_t redirect_home(httpd_req_t *req)
{
    set_dynamic_headers(req);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static void html_escape(const char *in, char *out, size_t out_len)
{
    size_t used = 0;
    for (; in && *in && used + 6 < out_len; ++in) {
        const char *rep = NULL;
        if (*in == '&') rep = "&amp;";
        else if (*in == '<') rep = "&lt;";
        else if (*in == '>') rep = "&gt;";
        else if (*in == '"') rep = "&quot;";
        if (rep) {
            size_t n = strlen(rep);
            memcpy(out + used, rep, n);
            used += n;
        } else {
            out[used++] = *in;
        }
    }
    out[used] = '\0';
}

static esp_err_t root_get(httpd_req_t *req)
{
    web_count_request();
    source_config_t cfg;
    source_config_get(&cfg);
    char ssid[80], status[160];
    html_escape(cfg.wifi_ssid[0] ? cfg.wifi_ssid : "(not configured)", ssid, sizeof(ssid));
    html_escape(source_wifi_status_text(), status, sizeof(status));
    char room_left[8] = {0};
    char room_right[8] = {0};
    memcpy(room_left, cfg.room_id, 3);
    memcpy(room_right, cfg.room_id + 4, 4);
    const char *disabled = cfg.locked ? "disabled" : "";

    httpd_resp_set_type(req, "text/html");
    set_dynamic_headers(req);
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>CP Room Source</title><style>"
        "*{box-sizing:border-box}:root{--g:#0a5e33;--gd:#074a28;--gl:#0b6b3a;--lg:#e8f5ee;--bd:#c8ddd0}"
        "body{margin:0;padding:0;font-family:'Segoe UI',system-ui,Arial,sans-serif;background:#f0f5f2;color:#1a2e22}"
        ".top-green-bar{position:relative;z-index:1;min-height:52px;background:linear-gradient(135deg,var(--gd),var(--gl));box-shadow:0 2px 12px #0003}"
        ".hdr-inner{position:relative;min-height:52px;display:flex;align-items:center;padding:4px 12px}"
        ".page-logo{display:block;width:168px;height:43px;object-fit:contain;flex:0 0 auto}"
        ".title-block{position:absolute;left:50%;top:50%;transform:translate(-50%,-50%);text-align:center;white-space:nowrap}"
        ".top-green-bar h1{font-size:20px;color:#fff;margin:0;font-weight:700;letter-spacing:.3px}"
        ".top-green-bar .sub{font-size:12px;color:rgba(255,255,255,.75);margin-top:2px}"
        ".wrap{max-width:960px;margin:0 auto;padding:20px 16px}"
        ".status-bar{background:var(--lg);border:1px solid var(--bd);border-radius:12px;padding:16px 20px;margin-bottom:20px;display:flex;flex-wrap:wrap;gap:20px;align-items:center}"
        ".status-bar .pill{display:inline-block;background:var(--g);color:#fff;border-radius:99px;padding:5px 14px;font-weight:700;font-size:13px;text-transform:uppercase;letter-spacing:.5px}"
        ".status-bar .info{font-size:13px;color:#2a4a35;line-height:1.6}.info b{color:var(--gd)}"
        ".card{background:#fff;border:1px solid var(--bd);border-radius:12px;padding:20px 22px;margin-bottom:16px;box-shadow:0 1px 6px #0001}"
        ".card h2{font-size:16px;color:var(--g);margin:0 0 14px;padding-bottom:10px;border-bottom:2px solid var(--lg)}"
        "label{display:block;font-weight:600;margin:10px 0 4px;font-size:14px;color:#2a4a35}"
        "input{width:100%;padding:9px 12px;border:1px solid var(--bd);border-radius:8px;font-size:15px;transition:border .15s}"
        "input:focus{outline:none;border-color:var(--g);box-shadow:0 0 0 3px rgba(10,94,51,.12)}"
        "input:disabled{background:#f5f8f6;color:#8a9e90}"
        ".room-pair{display:grid;grid-template-columns:1fr auto 1.2fr;gap:8px;align-items:center}"
        ".room-dash{font-weight:800;color:var(--g);font-size:20px;text-align:center}"
        "button{margin-top:10px;background:var(--g);color:#fff;border:0;border-radius:8px;padding:9px 18px;font-weight:700;font-size:14px;cursor:pointer;transition:background .15s}"
        "button:hover{background:var(--gd)}button:disabled{background:#9ab8a5;cursor:default}"
        ".btn-outline{background:transparent;color:var(--g);border:2px solid var(--g)}.btn-outline:hover{background:var(--lg)}"
        ".btn-warn{background:#c9372c}.btn-warn:hover{background:#a52e24}"
        ".row{display:grid;grid-template-columns:1fr 1fr;gap:16px}"
        "@media(max-width:600px){.row{grid-template-columns:1fr}}"
        ".note{font-size:13px;color:#7a8f80;margin:6px 0 0;line-height:1.5}"
        "</style></head><body>"
        "<div class=top-green-bar><div class=hdr-inner>"
        "<img class=page-logo src=/CP_logo_rev.png width=168 height=43 alt='Cal Poly'>"
        "<div class=title-block><h1>CP Room Source</h1>"
        "<div class=sub>ESP-NOW + UDP Audio Transmitter</div></div></div></div><div class=wrap>");

    char chunk[1024];
    const char *lock_icon = cfg.locked ? "&#128274;" : "&#128275;";
    snprintf(chunk, sizeof(chunk),
             "<div class=status-bar><span class=pill>%s</span>"
             "<div class=info>Wi-Fi: <b>%s</b> &middot; Channel: <b>%u</b> &middot; "
             "Multicast UDP (optional): <b>239.10.10.10:%u</b> &middot; %s <b>%s</b></div></div>",
             status, ssid, source_wifi_current_channel(), cfg.udp_port,
             lock_icon, cfg.locked ? "Locked" : "Unlocked");
    httpd_resp_sendstr_chunk(req, chunk);

    if (cfg.locked) {
        httpd_resp_sendstr_chunk(req,
            "<div class=card><h2>Unlock Settings</h2><form method=post action=/unlock>"
            "<label>Password</label><input name=password type=password placeholder='Enter password to unlock' required>"
            "<button>Unlock</button></form></div>");
    } else if (cfg.password_set) {
        httpd_resp_sendstr_chunk(req,
            "<div class=card style='text-align:center;padding:14px'>"
            "<form method=post action=/lock><button class=btn-warn>Lock Settings Now</button></form></div>");
    }

    snprintf(chunk, sizeof(chunk),
             "<div class=card><h2>Room &amp; Audio</h2><div class=row>"
             "<div><form method=post action=/set_room>"
             "<label>Room Number</label><div class=room-pair>"
             "<input name=room_a maxlength=3 value='%.3s' placeholder='XXX' %s>"
             "<span class=room-dash>-</span>"
             "<input name=room_b maxlength=4 value='%.4s' placeholder='XXXX' %s>"
             "</div><p class=note>Format: XXX-XXXX (hyphen is automatic)</p>"
             "<button %s>Save Room</button></form></div>"
             "<div><form method=post action=/set_gain>"
             "<label>ADC Gain / Volume (dB)</label><input name=gain value='%.1f' placeholder='0.0' %s>"
             "<p class=note>Range: -24.0 to +24.0 dB</p>"
             "<button %s>Save Gain</button></form></div></div></div>",
             room_left, disabled, room_right, disabled, disabled,
             cfg.gain_db_x10 / 10.0, disabled, disabled);
    httpd_resp_sendstr_chunk(req, chunk);

    snprintf(chunk, sizeof(chunk),
             "<div class=card><h2>Wi-Fi &amp; UDP</h2>"
             "<p class=note>The source follows the router/hotspot channel. The sink scans channels 1, 6, and 11 to find it.</p>"
             "<div class=row>"
             "<div><form method=post action=/set_wifi><label>Wi-Fi SSID</label>"
             "<input name=ssid placeholder='Network name' %s>"
             "<label>Password</label><input name=pass type=password placeholder='Network password' %s>"
             "<button %s>Save Wi-Fi</button></form></div>"
             "<div><form method=post action=/set_udp>"
             "<label>UDP Port</label><input name=port type=number min=1 max=65535 value='%u' %s>"
             "<p class=note>239.10.10.10 is optional LAN fanout. Windows pairing uses unicast UDP port 46000 to this device; after Connect, audio is sent to your PC's IP (not this multicast).</p>"
             "<button %s>Save UDP Port</button></form></div></div></div>",
             disabled, disabled, disabled, cfg.udp_port, disabled, disabled);
    httpd_resp_sendstr_chunk(req, chunk);

    httpd_resp_sendstr_chunk(req,
        "<div class=card><h2>Settings Lock</h2>"
        "<p class=note>Set a password to prevent unauthorized changes. Takes effect immediately.</p>"
        "<form method=post action=/set_password>"
        "<label>New Password</label><input name=password type=password minlength=4 placeholder='Minimum 4 characters' required>"
        "<button>Set Password &amp; Lock</button></form></div>");

    char pin[7] = {0};
    bool pairing = source_udp_control_get_pairing_pin(pin, NULL);
    snprintf(chunk, sizeof(chunk),
             "<div class=card><h2>Windows Receiver Pairing</h2>"
             "<p class=note>Pairing is always available. The 6-digit PIN is fixed from the room number.</p>"
             "<div class=status-bar style='margin-bottom:12px'><span class=pill>%s</span>"
             "<div class=info>PIN: <b>%s</b> &middot; Availability: <b>Always on</b></div></div></div>",
             pairing ? "PAIRING ON" : "PAIRING OFF",
             pairing ? pin : "------");
    httpd_resp_sendstr_chunk(req, chunk);

    httpd_resp_sendstr_chunk(req,
        "<div class=card><h2>Authorized Windows Clients</h2>"
        "<p class=note>Each Windows device has its own key. Revoking one client does not affect others.</p>");
    source_authorized_client_view_t clients[SOURCE_UDP_MAX_AUTH_CLIENTS];
    int client_count = source_udp_control_get_authorized_clients(clients, SOURCE_UDP_MAX_AUTH_CLIENTS);
    if (client_count == 0) {
        httpd_resp_sendstr_chunk(req, "<p class=note>No Windows clients paired yet.</p>");
    } else {
        for (int i = 0; i < client_count; ++i) {
            char id_short[13];
            snprintf(id_short, sizeof(id_short), "%02X%02X%02X%02X%02X%02X",
                     clients[i].client_id[0], clients[i].client_id[1], clients[i].client_id[2],
                     clients[i].client_id[3], clients[i].client_id[4], clients[i].client_id[5]);
            snprintf(chunk, sizeof(chunk),
                     "<div class=status-bar style='margin-bottom:8px'><span class=pill>%s</span>"
                     "<div class=info><b>%s</b> &middot; ID: <b>%s</b> &middot; Last seen: <b>%" PRIu32 " ms</b></div></div>",
                     clients[i].enabled ? "ENABLED" : "DISABLED",
                     clients[i].client_name[0] ? clients[i].client_name : "Windows Receiver",
                     id_short, clients[i].last_seen_ms);
            httpd_resp_sendstr_chunk(req, chunk);
        }
    }
    httpd_resp_sendstr_chunk(req, "</div>");
    if (!cfg.locked && cfg.password_set) {
        httpd_resp_sendstr_chunk(req,
            "<script>"
            "(()=>{let t;const lock=()=>fetch('/lock',{method:'POST',keepalive:true}).then(()=>location.reload()).catch(()=>{});"
            "const reset=()=>{clearTimeout(t);t=setTimeout(lock,10000)};"
            "['input','change','keydown','click'].forEach(e=>document.addEventListener(e,reset,{passive:true}));"
            "reset();})();"
            "</script>");
    }
    httpd_resp_sendstr_chunk(req, "</div></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t logo_get(httpd_req_t *req)
{
    web_count_request();
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
    set_close_header(req);
    return httpd_resp_send(req,
                           reinterpret_cast<const char *>(cp_logo_png),
                           cp_logo_png_len);
}

static esp_err_t status_get(httpd_req_t *req)
{
    web_count_request();

    int64_t now_us = esp_timer_get_time();
    if (now_us - s_status_last_us < 1000000) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_set_type(req, "text/plain");
        set_dynamic_headers(req);
        return httpd_resp_sendstr(req, "status polling limited to 1 Hz");
    }
    s_status_last_us = now_us;

    source_udp_stats_t udp = {0};
    source_audio_stats_t audio = {0};
    source_udp_control_stats_t ctrl = {0};
    source_udp_stream_get_stats(&udp);
    source_audio_get_stats(&audio);
    source_udp_control_get_stats(&ctrl);

    wifi_ap_record_t ap = {0};
    int rssi = 0;
    if (source_wifi_is_sta_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }

    char json[768];
    int n = snprintf(json, sizeof(json),
                     "{\"udp_sent\":%" PRIu32 ",\"udp_q\":%" PRIu32
                     ",\"udp_q_drop\":%" PRIu32 ",\"udp_err\":%" PRIu32
                     ",\"espnow_frag\":%" PRIu32 ",\"espnow_frames\":%" PRIu32
                     ",\"espnow_q\":%" PRIu32 ",\"espnow_q_drop\":%" PRIu32
                     ",\"espnow_frag_drop\":%" PRIu32 ",\"espnow_err\":%" PRIu32
                     ",\"web_req\":%" PRIu32 ",\"rssi\":%d,\"heap\":%u"
                     ",\"internal\":%u,\"deadline_miss\":%" PRIu32
                     ",\"alloc_fail\":%" PRIu32
                     ",\"room_advertisements\":%" PRIu32
                     ",\"pair_ok\":%" PRIu32 ",\"pair_reject\":%" PRIu32
                     ",\"join_ok\":%" PRIu32 ",\"join_reject\":%" PRIu32
                     ",\"auth_fail\":%" PRIu32 ",\"active_udp_clients\":%" PRIu32 "}",
                     udp.packets_sent, udp.queue_level, udp.queue_drops, udp.send_errors,
                     audio.espnow_fragments_sent, audio.espnow_frames_sent,
                     audio.espnow_queue_level, audio.espnow_queue_drops,
                     audio.espnow_fragment_drops, audio.espnow_send_errors,
                     s_web_requests, rssi, (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     audio.audio_scheduler_deadline_misses, audio.allocation_failures,
                     ctrl.advertisements_sent, ctrl.pair_accepts, ctrl.pair_rejects,
                     ctrl.join_accepts, ctrl.join_rejects, ctrl.auth_failures,
                     ctrl.active_clients);
    if (n < 0) {
        return ESP_FAIL;
    }
    if ((size_t)n >= sizeof(json)) {
        n = sizeof(json) - 1;
    }

    httpd_resp_set_type(req, "application/json");
    set_dynamic_headers(req);
    return httpd_resp_send(req, json, n);
}

static esp_err_t post_unlock(httpd_req_t *req)
{
    web_count_request();
    char body[160], pass[96], tmp[160];
    if (read_body(req, body, sizeof(body)) == ESP_OK) {
        snprintf(tmp, sizeof(tmp), "%s", body);
        if (form_get(tmp, "password", pass, sizeof(pass))) {
            (void)source_config_unlock(pass);
        }
    }
    return redirect_home(req);
}

static esp_err_t post_lock(httpd_req_t *req)
{
    web_count_request();
    (void)req;
    source_config_lock();
    return redirect_home(req);
}

static esp_err_t post_set_room(httpd_req_t *req)
{
    web_count_request();
    char body[160], room[32], room_a[8], room_b[8], tmp[160];
    if (read_body(req, body, sizeof(body)) == ESP_OK && !source_config_is_locked()) {
        snprintf(tmp, sizeof(tmp), "%s", body);
        bool has_room = form_get(tmp, "room", room, sizeof(room));
        snprintf(tmp, sizeof(tmp), "%s", body);
        bool has_a = form_get(tmp, "room_a", room_a, sizeof(room_a));
        snprintf(tmp, sizeof(tmp), "%s", body);
        bool has_b = form_get(tmp, "room_b", room_b, sizeof(room_b));
        if (has_a && has_b) {
            snprintf(room, sizeof(room), "%.3s-%.4s", room_a, room_b);
            (void)source_config_set_room_id(room);
        } else if (has_room) {
            (void)source_config_set_room_id(room);
        }
    }
    return redirect_home(req);
}

static esp_err_t post_set_gain(httpd_req_t *req)
{
    web_count_request();
    char body[160], gain[32], tmp[160];
    if (read_body(req, body, sizeof(body)) == ESP_OK && !source_config_is_locked()) {
        snprintf(tmp, sizeof(tmp), "%s", body);
        if (form_get(tmp, "gain", gain, sizeof(gain))) {
            int gain_x10 = (int)(strtod(gain, NULL) * 10.0);
            (void)source_config_set_gain_db_x10(gain_x10);
        }
    }
    return redirect_home(req);
}

static esp_err_t post_set_udp(httpd_req_t *req)
{
    web_count_request();
    char body[160], port[32], tmp[160];
    if (read_body(req, body, sizeof(body)) == ESP_OK && !source_config_is_locked()) {
        snprintf(tmp, sizeof(tmp), "%s", body);
        if (form_get(tmp, "port", port, sizeof(port))) {
            int p = atoi(port);
            if (p > 0 && p <= 65535) {
                (void)source_config_set_udp_port((uint16_t)p);
            }
        }
    }
    return redirect_home(req);
}

static esp_err_t post_set_wifi(httpd_req_t *req)
{
    web_count_request();
    char body[256], ssid[64], pass[96], tmp[256], msg[128];
    if (read_body(req, body, sizeof(body)) == ESP_OK && !source_config_is_locked()) {
        snprintf(tmp, sizeof(tmp), "%s", body);
        bool has_ssid = form_get(tmp, "ssid", ssid, sizeof(ssid));
        snprintf(tmp, sizeof(tmp), "%s", body);
        bool has_pass = form_get(tmp, "pass", pass, sizeof(pass));
        if (has_ssid) {
            esp_err_t err = source_wifi_save_credentials_if_channel_ok(ssid, has_pass ? pass : "",
                                                                       msg, sizeof(msg));
            if (err == ESP_ERR_INVALID_STATE) {
                httpd_resp_set_status(req, "409 Conflict");
                httpd_resp_set_type(req, "text/plain");
                set_dynamic_headers(req);
                return httpd_resp_sendstr(req, msg[0] ? msg : "busy streaming");
            }
            ESP_LOGI(TAG, "Wi-Fi config result: %s", msg);
        }
    }
    return redirect_home(req);
}

static esp_err_t post_set_password(httpd_req_t *req)
{
    web_count_request();
    char body[160], pass[96], tmp[160];
    if (read_body(req, body, sizeof(body)) == ESP_OK) {
        snprintf(tmp, sizeof(tmp), "%s", body);
        if (form_get(tmp, "password", pass, sizeof(pass))) {
            (void)source_config_set_password(pass);
        }
    }
    return redirect_home(req);
}

static esp_err_t post_pairing_start(httpd_req_t *req)
{
    web_count_request();
    if (!source_config_is_locked()) {
        (void)source_udp_control_enable_pairing(60000);
    }
    return redirect_home(req);
}

static void reg_uri(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t u = {};
    u.uri = uri;
    u.method = method;
    u.handler = handler;
    u.user_ctx = NULL;
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &u));
}

esp_err_t source_web_start(void)
{
    if (s_httpd) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.task_priority = 2;
    cfg.core_id = 0;
    cfg.stack_size = 6144;
    cfg.max_open_sockets = 2;
    cfg.backlog_conn = 1;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 1;
    cfg.send_wait_timeout = 1;
    cfg.max_uri_handlers = 11;
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &cfg), TAG, "start httpd");

    reg_uri("/", HTTP_GET, root_get);
    reg_uri("/CP_logo_rev.png", HTTP_GET, logo_get);
    reg_uri("/status", HTTP_GET, status_get);
    reg_uri("/unlock", HTTP_POST, post_unlock);
    reg_uri("/lock", HTTP_POST, post_lock);
    reg_uri("/set_room", HTTP_POST, post_set_room);
    reg_uri("/set_gain", HTTP_POST, post_set_gain);
    reg_uri("/set_udp", HTTP_POST, post_set_udp);
    reg_uri("/set_wifi", HTTP_POST, post_set_wifi);
    reg_uri("/set_password", HTTP_POST, post_set_password);
    reg_uri("/pairing_start", HTTP_POST, post_pairing_start);
    ESP_LOGI(TAG, "Web UI started");
    return ESP_OK;
}
