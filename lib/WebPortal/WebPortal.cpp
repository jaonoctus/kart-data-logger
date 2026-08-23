#include "WebPortal.h"
#include "portal_page.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>

#ifndef WEB_PORTAL_SSID
#define WEB_PORTAL_SSID "kart-dash"
#endif
#ifndef WEB_PORTAL_PASS
#define WEB_PORTAL_PASS "kartdash"     /* WPA2 needs >= 8 chars */
#endif
#ifndef WEB_PORTAL_HOST
#define WEB_PORTAL_HOST "kart"         /* http://kart.local */
#endif

static WebServer  s_server(80);
static DNSServer  s_dns;
static File       s_upload;
static bool       s_routesBound = false;

/* Streaming chunk for downloads. Big enough to keep WiFi busy, small enough to
 * stay off the heap's back while LVGL is drawing from PSRAM. */
#define WP_CHUNK 2048

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */
static String jsonEscape(const String &in) {
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if ((uint8_t)c < 0x20) continue;
        else out += c;
    }
    return out;
}

/* Reject anything that could climb out of the card root. */
static bool safePath(const String &p) {
    if (p.length() < 2 || p[0] != '/') return false;
    if (p.indexOf("..") >= 0) return false;
    return true;
}

static const char *mimeFor(const String &p) {
    String l = p; l.toLowerCase();
    if (l.endsWith(".csv"))  return "text/csv";
    if (l.endsWith(".ini"))  return "text/plain";
    if (l.endsWith(".txt"))  return "text/plain";
    if (l.endsWith(".json")) return "application/json";
    return "application/octet-stream";
}

/* ---------------------------------------------------------------------------
 * Handlers
 * ------------------------------------------------------------------------- */
/* Every request in and out, with how long it took. Without this the failure
 * mode is a browser spinner and no way to tell a slow handler from a request
 * that never arrived. */
static uint32_t s_reqStart = 0;
static void reqIn(const char *what) {
    s_reqStart = millis();
    log_i("HTTP  <- %s %s  from %s", what, s_server.uri().c_str(),
          s_server.client().remoteIP().toString().c_str());
}
static void reqOut(const char *what, size_t bytes) {
    log_i("HTTP  -> %s  %u bytes in %lu ms", what, (unsigned)bytes,
          (unsigned long)(millis() - s_reqStart));
}

static void hRoot() {
    reqIn("GET");
    s_server.sendHeader("Cache-Control", "no-store");
    size_t n = strlen_P(PORTAL_PAGE);
    s_server.send_P(200, "text/html", PORTAL_PAGE);
    reqOut("page", n);
}

/* Deliberately cheap. cardSize() reads the CSD register; totalBytes() and
 * usedBytes() both go through f_getfree(), which on FAT32 with a stale FSInfo
 * sector rescans the entire FAT — seconds to tens of seconds on a big card.
 * This is the first request the page makes, and doing that here blocked the
 * single-threaded server hard enough that the page never finished loading.
 * The slow query lives at /api/space and is only run when asked for. */
static void hInfo() {
    reqIn("GET");
    String j = "{\"card\":" + String((uint32_t)(SD_MMC.cardSize() / 1048576ULL)) +
               ",\"clients\":" + String(WiFi.softAPgetStationNum()) + "}";
    s_server.send(200, "application/json", j);
    reqOut("info", j.length());
}

static void hSpace() {
    reqIn("GET");
    uint64_t total = SD_MMC.totalBytes();      /* may take a while — see above */
    uint64_t used  = SD_MMC.usedBytes();
    String j = "{\"total\":" + String((uint32_t)(total / 1048576ULL)) +
               ",\"used\":"  + String((uint32_t)(used  / 1048576ULL)) + "}";
    s_server.send(200, "application/json", j);
    reqOut("space", j.length());
}

static void hList() {
    reqIn("GET");
    String path = s_server.hasArg("path") ? s_server.arg("path") : "/";
    if (path.length() == 0) path = "/";
    if (path != "/" && !safePath(path)) { s_server.send(400, "text/plain", "bad path"); return; }

    File dir = SD_MMC.open(path);
    if (!dir || !dir.isDirectory()) { s_server.send(404, "text/plain", "not a directory"); return; }

    String j = "[";
    bool first = true;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        String name = f.name();
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        if (name.startsWith(".")) { f.close(); continue; }   /* skip dotfiles */
        if (!first) j += ",";
        first = false;
        /* getLastWrite() is only meaningful once LogManager::syncClockFromTelemetry()
         * has run: the ESP32 boots at epoch 0, so anything the card received before
         * the first GPS fix is stamped 1970. Sent raw — the page decides what to do
         * with a pre-2021 value rather than this handler inventing a placeholder. */
        j += "{\"name\":\"" + jsonEscape(name) + "\",\"size\":" + String((uint32_t)f.size()) +
             ",\"mtime\":" + String((uint32_t)f.getLastWrite()) +
             ",\"dir\":" + String(f.isDirectory() ? "true" : "false") + "}";
        f.close();
    }
    dir.close();
    j += "]";
    s_server.send(200, "application/json", j);
    reqOut("list", j.length());
}

