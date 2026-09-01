#include "revlink_onboarding.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "revlink_captive_dns.h"
#include "revlink_credentials.h"
#include "revlink_network_runtime.h"
#include "revlink_portal.h"
#include "revlink_sidecar_identity.h"
#include "revlink_status_oled.h"
#include "revlink_wifi_radio.h"

#define ONBOARDING_BODY_LIMIT 512U
#define ONBOARDING_TASK_STACK 4096U
#define CAPTIVE_DNS_TASK_STACK 4096U
#define CAPTIVE_DNS_PACKET_LIMIT 512U
#define LOCAL_IDENTITY_CAPACITY 33U
#define SETUP_PASSWORD_LENGTH 8U
#define ONBOARDING_DISPLAY_TASK_STACK 2048U
#define ONBOARDING_DISPLAY_PERIOD_MS 250U
#define ONBOARDING_SUCCESS_HOLD_MS 8000U
#define ONBOARDING_DIRECT_CLOSE_SECONDS 5U

static const char *TAG = "revlink_onboarding";
static httpd_handle_t onboarding_server;

typedef enum {
    ONBOARDING_IDLE = 0,
    ONBOARDING_CONNECTING,
    ONBOARDING_FAILED,
    ONBOARDING_SUCCEEDED,
    ONBOARDING_DIRECT,
} onboarding_state_t;

typedef struct {
    onboarding_state_t state;
    esp_err_t last_error;
    int64_t close_deadline_us;
} onboarding_status_t;

static portMUX_TYPE onboarding_lock = portMUX_INITIALIZER_UNLOCKED;
static onboarding_status_t onboarding_status;

