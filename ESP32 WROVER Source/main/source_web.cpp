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
#include "cp_logo_png.hpp"
#include "source_audio_stats.h"
#include "source_config.h"
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

static void set_static_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
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

/* ────────────────────────────────────────────────────────────────────
 *  Stylesheet (cached, served once per device, ~5 KB)
 *  Uses CSS custom properties so light/dark are pure theme swaps:
 *  no JS needed for repainting and every element automatically
 *  inverts when --bg / --text / --border tokens change.
 * ──────────────────────────────────────────────────────────────────── */
static const char STYLE_CSS[] =
"*,*::before,*::after{box-sizing:border-box}"
"html{color-scheme:light dark}"
":root{"
"--bg:#f1f7f3;--surface:#ffffff;--surface-2:#eaf3ee;--surface-3:#dfece4;"
"--border:#cfe1d6;--border-strong:#a9c8b6;"
"--text:#0f221a;--text-muted:#506a5b;--text-faint:#7c9285;"
"--primary:#0a5e33;--primary-2:#0b6b3a;--primary-dark:#063e22;"
"--primary-soft:#dff0e6;--primary-on:#ffffff;"
"--danger:#c9372c;--danger-hover:#a52e24;--danger-soft:#fdecea;"
"--accent:#0f8a50;"
"--ring:rgba(10,94,51,.18);"
"--shadow-sm:0 1px 2px rgba(8,40,24,.06),0 1px 3px rgba(8,40,24,.08);"
"--shadow-md:0 4px 16px rgba(8,40,24,.08);"
"--shadow-lg:0 16px 40px rgba(8,40,24,.14);"
"--bar-grad:linear-gradient(135deg,#063e22,#0b6b3a);"
"}"
"[data-theme=dark]{"
"--bg:#06120d;--surface:#0f1f18;--surface-2:#162a21;--surface-3:#1e3a2e;"
"--border:#26443a;--border-strong:#37614f;"
"--text:#e9f3ec;--text-muted:#a3b8ab;--text-faint:#7a8f82;"
"--primary:#2db272;--primary-2:#38c47f;--primary-dark:#1f9c60;"
"--primary-soft:#152a20;--primary-on:#06120d;"
"--danger:#ef5b50;--danger-hover:#dd4a3f;--danger-soft:#2a1715;"
"--accent:#3fd089;"
"--ring:rgba(45,178,114,.28);"
"--shadow-sm:0 1px 2px rgba(0,0,0,.4);"
"--shadow-md:0 6px 24px rgba(0,0,0,.45);"
"--shadow-lg:0 18px 50px rgba(0,0,0,.55);"
"--bar-grad:linear-gradient(135deg,#04150c,#0a3a23);"
"}"
"html,body{margin:0;padding:0}"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Inter,system-ui,Arial,sans-serif;"
"background:var(--bg);color:var(--text);min-height:100vh;"
"transition:background .35s ease,color .35s ease;-webkit-font-smoothing:antialiased}"
".bar{position:sticky;top:0;z-index:50;background:var(--bar-grad);"
"box-shadow:0 4px 18px rgba(0,0,0,.18);backdrop-filter:saturate(160%) blur(8px)}"
".bar-inner{max-width:1080px;margin:0 auto;padding:10px 18px;display:flex;align-items:center;gap:14px;min-height:60px}"
".bar-logo{flex:0 0 auto;width:148px;height:38px;object-fit:contain;filter:drop-shadow(0 1px 2px rgba(0,0,0,.3))}"
".bar-title{flex:1;min-width:0;text-align:center;color:#fff}"
".bar-title h1{margin:0;font-size:18px;font-weight:700;letter-spacing:.4px;line-height:1.2}"
".bar-title .sub{margin-top:2px;font-size:11px;font-weight:500;color:rgba(255,255,255,.72);text-transform:uppercase;letter-spacing:1.5px}"
".theme-tog{position:relative;flex:0 0 auto;width:42px;height:42px;border-radius:50%;"
"background:rgba(255,255,255,.14);border:1px solid rgba(255,255,255,.28);"
"color:#fff;cursor:pointer;display:flex;align-items:center;justify-content:center;"
"overflow:hidden;transition:background .2s ease,transform .2s ease,box-shadow .2s ease;"
"-webkit-tap-highlight-color:transparent}"
".theme-tog:hover{background:rgba(255,255,255,.24);transform:scale(1.06)}"
".theme-tog:focus-visible{outline:none;box-shadow:0 0 0 3px rgba(255,255,255,.45)}"
".theme-tog svg{position:absolute;width:22px;height:22px;transition:opacity .4s ease,transform .55s cubic-bezier(.4,.2,.2,1)}"
"[data-theme=light] .theme-tog .sun{opacity:1;transform:rotate(0) scale(1)}"
"[data-theme=light] .theme-tog .moon{opacity:0;transform:rotate(-90deg) scale(.4)}"
"[data-theme=dark]  .theme-tog .sun{opacity:0;transform:rotate(90deg) scale(.4)}"
"[data-theme=dark]  .theme-tog .moon{opacity:1;transform:rotate(0) scale(1)}"
".wrap{max-width:1080px;margin:0 auto;padding:24px 16px 56px}"
".hero{display:grid;grid-template-columns:1fr;gap:14px;margin-bottom:22px}"
"@media(min-width:760px){.hero{grid-template-columns:1.4fr 1fr}}"
".status{background:var(--surface);border:1px solid var(--border);border-radius:18px;"
"padding:18px 20px;box-shadow:var(--shadow-sm);display:flex;align-items:center;gap:16px}"
".dot{width:12px;height:12px;border-radius:50%;flex:0 0 auto;background:var(--accent);"
"box-shadow:0 0 0 4px var(--primary-soft);animation:pulse 2.4s ease-in-out infinite}"
".status .dot.warn{background:#d9a800;box-shadow:0 0 0 4px rgba(217,168,0,.18);animation:none}"
".status .dot.err{background:var(--danger);box-shadow:0 0 0 4px var(--danger-soft);animation:none}"
"@keyframes pulse{0%,100%{box-shadow:0 0 0 4px var(--primary-soft)}50%{box-shadow:0 0 0 10px transparent}}"
".status-text{min-width:0;flex:1}"
".status-text .label{font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:1.5px;color:var(--text-muted)}"
".status-text .value{margin-top:2px;font-size:17px;font-weight:700;color:var(--text);word-break:break-word}"
".kpis{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}"
".kpi{background:var(--surface);border:1px solid var(--border);border-radius:14px;padding:12px 14px;box-shadow:var(--shadow-sm)}"
".kpi .l{font-size:10px;font-weight:700;text-transform:uppercase;letter-spacing:1.3px;color:var(--text-muted)}"
".kpi .v{margin-top:4px;font-size:18px;font-weight:700;color:var(--text);font-variant-numeric:tabular-nums}"
".kpi .v small{font-size:11px;color:var(--text-faint);font-weight:600;margin-left:4px}"
".card{background:var(--surface);border:1px solid var(--border);border-radius:18px;"
"padding:22px 24px;margin-bottom:16px;box-shadow:var(--shadow-sm);"
"transition:border-color .25s ease,box-shadow .25s ease}"
".card:hover{border-color:var(--border-strong);box-shadow:var(--shadow-md)}"
".card-hd{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:6px}"
".card h2{margin:0;font-size:15px;font-weight:700;color:var(--primary);text-transform:uppercase;letter-spacing:1.4px}"
".card .desc{margin:0 0 16px;font-size:13px;color:var(--text-muted);line-height:1.55}"
".badge{display:inline-flex;align-items:center;gap:6px;padding:4px 10px;border-radius:99px;"
"font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:1px;"
"background:var(--primary-soft);color:var(--primary)}"
".badge.lock{background:var(--danger-soft);color:var(--danger)}"
".row{display:grid;grid-template-columns:1fr 1fr;gap:18px}"
"@media(max-width:640px){.row{grid-template-columns:1fr}}"
".field{display:flex;flex-direction:column;gap:6px;margin-bottom:14px}"
".field label{font-size:12px;font-weight:700;color:var(--text-muted);text-transform:uppercase;letter-spacing:1.2px}"
"input,select{appearance:none;-webkit-appearance:none;width:100%;padding:11px 14px;"
"font-size:15px;font-family:inherit;color:var(--text);background:var(--surface-2);"
"border:1px solid var(--border);border-radius:10px;"
"transition:border .15s ease,box-shadow .15s ease,background .25s ease}"
"input::placeholder{color:var(--text-faint)}"
"input:focus,select:focus{outline:none;border-color:var(--primary);box-shadow:0 0 0 4px var(--ring);background:var(--surface)}"
"input:disabled{opacity:.55;cursor:not-allowed}"
".room{display:grid;grid-template-columns:1fr auto 1.3fr;gap:10px;align-items:center}"
".room .sep{font-size:22px;font-weight:800;color:var(--primary);text-align:center;line-height:1}"
".note{margin:6px 0 0;font-size:12px;color:var(--text-faint);line-height:1.5}"
".btn{display:inline-flex;align-items:center;justify-content:center;gap:8px;"
"margin-top:10px;padding:11px 22px;border:0;border-radius:10px;cursor:pointer;"
"font-family:inherit;font-size:14px;font-weight:700;letter-spacing:.3px;"
"background:var(--primary);color:var(--primary-on);"
"transition:background .15s ease,transform .1s ease,box-shadow .15s ease;"
"-webkit-tap-highlight-color:transparent}"
".btn:hover{background:var(--primary-2);box-shadow:0 4px 14px var(--ring)}"
".btn:active{transform:translateY(1px)}"
".btn:disabled{background:var(--surface-3);color:var(--text-faint);cursor:not-allowed;box-shadow:none}"
".btn.ghost{background:transparent;color:var(--primary);border:1.5px solid var(--primary)}"
".btn.ghost:hover{background:var(--primary-soft);box-shadow:none}"
".btn.danger{background:var(--danger)}"
".btn.danger:hover{background:var(--danger-hover);box-shadow:0 4px 14px rgba(201,55,44,.3)}"
".btn-row{display:flex;justify-content:flex-end;gap:10px;margin-top:8px}"
".grid-stats{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px}"
"@media(max-width:520px){.grid-stats{grid-template-columns:repeat(2,minmax(0,1fr))}}"
".stat{background:var(--surface-2);border:1px solid var(--border);border-radius:12px;padding:10px 12px}"
".stat .l{font-size:10px;font-weight:700;text-transform:uppercase;letter-spacing:1.2px;color:var(--text-muted)}"
".stat .v{margin-top:3px;font-size:16px;font-weight:700;color:var(--text);font-variant-numeric:tabular-nums}"
".foot{margin-top:32px;text-align:center;font-size:11px;color:var(--text-faint);letter-spacing:.8px}"
;