static void hFile() {
    reqIn("GET");
    String path = s_server.arg("path");
    if (!safePath(path)) { s_server.send(400, "text/plain", "bad path"); return; }

    File f = SD_MMC.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        log_w("HTTP  -- %s not found", path.c_str());
        s_server.send(404, "text/plain", "not found");
        return;
    }
    size_t sz = f.size();

    /* streamFile chunks internally, so a 600 kB log does not have to be
     * materialised in RAM. This runs on the portal task, well away from the
     * loopTask watchdog. */
    s_server.sendHeader("Cache-Control", "no-store");
    s_server.streamFile(f, mimeFor(path));
    f.close();
    reqOut(path.c_str(), sz);
}

static void hDelete() {
    reqIn("GET");
    String path = s_server.arg("path");
    if (!safePath(path)) { s_server.send(400, "text/plain", "bad path"); return; }
    bool ok = SD_MMC.remove(path);
    s_server.send(ok ? 200 : 500, "text/plain", ok ? "deleted" : "delete failed");
    if (ok) log_w("WebPortal: deleted %s", path.c_str());
}

static void hUploadDone() {
    s_server.send(200, "text/plain", "ok");
}

static void hUpload() {
    HTTPUpload &up = s_server.upload();
    if (up.status == UPLOAD_FILE_START) {
        String name = up.filename;
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        if (name.length() == 0) return;
        String path = "/" + name;
        SD_MMC.remove(path);
        s_upload = SD_MMC.open(path, FILE_WRITE);
        log_i("WebPortal: upload %s", path.c_str());
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (s_upload) s_upload.write(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
        if (s_upload) { s_upload.close(); }
        log_i("WebPortal: upload %s (%u bytes)",
              up.status == UPLOAD_FILE_END ? "complete" : "aborted", (unsigned)up.totalSize);
    }
}

/* With the captive portal enabled, bounce unknown paths at the app so it pops
 * up on joining. Without it, answer 404 immediately: a redirect invites the
 * client to come back and ask again, and the OS's connectivity probes will
 * happily do that forever. */
static void hNotFound() {
    /* Worth seeing: a flood here is the host's connectivity probes or a
     * background app treating the dash as its internet connection. */
    log_w("HTTP  ?? %s %s from %s",
          s_server.method() == HTTP_GET ? "GET" : "POST",
          s_server.uri().c_str(), s_server.client().remoteIP().toString().c_str());
#if defined(WEB_PORTAL_CAPTIVE)
    s_server.sendHeader("Location", "http://" WEB_PORTAL_HOST ".local/", true);
    s_server.send(302, "text/plain", "");
#else
    s_server.send(404, "text/plain", "not found");
#endif
}

void WebPortal::routes() {
    if (s_routesBound) return;
    s_server.on("/",            HTTP_GET,  hRoot);
    s_server.on("/api/info",    HTTP_GET,  hInfo);
    s_server.on("/api/space",   HTTP_GET,  hSpace);
    s_server.on("/api/list",    HTTP_GET,  hList);
    s_server.on("/api/file",    HTTP_GET,  hFile);
    s_server.on("/api/delete",  HTTP_GET,  hDelete);
    s_server.on("/api/upload",  HTTP_POST, hUploadDone, hUpload);
    s_server.onNotFound(hNotFound);
    s_routesBound = true;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */
bool WebPortal::start() {
    if (_running) return true;

    snprintf(_ssid, sizeof(_ssid), "%s", WEB_PORTAL_SSID);

    /* WiFi wants ~50 kB of INTERNAL DRAM and cannot fall back to PSRAM, so this
     * is the number that decides whether the portal can come up at all. Logged
     * every time: an ESP_ERR_NO_MEM from esp_wifi_init is otherwise a very
     * opaque way to discover the heap is full. */
    size_t freeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t bigInt  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    log_w("WebPortal: internal heap free=%u largest=%u", (unsigned)freeInt, (unsigned)bigInt);

    /* Association is a separate step from anything HTTP: if these never appear,
     * the problem is joining the AP, not the server. */
    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t) {
        log_w("WiFi  ++ station joined (%u total)", (unsigned)WiFi.softAPgetStationNum());
    }, ARDUINO_EVENT_WIFI_AP_STACONNECTED);
    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t) {
        log_w("WiFi  -- station left (%u total)", (unsigned)WiFi.softAPgetStationNum());
    }, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);

    WiFi.mode(WIFI_AP);
    /* Modem power save adds tens to hundreds of ms of latency to every packet —
     * on a request/response protocol like HTTP that compounds into a page that
     * never seems to finish loading. The portal is a plugged-in, deliberate
     * activity, so trade the power for responsiveness. */
    WiFi.setSleep(false);
    /* Cap associations: each client the OS opens costs buffers out of the same
     * scarce internal DRAM, and only one machine is ever pulling files. */
    if (!WiFi.softAP(_ssid, WEB_PORTAL_PASS, 1 /*channel*/, 0 /*hidden*/, 2 /*max*/)) {
        log_e("WebPortal: softAP failed (internal heap free=%u largest=%u). "
              "WiFi needs ~50 kB of internal DRAM — is the BLE stack still up?",
              (unsigned)freeInt, (unsigned)bigInt);
        WiFi.mode(WIFI_OFF);
        return false;
    }

    IPAddress ip = WiFi.softAPIP();

    /* Captive portal is OFF by default, and that is a performance decision, not
     * an oversight. Answering every DNS query with our own address makes the
     * host treat us as its internet connection: connectivity checks and every
     * background app then hammer this single-threaded server, which is enough
     * to stop the real page ever loading. Enable only if you want the
     * auto-popup and can live with the noise. */