static esp_err_t local_identity(
    char *ssid,
    size_t ssid_capacity,
    char *hostname,
    size_t hostname_capacity
)
{
    revlink_sidecar_identity_t identity = {0};
    const esp_err_t status =
        revlink_sidecar_identity_snapshot(&identity);
    if (status != ESP_OK) {
        return status;
    }
    const int ssid_length =
        snprintf(ssid, ssid_capacity, "%s", identity.ssid);
    const int hostname_length =
        snprintf(hostname, hostname_capacity, "%s", identity.hostname);
    return ssid_length > 0
        && hostname_length > 0
        && (size_t)ssid_length < ssid_capacity
        && (size_t)hostname_length < hostname_capacity
        ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static const char ONBOARDING_PAGE[] =
    "<!doctype html><html lang=en><head>"
    "<meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<meta name=color-scheme content=dark>"
    "<title>RevLink setup</title>"
    "<style>"
    ":root{font-family:Inter,ui-sans-serif,system-ui,-apple-system,sans-serif;"
    "color:#f6f8ff;background:#050912}*{box-sizing:border-box}"
    "body{margin:0;min-height:100vh;display:grid;place-items:center;padding:18px;"
    "background:radial-gradient(circle at 50% 0,#12305e 0,#08111f 38%,#050912 75%)}"
    "main{width:min(100%,560px);padding:28px;border:1px solid #29415f;"
    "border-radius:24px;background:#0c1421e8;box-shadow:0 28px 80px #0009}"
    ".brand{display:flex;align-items:center;gap:12px;font-weight:800;"
    "letter-spacing:.12em}.mark{display:grid;place-items:center;width:42px;"
    "height:42px;border-radius:14px;background:linear-gradient(145deg,#56b8ff,#3767ff);"
    "font-size:22px}.pill{margin-left:auto;padding:6px 9px;border-radius:99px;"
    "background:#102742;color:#7bc4ff;font-size:10px;letter-spacing:.08em}"
    "h1{font-size:30px;line-height:1.1;margin:25px 0 9px}"
    "p{color:#9eacc1;line-height:1.5;margin:0 0 20px}"
    ".card{padding:18px;border:1px solid #263a53;border-radius:17px;"
    "background:#0a121e}.card+.card{margin-top:13px}.title{font-weight:800}"
    ".copy{font-size:13px;color:#8798af;line-height:1.45;margin-top:5px}"
    "label{display:block;color:#c8d3e3;font-size:13px;font-weight:700;"
    "margin:15px 0 8px}input{width:100%;padding:14px 15px;border-radius:13px;"
    "border:1px solid #2b405b;background:#07101b;color:#fff;font:inherit;"
    "outline:none}input:focus{border-color:#55aaff;box-shadow:0 0 0 3px #2a83ff2b}"
    ".password-wrap{position:relative}.password-wrap input{padding-right:72px}"
    ".reveal{position:absolute;right:7px;top:50%;transform:translateY(-50%);width:auto;"
    "padding:8px 10px;border-radius:9px;background:#152238;border:1px solid #304762;"
    "color:#8dc8ff;font-size:12px}.reveal:focus-visible{outline:2px solid #55aaff;"
    "outline-offset:2px}"
    "button{width:100%;padding:14px;border:0;border-radius:13px;"
    "background:linear-gradient(135deg,#4db8ff,#4268ff);color:white;font:inherit;"
    "font-weight:800;cursor:pointer}.primary{margin-top:17px}.secondary{margin-top:14px;"
    "background:#152238;border:1px solid #304762}.link{padding:8px 0;margin-top:8px;"
    "width:auto;background:none;color:#78bfff;text-align:left;font-size:13px}"
    ".networks{display:grid;gap:8px;margin-top:14px}.network{display:flex;"
    "align-items:center;gap:10px;text-align:left;background:#101c2b;border:1px solid #263b55;"
    "font-weight:650}.network.selected{border-color:#57afff;background:#142b45}"
    ".signal{margin-left:auto;color:#7d90a9;font-size:12px}.hidden{display:none}"
    ".note{margin-top:12px;padding:10px 12px;border-radius:11px;background:#101c2b;"
    "color:#8fa2ba;font-size:12px;line-height:1.4}.status{margin:14px 0;"
    "padding:15px;border-radius:13px;background:#111e2e;color:#aebdd1;font-size:13px;"
    "line-height:1.5;position:sticky;top:10px;z-index:4;box-shadow:0 12px 30px #0008}"
    ".status button{margin-top:12px}.ok{color:#64e59b}.bad{color:#ff8792}.fine{font-size:11px;"
    "margin-top:16px;color:#708097}code{color:#8dc8ff}</style></head><body><main>"
    "<div class=brand><span class=mark>R</span> REVLINK<span class=pill>LOCAL SETUP</span></div>"
    "<h1>Choose how to connect</h1>"
    "<p>RevLink works directly anywhere, or it can join a nearby network so "
    "other devices on that network can open it.</p>"
    "<div id=status class=\"status hidden\"><div id=statusText></div>"
    "<button id=statusAction type=button class=\"secondary hidden\">Done</button></div>"
    "<section id=choices>"
    "<div class=card><div class=title>Join a Wi-Fi network</div>"
    "<div class=copy>Best at home or when using your phone hotspot.</div>"
    "<form id=form><div id=networks class=networks>"
    "<div class=copy>Finding nearby networks…</div></div>"
    "<button id=manual type=button class=link>Enter a hidden network</button>"
    "<div id=manualFields class=hidden><label for=ssid>Network name</label>"
    "<input id=ssid maxlength=32 autocomplete=off></div>"
    "<label for=password>Password</label>"
    "<div class=password-wrap><input id=password type=password maxlength=63 "
    "autocomplete=current-password><button id=reveal type=button class=reveal "
    "aria-label=\"Show password\" aria-pressed=false>Show</button></div>"
    "<div class=note>Using an iPhone hotspot? Enable <b>Maximize Compatibility</b> "
    "so RevLink can use its 2.4 GHz connection.</div>"
    "<button id=submit class=primary>Connect to selected network</button></form></div>"
    "<div class=card><div class=title>Use RevLink Direct</div>"
    "<div class=copy>Stay connected to this RevLink Wi-Fi. No internet or phone "
    "hotspot is needed—logs, maps, and sync remain completely local.</div>"
    "<button id=direct class=secondary>Continue with RevLink Direct</button></div>"
    "</section>"
    "<div class=fine>Local setup only · AccessPort writes remain locked</div>"
    "<script>"
    "const q=x=>document.querySelector(x),s=q('#status'),t=q('#statusText'),"
    "a=q('#statusAction'),b=q('#submit'),c=q('#choices');"
    "let chosen='',closing=false;function message(html,focus=false){t.innerHTML=html;"
    "s.classList.remove('hidden');if(focus)requestAnimationFrame(()=>"
    "s.scrollIntoView({behavior:'smooth',block:'start'}))}"
    "function connecting(html){c.classList.add('hidden');a.classList.add('hidden');"
    "message(html,true)}"
    "function finishPortal(){if(closing)return;closing=true;window.close();"
    "setTimeout(()=>location.replace('/hotspot-detect.html'),200)}"
    "a.onclick=finishPortal;"
    "q('#reveal').onclick=e=>{let p=q('#password'),show=p.type==='password';"
    "p.type=show?'text':'password';e.currentTarget.textContent=show?'Hide':'Show';"
    "e.currentTarget.setAttribute('aria-label',(show?'Hide':'Show')+' password');"
    "e.currentTarget.setAttribute('aria-pressed',show?'true':'false');p.focus()};"
    "function choose(btn,name){chosen=name;q('#ssid').value='';"
    "document.querySelectorAll('.network').forEach(x=>x.classList.remove('selected'));"
    "btn.classList.add('selected')}"
    "async function networks(){try{let r=await fetch('/api/onboarding/networks',{cache:'no-store'}),"
    "j=await r.json(),box=q('#networks');box.textContent='';"
    "if(!j.networks.length){box.innerHTML='<div class=copy>No visible networks found. Use hidden network entry.</div>';return}"
    "j.networks.forEach(n=>{let x=document.createElement('button');x.type='button';x.className='network';"
    "let name=document.createElement('span');name.textContent=(n.secured?'● ':'○ ')+n.ssid;"
    "let sig=document.createElement('span');sig.className='signal';sig.textContent=n.signal;"
    "x.append(name,sig);x.onclick=()=>choose(x,n.ssid);box.append(x)})"
    "}catch(e){q('#networks').innerHTML='<div class=copy>Scan unavailable. Enter the network name instead.</div>';"
    "q('#manualFields').classList.remove('hidden')}}"
    "q('#manual').onclick=()=>{chosen='';document.querySelectorAll('.network').forEach("
    "x=>x.classList.remove('selected'));q('#manualFields').classList.toggle('hidden');q('#ssid').focus()};"
    "function closeCountdown(seconds,text){c.classList.add('hidden');a.classList.remove('hidden');"
    "let left=Math.max(1,seconds),timer;const draw=()=>message('<span class=ok>'+text+"
    "'</span><br>This setup window will close in '+left+' seconds.',true);draw();"
    "timer=setInterval(()=>{left--;if(left<=0){clearInterval(timer);finishPortal();return}"
    "draw()},1000)}"
    "function directReady(j){c.innerHTML='<div class=card><div class=title>RevLink Direct is ready</div>"
    "<div class=copy>Keep this device connected to <b>'+j.device+'</b>. Open <code>http://'+"
    "j.hostname+'.local</code> anytime to manage the connected AccessPort.</div></div>';"
    "a.classList.remove('hidden');message('<span class=ok>Local connection active.</span> "
    "No internet connection is required.',true)}"
    "async function watch(){try{let r=await fetch('/api/onboarding/status',{cache:'no-store'}),j=await r.json();"
    "if(j.setup==='success'){closeCountdown(j.closeIn||1,'Connected successfully.');return}"
    "if(j.setup==='failed'){message('<span class=bad>'+j.message+'</span><br>Check the password and try again.',true);"
    "c.classList.remove('hidden');b.disabled=false;q('#password').value='';return}"
    "connecting('Connecting securely… RevLink Direct will stay available while this is tested.');"
    "setTimeout(watch,700)}catch(e){setTimeout(watch,700)}}"
    "document.querySelector('#form').addEventListener('submit',async e=>{e.preventDefault();"
    "let ssid=chosen||q('#ssid').value.trim();if(!ssid){message('<span class=bad>Select or enter a network.</span>');return}"
    "b.disabled=true;connecting('Starting secure connection test…');try{"
    "let r=await fetch('/api/onboarding/join',{method:'POST',headers:{"
    "'Content-Type':'application/x-www-form-urlencoded','X-RevLink-Onboarding':'1'},"
    "body:new URLSearchParams({ssid:ssid,password:q('#password').value})});let j=await r.json();"
    "if(!r.ok)throw Error(j.error||'Request failed');"
    "q('#password').value='';watch()}catch(e){c.classList.remove('hidden');"
    "message('<span class=bad>'+e.message+'</span>',true);b.disabled=false;}});"
    "q('#direct').onclick=async()=>{try{let r=await fetch('/api/onboarding/direct',"
    "{method:'POST',headers:{'X-RevLink-Onboarding':'1'}}),j=await r.json();"
    "if(!r.ok)throw Error(j.error||'Request failed');directReady(j)}catch(e){"
    "message('<span class=bad>'+e.message+'</span>')}};"
    "async function resume(){try{let r=await fetch('/api/onboarding/status',{cache:'no-store'}),"
    "j=await r.json();if(j.setup==='failed'){message('<span class=bad>'+j.message+'</span><br>"
    "Select a network and try again.',true);return}if(j.setup==='connecting'){"
    "connecting('Connection test in progress…');watch();return}if(j.setup==='success'){"
    "closeCountdown(j.closeIn||1,'Connected successfully.');return}if(j.setup==='direct'){"
    "directReady(j)}}"
    "catch(e){}}networks();resume();"
    "</script></main></body></html>";

typedef revlink_wifi_credentials_t join_request_t;

static void clear_sensitive(void *buffer, size_t size)
{
    volatile unsigned char *cursor = buffer;
    while (size-- > 0U) {
        *cursor++ = 0U;
    }
}

static void publish_onboarding_status(
    onboarding_state_t state,
    esp_err_t error,
    uint32_t close_after_ms
)
{
    portENTER_CRITICAL(&onboarding_lock);
    onboarding_status = (onboarding_status_t){
        .state = state,
        .last_error = error,
        .close_deadline_us = close_after_ms == 0U
            ? 0
            : esp_timer_get_time() + (int64_t)close_after_ms * 1000,
    };
    portEXIT_CRITICAL(&onboarding_lock);
}

static onboarding_status_t onboarding_status_snapshot(void)
{
    onboarding_status_t status;
    portENTER_CRITICAL(&onboarding_lock);
    status = onboarding_status;
    portEXIT_CRITICAL(&onboarding_lock);
    return status;
}

static const char *onboarding_state_name(onboarding_state_t state)
{
    switch (state) {
    case ONBOARDING_IDLE:
        return "idle";
    case ONBOARDING_CONNECTING:
        return "connecting";
    case ONBOARDING_FAILED:
        return "failed";
    case ONBOARDING_SUCCEEDED:
        return "success";
    case ONBOARDING_DIRECT:
        return "direct";
    default:
        return "unknown";
    }
}

static uint32_t onboarding_close_seconds(onboarding_status_t status)
{
    if (status.close_deadline_us == 0) {
        return 0U;
    }
    const int64_t remaining_us =
        status.close_deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0U;
    }
    return (uint32_t)(
        remaining_us / 1000000
        + (remaining_us % 1000000 == 0 ? 0 : 1)
    );
}

