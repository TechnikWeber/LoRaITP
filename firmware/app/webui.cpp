/*
 * Access point, captive portal and web interface.
 *
 * Three jobs: show the pictures, show what the radio is actually doing,
 * and let the settings that matter in the field be changed without a
 * toolchain.
 *
 * The live log is the important one. When a link does not work, the first
 * question is never "what was the result" - it is "was anything heard at
 * all", and that is a frame-by-frame question. Answering it used to need
 * a USB cable and a terminal, which is not a realistic thing to want from
 * a node up a mast.
 *
 * The captive portal exists for the same reason: on a phone, joining a
 * network and then remembering to type an IP address is a step too many
 * when you are standing in a field holding two boards.
 */
#include <Arduino.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "debuglog.h"
#include "webui.h"

static WebServer g_server(80);
static DNSServer g_dns;
static bool      g_dns_running;
static loraitp_appcfg_t *g_cfg;
static loraitp_store_t *g_store;
static loraitp_webui_status_cb g_status_cb;
static loraitp_webui_trigger_cb g_trigger_cb;
static void *g_status_user;
static uint32_t g_last_activity;
static bool g_running;
static IPAddress g_ip;

#define IMAGES_DIR "/images"

/* ------------------------------------------------------------ the page */

static const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>LoRaITP</title>
<style>
:root{color-scheme:light dark;--e:#8884}
body{font:15px/1.5 system-ui,sans-serif;margin:0;padding:1rem;max-width:54rem}
h1{font-size:1.15rem;margin:0 0 .5rem}
nav{margin-bottom:1rem}
nav a{margin-right:1rem;cursor:pointer;text-decoration:underline}
nav a.on{font-weight:600;text-decoration:none}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:.75rem}
.card{border:1px solid var(--e);border-radius:6px;padding:.4rem}
.card img{width:100%;display:block;border-radius:3px;background:#8882}
.card small{display:block;opacity:.7;font-size:.75rem}
table{border-collapse:collapse;width:100%}
td,th{text-align:left;padding:.2rem .5rem;border-bottom:1px solid var(--e);
  vertical-align:top}
th{white-space:nowrap;opacity:.75;font-weight:400}
#log{font:12px/1.45 ui-monospace,Menlo,Consolas,monospace;white-space:pre;
  overflow:auto;max-height:70vh;border:1px solid var(--e);border-radius:6px;
  padding:.5rem;background:#0001}
.bar{height:6px;background:var(--e);border-radius:3px;overflow:hidden;
  margin-top:.2rem}
.bar>i{display:block;height:100%;background:currentColor}
label{display:inline-flex;align-items:center;gap:.3rem;margin-right:1rem}
button{font:inherit;padding:.35rem .9rem;margin-right:.5rem}
</style>
<h1>LoRaITP <span id=hdr></span></h1>
<nav><a id=t_gallery onclick="show('gallery')">Images</a
><a id=t_status onclick="show('status')">Status</a
><a id=t_log onclick="show('log_v')">Live log</a
><a href="/settings">Settings</a></nav>

<p><button onclick="go()">Send / listen now</button>
<button onclick="if(confirm('Reboot the board?'))fetch('/api/reboot')">Reboot</button>
<span id=msg></span></p>
<div id=gallery><div class=grid id=grid></div></div>
<div id=status hidden><table id=stat></table></div>
<div id=log_v hidden>
  <label><input type=checkbox id=follow checked> follow</label>
  <label><input type=checkbox id=verbose> every frame (verbose)</label>
  <div id=log>connecting...</div>
</div>

<script>
let tab='gallery', since=0, lines=[];
function show(id){
  tab=id;
  for(const s of ['gallery','status','log_v'])
    document.getElementById(s).hidden=(s!==id);
  for(const s of ['gallery','status','log'])
    document.getElementById('t_'+s).className=
      ((s==='log'?'log_v':s)===id)?'on':'';
  if(id==='log_v') pollLog();
}
async function go(){
  await fetch('/api/trigger');
  document.getElementById('msg').textContent=' starting…';
  setTimeout(()=>{document.getElementById('msg').textContent='';},3000);
  show('log_v');
}
function fmt(ms){const s=Math.round(ms/1000);
  return s<90?s+' s':s<5400?(s/60).toFixed(1)+' min':(s/3600).toFixed(1)+' h';}

async function loadStatus(){
  const s=await (await fetch('/api/status')).json();
  document.getElementById('hdr').textContent=
    '— '+s.role+' — '+s.region+' — '+(s.freq/1e6).toFixed(3)+' MHz';
  const pct=s.budget_ms? Math.min(100,100*s.used_ms/s.budget_ms):0;
  const rows=[
    ['Role',s.role],
    ['Next transfer in',s.next_ms>0?fmt(s.next_ms):'now'],
    ['Last session',s.last||'—'],
    ['Region',s.region+' — '+(s.budget_ms?(s.duty+'% duty'):'no duty limit')],
    ['Frequency',(s.freq/1e6).toFixed(3)+' MHz, '+(s.bw/1000)+' kHz, SF'+s.sf+', 4/'+(s.cr+4)],
    ['TX power',s.power+' dBm'],
    ['Chunk / frame',s.chunk+' B payload, '+s.toa+' ms on air'],
    ['Airtime this hour',s.budget_ms
       ? s.used_ms+' of '+s.budget_ms+' ms ('+pct.toFixed(1)+'%)'
         +'<div class=bar><i style="width:'+pct+'%"></i></div>'
       : s.used_ms+' ms (no limit in this band)'],
    ['Image bytes still allowed',s.bytes_left>1e9?'unlimited':s.bytes_left],
    ['Antenna switch',s.rfsw],
    ['Last RSSI / SNR',s.rssi?(s.rssi+' dBm / '+s.snr.toFixed(2)+' dB'
        +' — margin '+s.margin.toFixed(1)+' dB over SF'+s.sf):'—'],
    ['Images stored',s.images+' of '+s.keep],
    ['Storage',(s.fs_used/1024).toFixed(0)+' of '+(s.fs_total/1024).toFixed(0)+' kB'],
    ['Free memory',(s.heap/1024).toFixed(0)+' kB'+
       (s.psram?' + '+(s.psram/1024/1024).toFixed(1)+' MB PSRAM':'')],
    ['Camera',s.camera],
    ['Uptime',fmt(s.uptime)],
    ['Board',s.board+' — '+s.version]];
  document.getElementById('stat').innerHTML=
    rows.map(r=>`<tr><th>${r[0]}</th><td>${r[1]}</td></tr>`).join('');
}

async function loadGallery(){
  const l=await (await fetch('/api/list')).json();
  document.getElementById('grid').innerHTML = l.length? l.map(i=>
    `<div class=card><a href="/img?n=${i.name}"><img loading=lazy
      src="/img?n=${i.name}"></a><small>${i.name}</small>
      <small>${i.bytes} B</small></div>`).join('')
    : '<p>No images yet. The status page says when the next transfer is due.</p>';
}

async function pollLog(){
  const v=document.getElementById('verbose').checked?1:0;
  const r=await (await fetch('/api/log?since='+since+'&v='+v)).json();
  since=r.next;
  if(r.lines.length){
    lines=lines.concat(r.lines).slice(-400);
    const el=document.getElementById('log');
    el.textContent=lines.map(l=>
      (l.ms/1000).toFixed(1).padStart(8)+'  '+l.t).join('\n');
    if(document.getElementById('follow').checked) el.scrollTop=el.scrollHeight;
  }
}
document.getElementById('verbose').onchange=()=>{
  fetch('/api/log?since='+since+'&v='+
    (document.getElementById('verbose').checked?1:0));};

function tick(){
  if(tab==='status') loadStatus();
  else if(tab==='gallery') loadGallery();
  else pollLog();
  fetch('/keepalive');
}
show('gallery'); loadStatus(); tick();
setInterval(()=>{ if(tab==='log_v') pollLog(); },1000);
setInterval(tick,5000);
</script>
)HTML";

