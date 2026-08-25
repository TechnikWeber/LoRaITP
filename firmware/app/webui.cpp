/*
 * Access point and web interface.
 *
 * Serves the image gallery and the settings the application actually
 * needs changed in the field: which end of the link this board is, which
 * regulatory region, and how much airtime a picture may cost.
 *
 * The access point is on by default and stays on, which is not the
 * long-term answer - it draws 100-150 mA continuously, more than the
 * radio does while transmitting. The auto-off is implemented and can be
 * ticked on here; it is off by default because a bench board whose
 * network keeps vanishing is a nuisance.
 *
 * When it is enabled, the page sends a heartbeat every 30 seconds. A
 * gallery generates no requests while somebody is looking at it, so a
 * plain request timer would close the network out from under them.
 */
#include <Arduino.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "webui.h"

static WebServer g_server(80);
static loraitp_appcfg_t *g_cfg;
static loraitp_store_t *g_store;
static loraitp_webui_status_cb g_status_cb;
static void *g_status_user;
static uint32_t g_last_activity;
static bool g_running;

#define IMAGES_DIR "/images"

/* ------------------------------------------------------------ the page */

static const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>LoRaITP</title>
<style>
:root{color-scheme:light dark}
body{font:15px/1.5 system-ui,sans-serif;margin:0;padding:1rem;max-width:52rem}
h1{font-size:1.2rem;margin:0 0 .5rem}
nav a{margin-right:1rem}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:.75rem}
.card{border:1px solid #8884;border-radius:6px;padding:.4rem}
.card img{width:100%;display:block;border-radius:3px;background:#8882}
.card small{display:block;opacity:.7;font-size:.75rem}
table{border-collapse:collapse;width:100%}
td,th{text-align:left;padding:.25rem .5rem;border-bottom:1px solid #8882}
label{display:block;margin:.5rem 0}
input,select{font:inherit;padding:.25rem}
.warn{background:#fd05;border:1px solid #fa08;padding:.5rem;border-radius:4px}
</style>
<h1>LoRaITP <span id=role></span></h1>
<nav><a href="#" onclick="show('gallery')">Images</a>
<a href="#" onclick="show('status')">Status</a>
<a href="/settings">Settings</a></nav>
<div id=gallery><div class=grid id=grid></div></div>
<div id=status hidden><table id=stat></table></div>
<script>
function show(id){for(const s of['gallery','status'])
  document.getElementById(s).hidden=(s!==id);}
async function load(){
  const s=await (await fetch('/api/status')).json();
  document.getElementById('role').textContent='- '+s.role+' - '+s.region;
  const rows=[['Role',s.role],['Region',s.region],
    ['Frequency',(s.freq/1e6).toFixed(3)+' MHz'],['SF',s.sf],
    ['TX power',s.power+' dBm'],['Images stored',s.images],
    ['Airtime used (rolling hour)',s.used_ms+' ms'],
    ['Airtime budget',s.budget_ms?s.budget_ms+' ms':'no limit'],
    ['Bytes still allowed today',s.bytes_left],
    ['Last result',s.last||'-'],['Uptime',Math.round(s.uptime/1000)+' s']];
  document.getElementById('stat').innerHTML=
    rows.map(r=>`<tr><th>${r[0]}</th><td>${r[1]}</td></tr>`).join('');
  const l=await (await fetch('/api/list')).json();
  document.getElementById('grid').innerHTML = l.length? l.map(i=>
    `<div class=card><a href="/img?n=${i.name}"><img loading=lazy
      src="/img?n=${i.name}"></a><small>${i.name}</small>
      <small>${i.bytes} B</small></div>`).join('')
    : '<p>No images yet.</p>';
}
load();setInterval(load,15000);
setInterval(()=>fetch('/keepalive'),30000);
</script>
)HTML";

static const char SETTINGS_HEAD[] PROGMEM = R"HTML(<!doctype html>
<meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>LoRaITP settings</title>
<style>
:root{color-scheme:light dark}
body{font:15px/1.5 system-ui,sans-serif;margin:0;padding:1rem;max-width:34rem}
label{display:block;margin:.6rem 0}
input,select{font:inherit;padding:.25rem}
fieldset{border:1px solid #8884;border-radius:6px;margin:1rem 0}
small{opacity:.7;display:block}
button{font:inherit;padding:.4rem 1rem;margin-top:1rem}
</style>
<h1>Settings</h1><form method=post>
)HTML";

/* ------------------------------------------------------------ helpers */

static void touch(void) { g_last_activity = millis(); }

static bool safe_name(const String &n)
{
    /* Only names this firmware produces. A path assembled from an
     * unchecked query string is how "../.." leaves the image directory. */
    if (n.length() < 5 || n.length() > 40)
        return false;
    for (size_t i = 0; i < n.length(); i++) {
        char ch = n[i];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')
                  || ch == '_' || ch == '.';
        if (!ok)
            return false;
    }
    return n.indexOf("..") < 0;
}

static int list_cb(void *user, const char *name, uint32_t bytes)
{
    String *s = (String *)user;
    if (s->length() > 1)
        *s += ",";
    *s += "{\"name\":\"";
    *s += name;
    *s += "\",\"bytes\":";
    *s += bytes;
    *s += "}";
    return 0;
}

/* ------------------------------------------------------------ handlers */

static void h_root(void)
{
    touch();
    g_server.send_P(200, "text/html", PAGE);
}

static void h_keepalive(void)
{
    touch();
    g_server.send(200, "text/plain", "ok");
}

static void h_list(void)
{
    touch();
    String out = "[";
    loraitp_store_list(g_store, list_cb, &out);
    out += "]";
    g_server.send(200, "application/json", out);
}

static void h_file(const char *ext, const char *mime)
{
    touch();
    String n = g_server.arg("n");
    if (!safe_name(n)) {
        g_server.send(400, "text/plain", "bad name");
        return;
    }
    String path = String(IMAGES_DIR) + "/" + n;
    if (ext != NULL) {
        int dot = path.lastIndexOf('.');
        if (dot > 0)
            path = path.substring(0, dot) + ext;
    }
    File f = LittleFS.open(path, "r");
    if (!f) {
        g_server.send(404, "text/plain", "not found");
        return;
    }
    g_server.streamFile(f, mime);
    f.close();
}

static void h_img(void)  { h_file(NULL, "image/jpeg"); }
static void h_meta(void) { h_file(".json", "application/json"); }

static void h_status(void)
{
    touch();
    loraitp_webui_status_t s;
    memset(&s, 0, sizeof(s));
    if (g_status_cb)
        g_status_cb(g_status_user, &s);

    String j = "{";
    j += "\"role\":\"";
    j += (g_cfg->role == LORAITP_ROLE_SENDER) ? "sender" : "receiver";
    j += "\",\"region\":\"";
    j += loraitp_cfg_region_name(g_cfg->region);
    j += "\",\"freq\":";      j += g_cfg->frequency_hz;
    j += ",\"sf\":";          j += g_cfg->spreading_factor;
    j += ",\"power\":";       j += g_cfg->tx_power_dbm;
    j += ",\"images\":";      j += loraitp_store_count(g_store);
    j += ",\"used_ms\":";     j += s.airtime_used_ms;
    j += ",\"budget_ms\":";   j += s.airtime_budget_ms;
    j += ",\"bytes_left\":";  j += s.bytes_remaining;
    j += ",\"uptime\":";      j += millis();
    j += ",\"last\":\"";      j += s.last_result;
    j += "\"}";
    g_server.send(200, "application/json", j);
}

static void radio(String &h, const char *name, const char *value,
                  const char *label, bool checked)
{
    h += "<label><input type=radio name=";
    h += name;
    h += " value=";
    h += value;
    h += checked ? " checked>" : ">";
    h += label;
    h += "</label>";
}

static void h_settings_get(void)
{
    touch();
    String h = FPSTR(SETTINGS_HEAD);

    h += "<fieldset><legend>Role</legend>";
    radio(h, "role", "1", "Sender - captures and transmits",
          g_cfg->role == LORAITP_ROLE_SENDER);
    radio(h, "role", "0", "Receiver - listens and stores",
          g_cfg->role == LORAITP_ROLE_RECEIVER);
    h += "<small>The same firmware runs both ends; this only decides "
         "which one this board is.</small></fieldset>";

    h += "<fieldset><legend>Access point</legend>";
    radio(h, "apoff", "0", "Stay on (default)", !g_cfg->ap_auto_off);
    radio(h, "apoff", "1", "Switch off when idle", g_cfg->ap_auto_off);
    h += "<label>Idle timeout, seconds <input name=apto value=";
    h += g_cfg->ap_timeout_s;
    h += " size=6></label>";
    h += "<small>WiFi draws 100-150 mA continuously - more than the radio "
         "uses while transmitting. Switching it off matters on a battery, "
         "and not at all on a bench.</small></fieldset>";

    h += "<fieldset><legend>Radio</legend><label>Region <select name=region>";
    static const uint8_t regions[] = { 4, 0, 1, 3, 5, 7 };
    for (unsigned i = 0; i < sizeof(regions); i++) {
        h += "<option value=";
        h += regions[i];
        if (regions[i] == g_cfg->region)
            h += " selected";
        h += ">";
        h += loraitp_cfg_region_name(regions[i]);
        h += "</option>";
    }
    h += "</select></label>";
    h += "<small>EU868_G4_LP is 5 mW with no duty-cycle limit - the right "
         "place to develop. EU868_G3 is 500 mW at 10%, which is where a "
         "real link belongs.</small>";
    h += "<label>Frequency, Hz <input name=freq value=";
    h += g_cfg->frequency_hz;
    h += "></label><label>Spreading factor <input name=sf value=";
    h += g_cfg->spreading_factor;
    h += " size=4></label><label>TX power, dBm <input name=pwr value=";
    h += g_cfg->tx_power_dbm;
    h += " size=4></label><label>Call sign (AMATEUR region only) "
         "<input name=call value=";
    h += g_cfg->callsign;
    h += "></label></fieldset>";

    h += "<fieldset><legend>Pictures</legend>";
    h += "<label>Interval, seconds <input name=iv value=";
    h += g_cfg->interval_s;
    h += " size=8></label><label>Byte budget per image <input name=budget value=";
    h += g_cfg->image_budget;
    h += " size=8></label><label>Keep how many <input name=keep value=";
    h += g_cfg->keep_images;
    h += " size=6></label></fieldset>";

    h += "<button>Save and restart</button></form>";
    g_server.send(200, "text/html", h);
}

static void h_settings_post(void)
{
    touch();
    loraitp_appcfg_t c = *g_cfg;

    if (g_server.hasArg("role"))   c.role = (uint8_t)g_server.arg("role").toInt();
    if (g_server.hasArg("apoff"))  c.ap_auto_off = g_server.arg("apoff").toInt() != 0;
    if (g_server.hasArg("apto"))   c.ap_timeout_s = (uint16_t)g_server.arg("apto").toInt();
    if (g_server.hasArg("region")) c.region = (uint8_t)g_server.arg("region").toInt();
    if (g_server.hasArg("freq"))   c.frequency_hz = (uint32_t)g_server.arg("freq").toInt();
    if (g_server.hasArg("sf"))     c.spreading_factor = (uint8_t)g_server.arg("sf").toInt();
    if (g_server.hasArg("pwr"))    c.tx_power_dbm = (int8_t)g_server.arg("pwr").toInt();
    if (g_server.hasArg("iv"))     c.interval_s = (uint32_t)g_server.arg("iv").toInt();
    if (g_server.hasArg("budget")) c.image_budget = (uint16_t)g_server.arg("budget").toInt();
    if (g_server.hasArg("keep"))   c.keep_images = (uint16_t)g_server.arg("keep").toInt();
    if (g_server.hasArg("call"))
        snprintf(c.callsign, sizeof(c.callsign), "%s",
                 g_server.arg("call").c_str());

    loraitp_cfg_save(&c);
    g_server.send(200, "text/html",
                  "<meta charset=utf-8><p>Saved. Restarting.");
    delay(300);
    ESP.restart();
}

/* ------------------------------------------------------------- public */

void loraitp_webui_begin(loraitp_appcfg_t *cfg, loraitp_store_t *store,
                         loraitp_webui_status_cb cb, void *user)
{
    g_cfg = cfg;
    g_store = store;
    g_status_cb = cb;
    g_status_user = user;

    if (!cfg->ap_enabled)
        return;

    uint8_t mac[6];
    WiFi.macAddress(mac);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "LoRaITP-%02X%02X", mac[4], mac[5]);

    WiFi.mode(WIFI_AP);
    bool ok = (cfg->ap_password[0] != '\0')
              ? WiFi.softAP(ssid, cfg->ap_password)
              : WiFi.softAP(ssid);
    if (!ok) {
        Serial.println("access point failed to start");
        return;
    }

    g_server.on("/", h_root);
    g_server.on("/keepalive", h_keepalive);
    g_server.on("/api/list", h_list);
    g_server.on("/api/status", h_status);
    g_server.on("/img", h_img);
    g_server.on("/meta", h_meta);
    g_server.on("/settings", HTTP_GET, h_settings_get);
    g_server.on("/settings", HTTP_POST, h_settings_post);
    g_server.begin();

    g_running = true;
    touch();
    Serial.printf("access point %s up at %s\n", ssid,
                  WiFi.softAPIP().toString().c_str());
}

void loraitp_webui_stop(void)
{
    if (!g_running)
        return;
    g_server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    g_running = false;
    Serial.println("access point down");
}

bool loraitp_webui_running(void) { return g_running; }

void loraitp_webui_poll(void)
{
    if (!g_running)
        return;
    g_server.handleClient();

    if (g_cfg->ap_auto_off && g_cfg->ap_timeout_s > 0) {
        uint32_t idle = millis() - g_last_activity;
        if (idle > (uint32_t)g_cfg->ap_timeout_s * 1000u)
            loraitp_webui_stop();
    }
}