static const char *onboarding_error_message(esp_err_t error)
{
    if (error == ESP_ERR_TIMEOUT) {
        return "The network did not respond before the connection timed out.";
    }
    if (error == ESP_ERR_WIFI_CONN) {
        return "RevLink could not join that network.";
    }
    return "The connection could not be completed.";
}

static bool hotspot_is_ready(void)
{
    const revlink_network_runtime_snapshot_t snapshot =
        revlink_network_runtime_snapshot();
    return snapshot.coordinator.state == REVLINK_NETWORK_HOTSPOT_READY
        && !snapshot.coordinator.transfer_active;
}

static void onboarding_display_task(void *context)
{
    (void)context;
    bool hotspot_was_visible = true;
    while (true) {
        const revlink_network_runtime_snapshot_t snapshot =
            revlink_network_runtime_snapshot();
        const onboarding_status_t setup = onboarding_status_snapshot();
        revlink_status_oled_update_network(
            &snapshot.coordinator,
            snapshot.connected_ssid
        );
        const bool hotspot_should_be_visible =
            (
                snapshot.coordinator.state == REVLINK_NETWORK_HOTSPOT_STARTING
                || snapshot.coordinator.state == REVLINK_NETWORK_HOTSPOT_READY
            )
            && setup.state != ONBOARDING_DIRECT
            && setup.state != ONBOARDING_CONNECTING
            && setup.state != ONBOARDING_SUCCEEDED;
        if (hotspot_should_be_visible != hotspot_was_visible) {
            if (hotspot_should_be_visible) {
                revlink_status_oled_restore_hotspot();
            } else {
                revlink_status_oled_hide_hotspot();
            }
            hotspot_was_visible = hotspot_should_be_visible;
        }
        vTaskDelay(pdMS_TO_TICKS(ONBOARDING_DISPLAY_PERIOD_MS));
    }
}