static const char SETTINGS_HEAD[] PROGMEM = R"HTML(<!doctype html>
<meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>LoRaITP settings</title>
<style>
:root{color-scheme:light dark}
body{font:15px/1.5 system-ui,sans-serif;margin:0;padding:1rem;max-width:36rem}
label{display:block;margin:.6rem 0}
input,select{font:inherit;padding:.25rem}
fieldset{border:1px solid #8884;border-radius:6px;margin:1rem 0}
legend{padding:0 .4rem}
small{opacity:.75;display:block;margin-top:.3rem}
button{font:inherit;padding:.45rem 1.2rem;margin-top:1rem}
a{color:inherit}
</style>
<p><a href="/">&larr; back</a></p><h1>Settings</h1><form method=post>
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
    *s += "{\"name\":\"";  *s += name;
    *s += "\",\"bytes\":"; *s += bytes;
    *s += "}";
    return 0;
}

static void json_escape(String &out, const char *in)
{
    for (const char *p = in; *p; p++) {
        if (*p == '"' || *p == '\\') { out += '\\'; out += *p; }
        else if ((unsigned char)*p < 0x20) out += ' ';
        else out += *p;
    }
}

static void log_cb(void *user, uint32_t seq, uint32_t ms, const char *line)
{
    (void)seq;
    String *s = (String *)user;
    if (s->length() > 1)
        *s += ",";
    *s += "{\"ms\":"; *s += ms; *s += ",\"t\":\"";
    json_escape(*s, line);
    *s += "\"}";
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

static void h_log(void)
{
    touch();
    if (g_server.hasArg("v")) {
        uint8_t want = g_server.arg("v").toInt() ? LORAITP_LOG_VERBOSE
                                                 : LORAITP_LOG_NORMAL;
        if (want != loraitp_log_level())
            loraitp_log_set_level(want);
    }
    uint32_t since = g_server.hasArg("since")
                     ? (uint32_t)g_server.arg("since").toInt() : 0;

    String out = "{\"lines\":[";
    uint32_t next = loraitp_log_read(since, log_cb, &out);
    out += "],\"next\":"; out += next;
    out += ",\"level\":";  out += loraitp_log_level();
    out += "}";
    g_server.send(200, "application/json", out);
}

static void h_trigger(void)
{
    touch();
    if (g_trigger_cb)
        g_trigger_cb(g_status_user);
    LOG("transfer triggered from the web page");
    g_server.send(200, "text/plain", "ok");
}

static void h_reboot(void)
{
    touch();
    g_server.send(200, "text/plain", "rebooting");
    delay(200);
    ESP.restart();
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
    j += ",\"bw\":";          j += g_cfg->bandwidth_hz;
    j += ",\"cr\":";          j += g_cfg->coding_rate;
    j += ",\"sf\":";          j += g_cfg->spreading_factor;
    j += ",\"power\":";       j += g_cfg->tx_power_dbm;
    j += ",\"duty\":";        j += s.duty_percent;
    j += ",\"chunk\":";       j += s.chunk_len;
    j += ",\"toa\":";         j += s.frame_toa_ms;
    j += ",\"images\":";      j += loraitp_store_count(g_store);
    j += ",\"keep\":";        j += g_cfg->keep_images;
    j += ",\"used_ms\":";     j += s.airtime_used_ms;
    j += ",\"budget_ms\":";   j += s.airtime_budget_ms;
    j += ",\"bytes_left\":";  j += s.bytes_remaining;
    j += ",\"next_ms\":";     j += s.next_run_ms;
    j += ",\"rssi\":";        j += s.last_rssi_dbm;
    j += ",\"snr\":";         j += String(s.last_snr_qdb / 4.0, 2);
    j += ",\"margin\":";      j += String(s.link_margin_db, 1);
    j += ",\"heap\":";        j += (uint32_t)ESP.getFreeHeap();
    j += ",\"psram\":";       j += (uint32_t)ESP.getFreePsram();
    j += ",\"fs_used\":";     j += (uint32_t)LittleFS.usedBytes();
    j += ",\"fs_total\":";    j += (uint32_t)LittleFS.totalBytes();
    j += ",\"uptime\":";      j += millis();
    j += ",\"rfsw\":\"";
    j += g_cfg->rf_sw_invert ? "high = receive (inverted)" : "high = transmit";
    j += "\",\"camera\":\"";  j += s.camera;
    j += "\",\"board\":\"";   j += s.board;
    j += "\",\"version\":\""; j += s.version;
    j += "\",\"last\":\"";    json_escape(j, s.last_result);
    j += "\"}";
    g_server.send(200, "application/json", j);
}

/* ------------------------------------------------------- captive portal */

/*
 * Phones and laptops probe a known URL after joining a network. If the
 * answer is not what they expect, they decide the network wants a login
 * page and pop one up. Redirecting everything we do not recognise is
 * enough for iOS, Android and Windows alike, and saves having to tell
 * somebody standing in a field to type an IP address.
 */
static void h_notfound(void)
{
    touch();
    if (!g_cfg->captive_portal) {
        g_server.send(404, "text/plain", "not found");
        return;
    }
    g_server.sendHeader("Location", String("http://") + g_ip.toString() + "/",
                        true);
    g_server.send(302, "text/plain", "");
}

/* ---------------------------------------------------------- settings */

static void radio_btn(String &h, const char *name, const char *value,
                      const char *label, bool checked)
{
    h += "<label style='display:block'><input type=radio name=";
    h += name; h += " value="; h += value;
    h += checked ? " checked>" : ">";
    h += label; h += "</label>";
}

static void opt(String &h, uint32_t value, const char *label, bool sel)
{
    h += "<option value="; h += value;
    if (sel) h += " selected";
    h += ">"; h += label; h += "</option>";
}

static void h_settings_get(void)
{
    touch();
    String h = FPSTR(SETTINGS_HEAD);

    h += "<fieldset><legend>Role</legend>";
    radio_btn(h, "role", "1", "Sender — captures and transmits",
              g_cfg->role == LORAITP_ROLE_SENDER);
    radio_btn(h, "role", "0", "Receiver — listens and stores",
              g_cfg->role == LORAITP_ROLE_RECEIVER);
    h += "<small>The same firmware runs both ends; this only decides which "
         "one this board is.</small></fieldset>";

    h += "<fieldset><legend>Antenna switch</legend>";
    radio_btn(h, "rfinv", "0", "RF_SW high to transmit (default)",
              !g_cfg->rf_sw_invert);
    radio_btn(h, "rfinv", "1", "RF_SW high to receive (inverted)",
              g_cfg->rf_sw_invert);
    h += "<small>The Wio-SX1262 brings its antenna switch out to a pin "
         "instead of steering it from DIO2, and which way round it goes is "
         "a convention rather than something anyone measured on your "
         "module. <b>If the boards hear nothing at all, change this "
         "first.</b></small></fieldset>";

    h += "<fieldset><legend>Radio</legend><label>Region <select name=region>";
    static const uint8_t regions[] = { 4, 0, 1, 3, 5, 7 };
    for (unsigned i = 0; i < sizeof(regions); i++) {
        h += "<option value="; h += regions[i];
        if (regions[i] == g_cfg->region) h += " selected";
        h += ">"; h += loraitp_cfg_region_name(regions[i]); h += "</option>";
    }
    h += "</select></label>";
    h += "<small>EU868_G4_LP is 5 mW with no duty-cycle limit — the "
         "place to develop. EU868_G3 is 500 mW at 10%, where a real link "
         "belongs. The firmware refuses a frequency or power the region "
         "does not allow rather than transmitting anyway.</small>";

    h += "<label>Frequency, Hz <input name=freq value=";
    h += g_cfg->frequency_hz; h += "></label>";

    h += "<label>Bandwidth <select name=bw>";
    opt(h, 20830,  "20.8 kHz — slowest, most range", g_cfg->bandwidth_hz == 20830);
    opt(h, 62500,  "62.5 kHz", g_cfg->bandwidth_hz == 62500);
    opt(h, 125000, "125 kHz — default", g_cfg->bandwidth_hz == 125000);
    opt(h, 250000, "250 kHz — fastest, least range", g_cfg->bandwidth_hz == 250000);
    h += "</select></label>";
    h += "<small>Halving the bandwidth is worth about 3 dB of sensitivity "
         "and doubles the airtime — the same trade as one step of "
         "spreading factor, in smaller increments.</small>";

    h += "<label>Spreading factor <select name=sf>";
    for (int sf = 7; sf <= 12; sf++) {
        char lbl[48];
        snprintf(lbl, sizeof(lbl), "SF%d%s", sf,
                 sf == 7 ? " — fastest" : sf == 12 ? " — most range" : "");
        opt(h, (uint32_t)sf, lbl, g_cfg->spreading_factor == sf);
    }
    h += "</select></label>";

    h += "<label>Coding rate <select name=cr>";
    opt(h, 1, "4/5 — least overhead (default)", g_cfg->coding_rate == 1);
    opt(h, 2, "4/6", g_cfg->coding_rate == 2);
    opt(h, 3, "4/7", g_cfg->coding_rate == 3);
    opt(h, 4, "4/8 — most robust", g_cfg->coding_rate == 4);
    h += "</select></label>";

    h += "<label>TX power, dBm <input name=pwr size=4 value=";
    h += g_cfg->tx_power_dbm; h += "></label>";
    h += "<label>Sync word (hex) <input name=sync size=4 value=";
    char sw[8]; snprintf(sw, sizeof(sw), "%02X", g_cfg->sync_word); h += sw;
    h += "></label>";
    h += "<small>12 is the private-network value. 34 is LoRaWAN's and does "
         "not belong to us.</small>";
    h += "<label>Call sign (AMATEUR region only) <input name=call value=";
    h += g_cfg->callsign; h += "></label></fieldset>";

    h += "<fieldset><legend>Transfer</legend>";
    radio_btn(h, "bcast", "0", "Interactive — the receiver asks for what it missed",
              !g_cfg->broadcast);
    radio_btn(h, "bcast", "1", "Broadcast — one way, repaired by parity alone",
              g_cfg->broadcast);
    h += "<label>Parity, % of the image <input name=parity size=4 value=";
    h += g_cfg->parity_percent; h += "></label>";
    h += "<small>Broadcast needs parity and cannot work without it. Note "
         "what it buys: parity is a fraction of the image, but what it "
         "tolerates is r/(k+r) of the frames — so 25% survives 20% "
         "loss, not 25%.</small>";
    h += "<label>Interval between transfers, seconds <input name=iv size=8 value=";
    h += g_cfg->interval_s; h += "></label>";
    h += "<small>Sender only. A receiver listens continuously — pausing it "
         "between windows would make it deaf for part of every cycle, and "
         "with no shared clock the gap would land wherever it liked.</small>";
    h += "<label>Byte budget per image <input name=budget size=8 value=";
    h += g_cfg->image_budget; h += "></label>";
    h += "<label>Keep how many images <input name=keep size=6 value=";
    h += g_cfg->keep_images; h += "></label></fieldset>";

    h += "<fieldset><legend>Access point</legend>";
    radio_btn(h, "apoff", "0", "Stay on (default)", !g_cfg->ap_auto_off);
    radio_btn(h, "apoff", "1", "Switch off when idle", g_cfg->ap_auto_off);
    h += "<label>Idle timeout, seconds <input name=apto size=6 value=";
    h += g_cfg->ap_timeout_s; h += "></label>";
    h += "<label>Password (blank = open) <input name=appw value=";
    h += g_cfg->ap_password; h += "></label>";
    h += "<label><input type=checkbox name=cportal value=1";
    if (g_cfg->captive_portal) h += " checked";
    h += "> Pop the page up automatically on joining</label>";
    h += "<small>WiFi draws 100–150 mA continuously — more than the "
         "radio uses while transmitting. Switching it off matters on a "
         "battery and not at all on a bench.</small></fieldset>";

    h += "<fieldset><legend>Log</legend>";
    radio_btn(h, "log", "0", "Off", g_cfg->log_level == 0);
    radio_btn(h, "log", "1", "Normal — sessions, repairs, waits",
              g_cfg->log_level == 1);
    radio_btn(h, "log", "2", "Verbose — every single frame",
              g_cfg->log_level == 2);
    h += "<small>Verbose is what to use when nothing arrives and you need "
         "to know whether anything was heard. It is noisy: a transfer is "
         "fifty frames.</small></fieldset>";

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
    if (g_server.hasArg("bw"))     c.bandwidth_hz = (uint32_t)g_server.arg("bw").toInt();
    if (g_server.hasArg("cr"))     c.coding_rate = (uint8_t)g_server.arg("cr").toInt();
    if (g_server.hasArg("sf"))     c.spreading_factor = (uint8_t)g_server.arg("sf").toInt();
    if (g_server.hasArg("pwr"))    c.tx_power_dbm = (int8_t)g_server.arg("pwr").toInt();
    if (g_server.hasArg("sync"))
        c.sync_word = (uint8_t)strtoul(g_server.arg("sync").c_str(), NULL, 16);
    if (g_server.hasArg("bcast"))  c.broadcast = g_server.arg("bcast").toInt() != 0;
    if (g_server.hasArg("parity")) c.parity_percent = (uint8_t)g_server.arg("parity").toInt();
    if (g_server.hasArg("iv"))     c.interval_s = (uint32_t)g_server.arg("iv").toInt();
    if (g_server.hasArg("budget")) c.image_budget = (uint16_t)g_server.arg("budget").toInt();
    if (g_server.hasArg("keep"))   c.keep_images = (uint16_t)g_server.arg("keep").toInt();
    if (g_server.hasArg("rfinv"))  c.rf_sw_invert = g_server.arg("rfinv").toInt() != 0;
    if (g_server.hasArg("log"))    c.log_level = (uint8_t)g_server.arg("log").toInt();
    c.captive_portal = g_server.hasArg("cportal");
    if (g_server.hasArg("appw"))
        snprintf(c.ap_password, sizeof(c.ap_password), "%s",
                 g_server.arg("appw").c_str());
    if (g_server.hasArg("call"))
        snprintf(c.callsign, sizeof(c.callsign), "%s",
                 g_server.arg("call").c_str());

    /* Broadcast without parity cannot work, and the core would refuse the
     * session with an error the page has no room to explain. Fix it here
     * instead of shipping a configuration that fails on the next tick. */
    if (c.broadcast && c.parity_percent == 0)
        c.parity_percent = 30;

    loraitp_cfg_save(&c);
    g_server.send(200, "text/html",
                  "<meta charset=utf-8><meta http-equiv=refresh content='3;url=/'>"
                  "<p style='font:15px system-ui;padding:1rem'>Saved. Restarting…");
    delay(300);
    ESP.restart();
}

/* ------------------------------------------------------------- public */

void loraitp_webui_begin(loraitp_appcfg_t *cfg, loraitp_store_t *store,
                         loraitp_webui_status_cb cb,
                         loraitp_webui_trigger_cb trigger, void *user)
{
    g_cfg = cfg;
    g_store = store;
    g_status_cb = cb;
    g_trigger_cb = trigger;
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
        LOG("access point failed to start");
        return;
    }
    g_ip = WiFi.softAPIP();

    g_server.on("/", h_root);
    g_server.on("/keepalive", h_keepalive);
    g_server.on("/api/list", h_list);
    g_server.on("/api/status", h_status);
    g_server.on("/api/log", h_log);
    g_server.on("/api/trigger", h_trigger);
    g_server.on("/api/reboot", h_reboot);
    g_server.on("/img", h_img);
    g_server.on("/meta", h_meta);
    g_server.on("/settings", HTTP_GET, h_settings_get);
    g_server.on("/settings", HTTP_POST, h_settings_post);
    g_server.onNotFound(h_notfound);
    g_server.begin();

    if (cfg->captive_portal) {
        /* Answer every name with our own address, so the probe the phone
         * makes lands here rather than failing to resolve. */
        g_dns.setErrorReplyCode(DNSReplyCode::NoError);
        g_dns_running = g_dns.start(53, "*", g_ip);
    }

    g_running = true;
    touch();
    LOG("access point %s at %s%s", ssid, g_ip.toString().c_str(),
        g_dns_running ? " (captive portal on)" : "");
}

void loraitp_webui_stop(void)
{
    if (!g_running)
        return;
    if (g_dns_running) { g_dns.stop(); g_dns_running = false; }
    g_server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    g_running = false;
    LOG("access point down");
}

bool loraitp_webui_running(void) { return g_running; }

void loraitp_webui_poll(void)
{
    if (!g_running)
        return;
    if (g_dns_running)
        g_dns.processNextRequest();
    g_server.handleClient();

    if (g_cfg->ap_auto_off && g_cfg->ap_timeout_s > 0) {
        uint32_t idle = millis() - g_last_activity;
        if (idle > (uint32_t)g_cfg->ap_timeout_s * 1000u)
            loraitp_webui_stop();
    }
}