#if defined(WEB_PORTAL_CAPTIVE)
    s_dns.setErrorReplyCode(DNSReplyCode::NoError);
    s_dns.start(53, "*", ip);
#endif
    if (MDNS.begin(WEB_PORTAL_HOST)) MDNS.addService("http", "tcp", 80);

    routes();
    s_server.begin();

    _stopWanted = false;
    _running    = true;

    if (!_task) {
        /* Priority 5 deliberately outranks the LVGL task (4), which blits the
         * whole 320x480 canvas over QSPI on every refresh — ~300 kB, tens of ms
         * of bus and CPU — and was starving the HTTP server at a lower
         * priority. LVGL has no affinity, so this still wins wherever it runs.
         *
         * CORE 0 IS LOAD-BEARING. loopTask lives on core 1 at priority 1 and is
         * the only subscriber to the 5 s task watchdog. Putting a hot priority-5
         * task on core 1 starved it into a reset every few seconds, which looks
         * from outside like the AP flapping. Keep this off core 1. */
        xTaskCreatePinnedToCore(taskTrampoline, "webportal", 8192, this, 5, &_task, 0);
    }

    /* Repeated deliberately, and after a pause. Serial.setTxTimeoutMs(0) drops
     * bytes when the CDC buffer is full, and this is exactly the line that gets
     * lost during the burst of WiFi driver output — leaving no way to tell a
     * portal that came up from one that failed. */
    for (int i = 0; i < 3; i++) {
        log_w("WebPortal: UP  ssid='%s'  http://%s  (%s.local)  heap_internal=%u",
              _ssid, ip.toString().c_str(), WEB_PORTAL_HOST,
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    return true;
}

void WebPortal::stop() {
    if (!_running) return;
    _stopWanted = true;               /* let the task leave handleClient() */
    vTaskDelay(pdMS_TO_TICKS(120));

    s_server.stop();
    s_dns.stop();
    MDNS.end();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);

    _running = false;
    log_w("WebPortal: down, radio off");
}

bool WebPortal::toggle() {
    if (_running) { stop(); return false; }
    return start();
}

uint8_t WebPortal::clients() const {
    return _running ? WiFi.softAPgetStationNum() : 0;
}

String WebPortal::ip() const {
    return _running ? WiFi.softAPIP().toString() : String();
}

void WebPortal::taskTrampoline(void *arg) {
    static_cast<WebPortal *>(arg)->task();
}

void WebPortal::task() {
    for (;;) {
        if (_running && !_stopWanted) {
#if defined(WEB_PORTAL_CAPTIVE)
            s_dns.processNextRequest();
#endif
            s_server.handleClient();
            /* Spin tightly only while someone is actually associated:
             * handleClient() services one client interaction per call and a
             * browser opens several connections per page, so sleeping between
             * them multiplies latency by every round trip. With nobody joined
             * there is nothing to be quick for, and a hot priority-5 task is
             * worth avoiding — that is what caused the watchdog resets. */
            vTaskDelay(WiFi.softAPgetStationNum() ? 1 : pdMS_TO_TICKS(20));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