static esp_err_t send_json(
    httpd_req_t *request,
    const char *status,
    const char *json
)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    return httpd_resp_sendstr(request, json);
}

static esp_err_t page_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(
        request,
        "Content-Security-Policy",
        "default-src 'self'; style-src 'unsafe-inline'; "
        "script-src 'unsafe-inline'; connect-src 'self'; "
        "img-src 'none'; object-src 'none'; frame-ancestors 'none'"
    );
    return httpd_resp_send(
        request,
        ONBOARDING_PAGE,
        HTTPD_RESP_USE_STRLEN
    );
}

static esp_err_t root_handler(httpd_req_t *request)
{
    const onboarding_status_t setup = onboarding_status_snapshot();
    const revlink_network_runtime_snapshot_t network =
        revlink_network_runtime_snapshot();
    if (
        setup.state == ONBOARDING_DIRECT
        || setup.state == ONBOARDING_SUCCEEDED
        || network.coordinator.state == REVLINK_NETWORK_CLIENT_READY
    ) {
        return revlink_portal_page_handler(request);
    }
    return page_handler(request);
}

static esp_err_t status_handler(httpd_req_t *request)
{
    char ssid[LOCAL_IDENTITY_CAPACITY] = {0};
    char hostname[LOCAL_IDENTITY_CAPACITY] = {0};
    if (
        local_identity(
            ssid,
            sizeof(ssid),
            hostname,
            sizeof(hostname)
        ) != ESP_OK
    ) {
        return send_json(
            request,
            "503 Service Unavailable",
            "{\"error\":\"Device identity is unavailable\"}"
        );
    }
    const revlink_network_runtime_snapshot_t snapshot =
        revlink_network_runtime_snapshot();
    const onboarding_status_t setup = onboarding_status_snapshot();
    char response[448] = {0};
    const int length = snprintf(
        response,
        sizeof(response),
        "{\"device\":\"%s\",\"hostname\":\"%s\","
        "\"state\":\"%s\",\"clients\":%u,\"transferActive\":%s,"
        "\"credentialsPersistent\":%s,\"setup\":\"%s\","
        "\"closeIn\":%u,\"message\":\"%s\"}",
        ssid,
        hostname,
        revlink_network_state_name(snapshot.coordinator.state),
        (unsigned int)snapshot.radio.hotspot_client_count,
        snapshot.coordinator.transfer_active ? "true" : "false",
        snapshot.station_credentials_persistent ? "true" : "false",
        onboarding_state_name(setup.state),
        (unsigned int)onboarding_close_seconds(setup),
        setup.state == ONBOARDING_FAILED
            ? onboarding_error_message(setup.last_error)
            : ""
    );
    if (length <= 0 || (size_t)length >= sizeof(response)) {
        return ESP_FAIL;
    }
    return send_json(request, HTTPD_200, response);
}

static const char *signal_name(int8_t rssi)
{
    if (rssi >= -55) {
        return "Excellent";
    }
    if (rssi >= -67) {
        return "Good";
    }
    if (rssi >= -75) {
        return "Fair";
    }
    return "Weak";
}

static bool append_json_ssid(
    char *target,
    size_t capacity,
    size_t *cursor,
    const char *ssid
)
{
    if (*cursor + 1U >= capacity) {
        return false;
    }
    target[(*cursor)++] = '"';
    for (size_t index = 0U; ssid[index] != '\0'; ++index) {
        if (ssid[index] == '"' || ssid[index] == '\\') {
            if (*cursor + 2U >= capacity) {
                return false;
            }
            target[(*cursor)++] = '\\';
        } else if (*cursor + 1U >= capacity) {
            return false;
        }
        target[(*cursor)++] = ssid[index];
    }
    if (*cursor + 2U > capacity) {
        return false;
    }
    target[(*cursor)++] = '"';
    target[*cursor] = '\0';
    return true;
}