/* ────────────────────────────────────────────────────────────────────
 *  Page script: theme toggle (persists in localStorage and reads
 *  prefers-color-scheme on first load) + status auto-refresh.
 *  Kept tiny — no frameworks, no dependencies.
 * ──────────────────────────────────────────────────────────────────── */
static const char APP_JS[] =
"(()=>{"
"const root=document.documentElement;"
"function apply(t){root.setAttribute('data-theme',t);try{localStorage.setItem('cp-theme',t)}catch(e){}}"
"const btn=document.getElementById('themeTog');"
"if(btn){btn.addEventListener('click',()=>{const cur=root.getAttribute('data-theme')==='dark'?'light':'dark';apply(cur);btn.setAttribute('aria-label',cur==='dark'?'Switch to light mode':'Switch to dark mode')})}"
"async function poll(){try{"
"const r=await fetch('/status',{cache:'no-store'});if(!r.ok)return;"
"const j=await r.json();"
"const fmt=n=>n>=1000?(n/1000).toFixed(1)+'k':String(n);"
"const set=(id,v)=>{const el=document.getElementById(id);if(el)el.textContent=v};"
"set('k-frames',fmt(j.espnow_frames||0));"
"set('k-frags',fmt(j.espnow_frag||0));"
"set('k-q',String(j.espnow_q||0));"
"set('k-drop',fmt((j.espnow_q_drop||0)+(j.espnow_frag_drop||0)));"
"set('k-err',fmt(j.espnow_err||0));"
"set('k-miss',fmt(j.deadline_miss||0));"
"set('k-rssi',j.rssi?(j.rssi+' dBm'):'—');"
"set('k-heap',Math.round((j.heap||0)/1024)+' KB');"
"const dot=document.getElementById('dot');"
"if(dot){const ok=(j.espnow_frag||0)>0&&(j.deadline_miss||0)===0;"
"dot.className='dot '+(ok?'':(j.deadline_miss>5?'err':'warn'))}"
"}catch(e){}}"
"poll();setInterval(poll,2000);"
"})();"
;