static esp_err_t networks_handler(httpd_req_t *request)
{
    revlink_wifi_visible_network_t
        networks[REVLINK_WIFI_VISIBLE_NETWORK_LIMIT] = {0};
    const size_t count = revlink_wifi_radio_visible_networks(
        networks,
        REVLINK_WIFI_VISIBLE_NETWORK_LIMIT
    );
    char response[2048] = "{\"networks\":[";
    size_t cursor = strlen(response);
    for (size_t index = 0U; index < count; ++index) {
        const int prefix = snprintf(
            response + cursor,
            sizeof(response) - cursor,
            "%s{\"ssid\":",
            index == 0U ? "" : ","
        );
        if (prefix <= 0 || (size_t)prefix >= sizeof(response) - cursor) {
            return ESP_FAIL;
        }
        cursor += (size_t)prefix;
        if (!append_json_ssid(
            response,
            sizeof(response),
            &cursor,
            networks[index].ssid
        )) {
            return ESP_FAIL;
        }
        const int suffix = snprintf(
            response + cursor,
            sizeof(response) - cursor,
            ",\"signal\":\"%s\",\"secured\":%s}",
            signal_name(networks[index].rssi),
            networks[index].secured ? "true" : "false"
        );
        if (suffix <= 0 || (size_t)suffix >= sizeof(response) - cursor) {
            return ESP_FAIL;
        }
        cursor += (size_t)suffix;
    }
    if (cursor + sizeof("]}") > sizeof(response)) {
        return ESP_FAIL;
    }
    memcpy(response + cursor, "]}", sizeof("]}"));
    return send_json(request, HTTPD_200, response);
}