/* Inline shim served in <head> to prevent flash-of-wrong-theme on
 * full page reloads. */
static const char THEME_SHIM[] =
"<script>(function(){try{var t=localStorage.getItem('cp-theme');"
"if(!t)t=(window.matchMedia&&matchMedia('(prefers-color-scheme: dark)').matches)?'dark':'light';"
"document.documentElement.setAttribute('data-theme',t);}catch(e){"
"document.documentElement.setAttribute('data-theme','light');}})();</script>";

static const char SVG_SUN_MOON[] =
"<svg class=sun viewBox='0 0 24 24' fill=none stroke=currentColor stroke-width=2 stroke-linecap=round stroke-linejoin=round aria-hidden=true>"
"<circle cx=12 cy=12 r=4/>"
"<path d='M12 2v2M12 20v2M4.93 4.93l1.41 1.41M17.66 17.66l1.41 1.41M2 12h2M20 12h2M4.93 19.07l1.41-1.41M17.66 6.34l1.41-1.41'/>"
"</svg>"
"<svg class=moon viewBox='0 0 24 24' fill=none stroke=currentColor stroke-width=2 stroke-linecap=round stroke-linejoin=round aria-hidden=true>"
"<path d='M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z'/>"
"</svg>";

static esp_err_t style_get(httpd_req_t *req)
{
    web_count_request();
    httpd_resp_set_type(req, "text/css");
    set_static_headers(req);
    return httpd_resp_send(req, STYLE_CSS, sizeof(STYLE_CSS) - 1);
}