static bool onboarding_header_is_valid(httpd_req_t *request)
{
    const size_t length = httpd_req_get_hdr_value_len(
        request,
        "X-RevLink-Onboarding"
    );
    if (length != 1U) {
        return false;
    }
    char value[2] = {0};
    return httpd_req_get_hdr_value_str(
        request,
        "X-RevLink-Onboarding",
        value,
        sizeof(value)
    ) == ESP_OK && value[0] == '1';
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

static bool decode_form_value(
    const char *encoded,
    size_t encoded_length,
    char *decoded,
    size_t decoded_capacity
)
{
    size_t output = 0U;
    for (size_t input = 0U; input < encoded_length; ++input) {
        unsigned char value = (unsigned char)encoded[input];
        if (value == '+') {
            value = ' ';
        } else if (value == '%') {
            if (input + 2U >= encoded_length) {
                return false;
            }
            const int high = hex_value(encoded[input + 1U]);
            const int low = hex_value(encoded[input + 2U]);
            if (high < 0 || low < 0) {
                return false;
            }
            value = (unsigned char)((high << 4) | low);
            input += 2U;
        }
        if (value == 0U || output + 1U >= decoded_capacity) {
            return false;
        }
        decoded[output++] = (char)value;
    }
    decoded[output] = '\0';
    return true;
}

static bool parse_form_credentials(
    const char *body,
    size_t body_size,
    revlink_wifi_credentials_t *credentials
)
{
    char ssid[REVLINK_WIFI_SSID_CAPACITY] = {0};
    char password[REVLINK_WIFI_PASSWORD_CAPACITY] = {0};
    bool found_ssid = false;
    bool found_password = false;
    size_t cursor = 0U;

    while (cursor < body_size) {
        size_t end = cursor;
        while (end < body_size && body[end] != '&') {
            ++end;
        }
        size_t equals = cursor;
        while (equals < end && body[equals] != '=') {
            ++equals;
        }
        if (equals == end) {
            return false;
        }
        const size_t name_length = equals - cursor;
        const char *value = &body[equals + 1U];
        const size_t value_length = end - equals - 1U;
        if (
            name_length == sizeof("ssid") - 1U
            && memcmp(&body[cursor], "ssid", name_length) == 0
            && !found_ssid
        ) {
            found_ssid = decode_form_value(
                value,
                value_length,
                ssid,
                sizeof(ssid)
            );
            if (!found_ssid) {
                return false;
            }
        } else if (
            name_length == sizeof("password") - 1U
            && memcmp(&body[cursor], "password", name_length) == 0
            && !found_password
        ) {
            found_password = decode_form_value(
                value,
                value_length,
                password,
                sizeof(password)
            );
            if (!found_password) {
                return false;
            }
        } else {
            return false;
        }
        cursor = end + 1U;
    }

    const bool valid = found_ssid
        && found_password
        && revlink_wifi_credentials_assign(credentials, ssid, password);
    clear_sensitive(ssid, sizeof(ssid));
    clear_sensitive(password, sizeof(password));
    return valid;
}

static void join_task(void *context)
{
    join_request_t *join = context;
    vTaskDelay(pdMS_TO_TICKS(750));
    const esp_err_t status =
        revlink_network_runtime_configure_station(
            join->ssid,
            join->password
        );
    const revlink_network_runtime_snapshot_t snapshot =
        revlink_network_runtime_snapshot();
    const bool joined =
        status == ESP_OK
        && snapshot.coordinator.state == REVLINK_NETWORK_CLIENT_READY;
    if (joined) {
        publish_onboarding_status(
            ONBOARDING_SUCCEEDED,
            ESP_OK,
            ONBOARDING_SUCCESS_HOLD_MS
        );
        revlink_status_oled_hide_hotspot();
        ESP_LOGI(
            TAG,
            "Onboarding station transition completed; identity suppressed"
        );
        vTaskDelay(pdMS_TO_TICKS(ONBOARDING_SUCCESS_HOLD_MS));
        const esp_err_t finish_status =
            revlink_wifi_radio_finish_onboarding_transition();
        if (finish_status != ESP_OK) {
            ESP_LOGW(
                TAG,
                "Unable to end onboarding network overlap: %s",
                esp_err_to_name(finish_status)
            );
        }
    } else {
        const esp_err_t station_error =
            revlink_network_runtime_last_station_error();
        publish_onboarding_status(
            ONBOARDING_FAILED,
            station_error != ESP_OK
                ? station_error
                : (status == ESP_OK
                    ? snapshot.coordinator.last_platform_error
                    : status),
            0U
        );
        ESP_LOGW(
            TAG,
            "Onboarding station transition returned to safe setup mode: "
            "status=%s state=%s",
            esp_err_to_name(status),
            revlink_network_state_name(snapshot.coordinator.state)
        );
    }
    clear_sensitive(join, sizeof(*join));
    free(join);
    vTaskDelete(NULL);
}

static esp_err_t join_handler(httpd_req_t *request)
{
    if (!hotspot_is_ready()) {
        return send_json(
            request,
            "409 Conflict",
            "{\"error\":\"Setup is available only from the RevLink hotspot "
            "while no transfer is active\"}"
        );
    }
    publish_onboarding_status(ONBOARDING_CONNECTING, ESP_OK, 0U);
    if (!onboarding_header_is_valid(request)) {
        publish_onboarding_status(ONBOARDING_IDLE, ESP_OK, 0U);
        return send_json(
            request,
            "403 Forbidden",
            "{\"error\":\"Onboarding request header is missing\"}"
        );
    }
    if (
        request->content_len <= 0
        || request->content_len >= (int)ONBOARDING_BODY_LIMIT
    ) {
        publish_onboarding_status(ONBOARDING_IDLE, ESP_OK, 0U);
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Invalid request size\"}"
        );
    }

    char body[ONBOARDING_BODY_LIMIT] = {0};
    size_t received = 0U;
    while (received < (size_t)request->content_len) {
        const int result = httpd_req_recv(
            request,
            &body[received],
            (size_t)request->content_len - received
        );
        if (result <= 0) {
            clear_sensitive(body, sizeof(body));
            publish_onboarding_status(ONBOARDING_IDLE, ESP_OK, 0U);
            return send_json(
                request,
                HTTPD_400,
                "{\"error\":\"Incomplete request\"}"
            );
        }
        received += (size_t)result;
    }

    join_request_t *join = calloc(1U, sizeof(*join));
    if (join == NULL) {
        clear_sensitive(body, sizeof(body));
        publish_onboarding_status(ONBOARDING_IDLE, ESP_OK, 0U);
        return send_json(
            request,
            HTTPD_500,
            "{\"error\":\"Unable to queue connection\"}"
        );
    }
    const bool parsed = parse_form_credentials(body, received, join);
    clear_sensitive(body, sizeof(body));
    if (!parsed) {
        clear_sensitive(join, sizeof(*join));
        free(join);
        publish_onboarding_status(ONBOARDING_IDLE, ESP_OK, 0U);
        return send_json(
            request,
            HTTPD_400,
            "{\"error\":\"Use a printable 1-32 character network name and "
            "an empty or 8-63 character password\"}"
        );
    }

    const BaseType_t created = xTaskCreate(
        join_task,
        "revlink_wifi_join",
        ONBOARDING_TASK_STACK,
        join,
        4,
        NULL
    );
    if (created != pdPASS) {
        clear_sensitive(join, sizeof(*join));
        free(join);
        publish_onboarding_status(ONBOARDING_IDLE, ESP_OK, 0U);
        return send_json(
            request,
            HTTPD_500,
            "{\"error\":\"Unable to queue connection\"}"
        );
    }

    char hostname[LOCAL_IDENTITY_CAPACITY] = {0};
    char unused_ssid[LOCAL_IDENTITY_CAPACITY] = {0};
    if (
        local_identity(
            unused_ssid,
            sizeof(unused_ssid),
            hostname,
            sizeof(hostname)
        ) != ESP_OK
    ) {
        memcpy(hostname, "revlink", sizeof("revlink"));
    }
    char response[128] = {0};
    const int response_length = snprintf(
        response,
        sizeof(response),
        "{\"accepted\":true,\"hostname\":\"%s\"}",
        hostname
    );
    if (
        response_length <= 0
        || (size_t)response_length >= sizeof(response)
    ) {
        return ESP_FAIL;
    }
    return send_json(request, "202 Accepted", response);
}

static esp_err_t direct_handler(httpd_req_t *request)
{
    if (!hotspot_is_ready()) {
        return send_json(
            request,
            "409 Conflict",
            "{\"error\":\"RevLink Direct is unavailable during an active "
            "transfer or network transition\"}"
        );
    }
    if (!onboarding_header_is_valid(request)) {
        return send_json(
            request,
            "403 Forbidden",
            "{\"error\":\"Onboarding request header is missing\"}"
        );
    }
    char ssid[LOCAL_IDENTITY_CAPACITY] = {0};
    char hostname[LOCAL_IDENTITY_CAPACITY] = {0};
    if (
        local_identity(
            ssid,
            sizeof(ssid),
            hostname,
            sizeof(hostname)
        ) != ESP_OK
    ) {
        return send_json(
            request,
            "503 Service Unavailable",
            "{\"error\":\"Device identity is unavailable\"}"
        );
    }
    publish_onboarding_status(
        ONBOARDING_DIRECT,
        ESP_OK,
        ONBOARDING_DIRECT_CLOSE_SECONDS * 1000U
    );
    revlink_status_oled_hide_hotspot();
    char response[128] = {0};
    const int length = snprintf(
        response,
        sizeof(response),
        "{\"accepted\":true,\"device\":\"%s\",\"hostname\":\"%s\","
        "\"closeIn\":%u}",
        ssid,
        hostname,
        (unsigned int)ONBOARDING_DIRECT_CLOSE_SECONDS
    );
    if (length <= 0 || (size_t)length >= sizeof(response)) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "RevLink Direct selected; local hotspot remains active");
    return send_json(request, HTTPD_200, response);
}

static esp_err_t captive_probe_handler(httpd_req_t *request)
{
    const onboarding_status_t setup = onboarding_status_snapshot();
    if (
        setup.state == ONBOARDING_DIRECT
        || setup.state == ONBOARDING_SUCCEEDED
    ) {
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        if (strcmp(request->uri, "/generate_204") == 0) {
            httpd_resp_set_status(request, "204 No Content");
            return httpd_resp_send(request, NULL, 0);
        }
        if (strcmp(request->uri, "/connecttest.txt") == 0) {
            httpd_resp_set_type(request, "text/plain");
            return httpd_resp_sendstr(request, "Microsoft Connect Test");
        }
        httpd_resp_set_type(request, "text/html");
        return httpd_resp_sendstr(
            request,
            "<HTML><HEAD><TITLE>Success</TITLE></HEAD>"
            "<BODY>Success</BODY></HTML>"
        );
    }
    return page_handler(request);
}

static esp_err_t redirect_handler(httpd_req_t *request)
{
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, NULL, 0);
}

static void captive_dns_task(void *context)
{
    (void)context;
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) {
        ESP_LOGE(TAG, "Unable to create captive DNS socket: %d", errno);
        vTaskDelete(NULL);
        return;
    }
    const struct timeval timeout = {
        .tv_sec = 1,
        .tv_usec = 0,
    };
    (void)setsockopt(
        socket_fd,
        SOL_SOCKET,
        SO_RCVTIMEO,
        &timeout,
        sizeof(timeout)
    );
    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(53U),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(
        socket_fd,
        (const struct sockaddr *)&address,
        sizeof(address)
    ) != 0) {
        ESP_LOGE(TAG, "Unable to bind captive DNS socket: %d", errno);
        close(socket_fd);
        vTaskDelete(NULL);
        return;
    }

    uint8_t query[CAPTIVE_DNS_PACKET_LIMIT] = {0};
    uint8_t response[CAPTIVE_DNS_PACKET_LIMIT] = {0};
    while (true) {
        struct sockaddr_storage peer = {0};
        socklen_t peer_size = sizeof(peer);
        const ssize_t received = recvfrom(
            socket_fd,
            query,
            sizeof(query),
            0,
            (struct sockaddr *)&peer,
            &peer_size
        );
        if (received <= 0) {
            continue;
        }
        if (!hotspot_is_ready()) {
            continue;
        }
        uint32_t hotspot_address = 0U;
        if (
            revlink_wifi_radio_hotspot_ipv4(&hotspot_address)
            != ESP_OK
        ) {
            continue;
        }
        size_t response_size = 0U;
        if (
            revlink_captive_dns_build_response(
                query,
                (size_t)received,
                hotspot_address,
                response,
                sizeof(response),
                &response_size
            ) != REVLINK_CAPTIVE_DNS_OK
        ) {
            continue;
        }
        (void)sendto(
            socket_fd,
            response,
            response_size,
            0,
            (const struct sockaddr *)&peer,
            peer_size
        );
    }
}

static esp_err_t register_uri(
    httpd_handle_t server,
    const char *uri,
    httpd_method_t method,
    esp_err_t (*handler)(httpd_req_t *)
)
{
    const httpd_uri_t route = {
        .uri = uri,
        .method = method,
        .handler = handler,
        .user_ctx = NULL,
    };
    return httpd_register_uri_handler(server, &route);
}