static esp_err_t app_js_get(httpd_req_t *req)
{
    web_count_request();
    httpd_resp_set_type(req, "application/javascript");
    set_static_headers(req);
    return httpd_resp_send(req, APP_JS, sizeof(APP_JS) - 1);
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
        "<!doctype html><html lang=en><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1,viewport-fit=cover'>"
        "<meta name=color-scheme content='light dark'>"
        "<title>CP Room Source</title>"
        "<link rel=stylesheet href=/style.css>"
        );
    httpd_resp_sendstr_chunk(req, THEME_SHIM);
    httpd_resp_sendstr_chunk(req,
        "</head><body>"
        "<header class=bar><div class=bar-inner>"
        "<img class=bar-logo src=/CP_logo_rev.png width=148 height=38 alt='Cal Poly'>"
        "<div class=bar-title><h1>CP Room Source</h1>"
        "<div class=sub>ESP-NOW Audio Transmitter</div></div>"
        "<button id=themeTog class=theme-tog type=button aria-label='Toggle dark mode'>"
        );
    httpd_resp_sendstr_chunk(req, SVG_SUN_MOON);
    httpd_resp_sendstr_chunk(req,
        "</button></div></header>"
        "<main class=wrap>"
        );

    char chunk[1024];
    const char *lock_class = cfg.locked ? "badge lock" : "badge";
    const char *lock_text = cfg.locked ? "Locked" : "Unlocked";
    snprintf(chunk, sizeof(chunk),
        "<section class=hero>"
        "<div class=status>"
        "<span id=dot class=dot></span>"
        "<div class=status-text>"
        "<div class=label>System Status</div>"
        "<div class=value>%s</div>"
        "</div>"
        "<span class='%s'>%s</span>"
        "</div>"
        "<div class=kpis>"
        "<div class=kpi><div class=l>Wi-Fi SSID</div><div class=v>%s</div></div>"
        "<div class=kpi><div class=l>Channel</div><div class=v>%u</div></div>"
        "<div class=kpi><div class=l>Signal</div><div class=v id=k-rssi>—</div></div>"
        "<div class=kpi><div class=l>Free Heap</div><div class=v id=k-heap>—</div></div>"
        "</div>"
        "</section>",
        status, lock_class, lock_text, ssid, source_wifi_current_channel());
    httpd_resp_sendstr_chunk(req, chunk);

    httpd_resp_sendstr_chunk(req,
        "<section class=card>"
        "<div class=card-hd><h2>Live Telemetry</h2><span class=badge>Live</span></div>"
        "<p class=desc>Updates every 2 seconds. Packet drops or deadline misses should remain at zero during normal playback.</p>"
        "<div class=grid-stats>"
        "<div class=stat><div class=l>Frames Sent</div><div class=v id=k-frames>0</div></div>"
        "<div class=stat><div class=l>Fragments</div><div class=v id=k-frags>0</div></div>"
        "<div class=stat><div class=l>Queue Depth</div><div class=v id=k-q>0</div></div>"
        "<div class=stat><div class=l>Drops</div><div class=v id=k-drop>0</div></div>"
        "<div class=stat><div class=l>TX Errors</div><div class=v id=k-err>0</div></div>"
        "<div class=stat><div class=l>Deadline Miss</div><div class=v id=k-miss>0</div></div>"
        "</div></section>"
        );

    if (cfg.locked) {
        httpd_resp_sendstr_chunk(req,
            "<section class=card>"
            "<div class=card-hd><h2>Unlock Settings</h2></div>"
            "<p class=desc>Enter your administrator password to modify device settings.</p>"
            "<form method=post action=/unlock>"
            "<div class=field><label>Password</label>"
            "<input name=password type=password placeholder='Administrator password' required autocomplete=current-password></div>"
            "<div class=btn-row><button class=btn type=submit>Unlock</button></div>"
            "</form></section>");
    } else if (cfg.password_set) {
        httpd_resp_sendstr_chunk(req,
            "<section class=card>"
            "<div class=card-hd><h2>Session</h2><span class=badge>Auto-lock in 10s idle</span></div>"
            "<p class=desc>Settings will automatically re-lock after 10 seconds of inactivity. Lock manually below.</p>"
            "<form method=post action=/lock>"
            "<div class=btn-row><button class='btn danger' type=submit>Lock Now</button></div>"
            "</form></section>");
    }

    snprintf(chunk, sizeof(chunk),
        "<section class=card>"
        "<div class=card-hd><h2>Room &amp; Audio</h2></div>"
        "<p class=desc>The room identifier pairs this transmitter with matching receivers. Gain adjusts the input level before SBC encoding.</p>"
        "<div class=row>"
        "<form method=post action=/set_room>"
        "<div class=field><label>Room Number</label>"
        "<div class=room>"
        "<input name=room_a maxlength=3 value='%.3s' placeholder=XXX %s pattern='[0-9A-Za-z]{1,3}'>"
        "<span class=sep>&minus;</span>"
        "<input name=room_b maxlength=4 value='%.4s' placeholder=XXXX %s pattern='[0-9A-Za-z]{1,4}'>"
        "</div>"
        "<p class=note>Format: XXX-XXXX</p></div>"
        "<div class=btn-row><button class=btn type=submit %s>Save Room</button></div>"
        "</form>"
        "<form method=post action=/set_gain>"
        "<div class=field><label>Input Gain (dB)</label>"
        "<input name=gain value='%.1f' placeholder='0.0' %s>"
        "<p class=note>Range &minus;24.0 to &plus;24.0 dB</p></div>"
        "<div class=btn-row><button class=btn type=submit %s>Save Gain</button></div>"
        "</form>"
        "</div></section>",
        room_left, disabled, room_right, disabled, disabled,
        cfg.gain_db_x10 / 10.0, disabled, disabled);
    httpd_resp_sendstr_chunk(req, chunk);

    snprintf(chunk, sizeof(chunk),
        "<section class=card>"
        "<div class=card-hd><h2>Wi-Fi Network</h2></div>"
        "<p class=desc>Source follows the network channel. Receivers scan channels 1, 6, and 11 to discover it. Saving requires the device to be idle and not currently streaming.</p>"
        "<form method=post action=/set_wifi>"
        "<div class=row>"
        "<div class=field><label>SSID</label>"
        "<input name=ssid placeholder='Network name' %s autocomplete=off></div>"
        "<div class=field><label>Password</label>"
        "<input name=pass type=password placeholder='Network password' %s autocomplete=new-password></div>"
        "</div>"
        "<div class=btn-row><button class=btn type=submit %s>Save Wi-Fi</button></div>"
        "</form></section>",
        disabled, disabled, disabled);
    httpd_resp_sendstr_chunk(req, chunk);

    httpd_resp_sendstr_chunk(req,
        "<section class=card>"
        "<div class=card-hd><h2>Settings Lock</h2></div>"
        "<p class=desc>Set or update the administrator password. Locking prevents accidental changes from unauthorized users.</p>"
        "<form method=post action=/set_password>"
        "<div class=field><label>New Password</label>"
        "<input name=password type=password minlength=4 placeholder='At least 4 characters' required autocomplete=new-password></div>"
        "<div class=btn-row><button class=btn type=submit>Set Password &amp; Lock</button></div>"
        "</form></section>"
        );

    httpd_resp_sendstr_chunk(req,
        "<div class=foot>Cal Poly Room Audio &bull; ESP32 WROVER &bull; SBC 249 kbps &bull; RTN=4</div>"
        "</main>"
        "<script src=/app.js defer></script>"
        );

    if (!cfg.locked && cfg.password_set) {
        httpd_resp_sendstr_chunk(req,
            "<script>"
            "(()=>{let t;const lock=()=>fetch('/lock',{method:'POST',keepalive:true}).then(()=>location.reload()).catch(()=>{});"
            "const reset=()=>{clearTimeout(t);t=setTimeout(lock,10000)};"
            "['input','change','keydown','click','touchstart'].forEach(e=>document.addEventListener(e,reset,{passive:true}));"
            "reset();})();"
            "</script>");
    }

    httpd_resp_sendstr_chunk(req, "</body></html>");
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

    source_audio_stats_t audio = {0};
    source_audio_get_stats(&audio);

    int rssi = source_wifi_is_sta_connected() ? source_wifi_cached_rssi() : 0;

    char json[512];
    int n = snprintf(json, sizeof(json),
                     "{\"espnow_frag\":%" PRIu32 ",\"espnow_frames\":%" PRIu32
                     ",\"espnow_q\":%" PRIu32 ",\"espnow_q_drop\":%" PRIu32
                     ",\"espnow_frag_drop\":%" PRIu32 ",\"espnow_err\":%" PRIu32
                     ",\"web_req\":%" PRIu32 ",\"rssi\":%d,\"heap\":%u"
                     ",\"internal\":%u,\"deadline_miss\":%" PRIu32
                     ",\"alloc_fail\":%" PRIu32 "}",
                     audio.espnow_fragments_sent, audio.espnow_frames_sent,
                     audio.espnow_queue_level, audio.espnow_queue_drops,
                     audio.espnow_fragment_drops, audio.espnow_send_errors,
                     s_web_requests, rssi, (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     audio.audio_scheduler_deadline_misses, audio.allocation_failures);
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
    reg_uri("/style.css", HTTP_GET, style_get);
    reg_uri("/app.js", HTTP_GET, app_js_get);
    reg_uri("/CP_logo_rev.png", HTTP_GET, logo_get);
    reg_uri("/status", HTTP_GET, status_get);
    reg_uri("/unlock", HTTP_POST, post_unlock);
    reg_uri("/lock", HTTP_POST, post_lock);
    reg_uri("/set_room", HTTP_POST, post_set_room);
    reg_uri("/set_gain", HTTP_POST, post_set_gain);
    reg_uri("/set_wifi", HTTP_POST, post_set_wifi);
    reg_uri("/set_password", HTTP_POST, post_set_password);
    ESP_LOGI(TAG, "Web UI started");
    return ESP_OK;
}