esp_err_t revlink_onboarding_start(void)
{
    if (onboarding_server != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    char ssid[LOCAL_IDENTITY_CAPACITY] = {0};
    char hostname[LOCAL_IDENTITY_CAPACITY] = {0};
    esp_err_t status = local_identity(
        ssid,
        sizeof(ssid),
        hostname,
        sizeof(hostname)
    );
    if (status != ESP_OK) {
        return status;
    }
    publish_onboarding_status(ONBOARDING_IDLE, ESP_OK, 0U);

    /*
     * Thirty-one unambiguous lowercase symbols provide about 40 bits across
     * eight characters while avoiding Shift and similar 0/O, 1/I/L.
     */
    static const char setup_alphabet[] =
        "23456789abcdefghjkmnpqrstuvwxyz";
    char setup_password[SETUP_PASSWORD_LENGTH + 1U] = {0};
    uint8_t random_bytes[SETUP_PASSWORD_LENGTH] = {0};
    esp_fill_random(random_bytes, sizeof(random_bytes));
    for (size_t index = 0U; index < SETUP_PASSWORD_LENGTH; ++index) {
        setup_password[index] =
            setup_alphabet[random_bytes[index] % (sizeof(setup_alphabet) - 1U)];
    }
    clear_sensitive(random_bytes, sizeof(random_bytes));
    revlink_status_oled_show_hotspot(ssid, setup_password);
    status = revlink_network_runtime_configure_hotspot_ephemeral(
        setup_password
    );
    clear_sensitive(setup_password, sizeof(setup_password));
    if (status != ESP_OK) {
        revlink_status_oled_clear_hotspot();
        return status;
    }

    status = mdns_init();
    if (status == ESP_OK) {
        status = mdns_hostname_set(hostname);
    }
    if (status == ESP_OK) {
        status = mdns_instance_name_set("RevLink Setup");
    }
    if (status == ESP_OK) {
        status = mdns_service_add(
            "RevLink Setup",
            "_http",
            "_tcp",
            80U,
            NULL,
            0U
        );
    }
    if (status != ESP_OK) {
        ESP_LOGE(TAG, "mDNS startup failed: %s", esp_err_to_name(status));
        return status;
    }

    httpd_config_t configuration = HTTPD_DEFAULT_CONFIG();
    configuration.server_port = 80U;
    /*
     * Onboarding, captive-network compatibility, and the authenticated local
     * portal share one listener. Keep deliberate headroom for read-only
     * product features such as content-addressed cache streaming.
     */
    /*
     * Ten onboarding/captive routes, nineteen portal routes, and the final
     * wildcard redirect are registered below. Keep two spare slots so adding
     * a small portal asset cannot silently prevent the rest of app_main from
     * starting on the device.
     */
    /*
     * Keep deliberate headroom for independently owned portal services.
     * Backup/restore adds three handlers and future optional modules should
     * not make an otherwise healthy Sidecar fail to start.
     */
    configuration.max_uri_handlers = 48U;
    /*
     * One listener serves the onboarding routes and every portal route, so
     * this stack has to cover the deepest handler, not the shallowest. The
     * portal streams cached files through FatFS and parses request bodies in
     * stack buffers — the note handler alone reserves 2600 bytes — on top of
     * httpd's own header parsing.
     *
     * At 6144 that overflowed: the board panicked with a stack protection
     * fault whose corrupted task name was the text of an Accept- header,
     * because the overrun walked into the task control block.
     *
     * Measured under 720 browser-shaped requests, the deepest this task goes
     * is about 6116 bytes — twenty-eight bytes inside the old size, which is
     * why it survived light use and died under a real browser. Handlers log
     * the remaining headroom as it reaches new lows; see send_json().
     */
    configuration.stack_size = 12288U;
    configuration.lru_purge_enable = true;
    configuration.uri_match_fn = httpd_uri_match_wildcard;
    status = httpd_start(&onboarding_server, &configuration);
    if (status != ESP_OK) {
        mdns_free();
        onboarding_server = NULL;
        return status;
    }

    const struct {
        const char *uri;
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t *);
    } routes[] = {
        {"/", HTTP_GET, root_handler},
        {"/index.html", HTTP_GET, root_handler},
        {"/setup", HTTP_GET, page_handler},
        {"/api/onboarding/status", HTTP_GET, status_handler},
        {"/api/onboarding/networks", HTTP_GET, networks_handler},
        {"/api/onboarding/join", HTTP_POST, join_handler},
        {"/api/onboarding/direct", HTTP_POST, direct_handler},
        {"/hotspot-detect.html", HTTP_GET, captive_probe_handler},
        {"/generate_204", HTTP_GET, captive_probe_handler},
        {"/connecttest.txt", HTTP_GET, captive_probe_handler},
    };
    for (size_t index = 0U; index < sizeof(routes) / sizeof(routes[0]); ++index) {
        status = register_uri(
            onboarding_server,
            routes[index].uri,
            routes[index].method,
            routes[index].handler
        );
        if (status != ESP_OK) {
            httpd_stop(onboarding_server);
            onboarding_server = NULL;
            mdns_free();
            return status;
        }
    }
    status = revlink_portal_register(onboarding_server);
    if (status == ESP_OK) {
        status = register_uri(
            onboarding_server,
            "/*",
            HTTP_GET,
            redirect_handler
        );
    }
    if (status != ESP_OK) {
        httpd_stop(onboarding_server);
        onboarding_server = NULL;
        mdns_free();
        return status;
    }

    TaskHandle_t captive_dns_handle = NULL;
    const BaseType_t created = xTaskCreate(
        captive_dns_task,
        "revlink_captive_dns",
        CAPTIVE_DNS_TASK_STACK,
        NULL,
        4,
        &captive_dns_handle
    );
    if (created != pdPASS) {
        httpd_stop(onboarding_server);
        onboarding_server = NULL;
        mdns_free();
        return ESP_ERR_NO_MEM;
    }
    if (
        xTaskCreate(
            onboarding_display_task,
            "revlink_onboard_display",
            ONBOARDING_DISPLAY_TASK_STACK,
            NULL,
            3,
            NULL
        ) != pdPASS
    ) {
        vTaskDelete(captive_dns_handle);
        httpd_stop(onboarding_server);
        onboarding_server = NULL;
        mdns_free();
        revlink_status_oled_clear_hotspot();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Local onboarding is ready on port 80; only working station "
        "credentials persist"
    );
    return ESP_OK;
}
