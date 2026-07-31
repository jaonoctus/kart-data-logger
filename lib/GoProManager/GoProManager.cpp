#include "GoProManager.h"
#include <NimBLEDevice.h>

/* ---------------------------------------------------------------------------
 * Protocol constants (GoPro Control & Query service).
 * Verified against a HERO8 Black GATT dump; identical on HERO9+ where GoPro
 * documents them as the Open GoPro BLE API.
 * ------------------------------------------------------------------------- */
static const NimBLEUUID SVC_CONTROL_QUERY("0000fea6-0000-1000-8000-00805f9b34fb");
static const NimBLEUUID CH_CMD_REQ       ("b5f90072-aa8d-11e3-9046-0002a5d5c51b");
static const NimBLEUUID CH_CMD_RSP       ("b5f90073-aa8d-11e3-9046-0002a5d5c51b");
static const NimBLEUUID CH_SET_REQ       ("b5f90074-aa8d-11e3-9046-0002a5d5c51b");
static const NimBLEUUID CH_SET_RSP       ("b5f90075-aa8d-11e3-9046-0002a5d5c51b");
static const NimBLEUUID CH_QUERY_REQ     ("b5f90076-aa8d-11e3-9046-0002a5d5c51b");
static const NimBLEUUID CH_QUERY_RSP     ("b5f90077-aa8d-11e3-9046-0002a5d5c51b");

/* Command requests — [len][cmdId][paramLen][param...] */
static const uint8_t CMD_SHUTTER_ON [] = { 0x03, 0x01, 0x01, 0x01 };
static const uint8_t CMD_SHUTTER_OFF[] = { 0x03, 0x01, 0x01, 0x00 };

/* Query requests — [len][queryId] */
static const uint8_t QRY_GET_STATUS  [] = { 0x01, 0x13 };   /* one-shot full dump */
static const uint8_t QRY_REG_UPDATES [] = { 0x01, 0x53 };   /* push on change     */

/* Preset group — [len][0x3E][paramLen][group BE]. 1000 = video, 1001 = photo,
 * 1002 = timelapse. This is the HERO9+ way to say "be a video camera"; it is
 * sent after the legacy mode commands so it wins where it is supported. A camera
 * that does not know the command answers with a non-zero status, which we log,
 * so trying it costs nothing but a round trip. */
static const uint8_t CMD_PRESET_GROUP_VIDEO[] = { 0x04, 0x3E, 0x02, 0x03, 0xE8 };

/* Preset status is protobuf-framed, not TLV: [len][featureId][actionId]. The
 * reply carries the available presets and their title *enums* — GoPro does not
 * put the user's custom name ("Motor") on the wire in any documented form. We
 * hex-dump whatever comes back rather than pretending to parse protobuf, since
 * the point is to find out whether HERO8 answers this at all. */
static const uint8_t QRY_PRESET_STATUS[] = { 0x02, 0xF5, 0x72 };

/* Load a specific preset by ID — [len][0x40][paramLen][id BE32]. Populated from
 * whatever the preset-status dump reveals. */
#if defined(GOPRO_PRESET_ID)
static const uint8_t CMD_LOAD_PRESET[] = {
    0x06, 0x40, 0x04,
    (uint8_t)((GOPRO_PRESET_ID) >> 24), (uint8_t)((GOPRO_PRESET_ID) >> 16),
    (uint8_t)((GOPRO_PRESET_ID) >> 8),  (uint8_t)(GOPRO_PRESET_ID)
};
#endif
static const uint8_t QRY_GET_SETTINGS[] = { 0x01, 0x12 };   /* current settings   */

/* Force the camera into plain video before the shutter is rolled.
 *
 * The camera remembers whatever mode it was last left in, so a session started
 * after someone browsed to TimeLapse records a timelapse — which is exactly
 * how a race got recorded as 773 stills instead of video. Sending these on
 * every connect makes that impossible regardless of what state the camera was
 * left in.
 *
 *   mode    [len][0x02][paramLen][0=video 1=photo 2=multishot]
 *   submode [len][0x03][paramLen][mode][paramLen][sub]
 *           video submodes: 0 = single (normal video), 1 = timelapse
 *
 * Verified against a HERO8 Black capture; identical on HERO9+, which also
 * accepts the newer preset commands this deliberately does not use — these
 * work on both. */
static const uint8_t CMD_MODE_VIDEO   [] = { 0x03, 0x02, 0x01, 0x00 };
static const uint8_t CMD_SUBMODE_VIDEO[] = { 0x05, 0x03, 0x01, 0x00, 0x01, 0x00 };

/* Keep-alive: setting 0x5B written with the out-of-range sentinel 0x42. GoPro
 * uses a deliberately invalid LED value so the camera treats it as a heartbeat
 * rather than a settings change. Only sent while we want the camera recording,
 * so an idle camera is still free to auto-sleep and save its battery. */
static const uint8_t SET_KEEP_ALIVE[] = { 0x03, 0x5B, 0x01, 0x42 };

/* Status IDs we care about (same numbering on HERO8 and HERO9+). */
#define ST_BUSY        0x08
#define ST_ENCODING    0x0A
#define ST_REC_SECONDS 0x0D
#define ST_GPS_LOCK    0x44
#define ST_BATTERY_PCT 0x46

#define GOPRO_SCAN_MS          5000
#define GOPRO_RETRY_ACTIVE_MS  3000    /* only ever retried while a session runs */
#define GOPRO_KEEPALIVE_MS     3000
#define GOPRO_SHUTTER_GAP_MS   1500    /* don't spam shutter while it settles  */

/* ========================================================================= */

bool GoProManager::begin(const char *nameFilter) {
    if (nameFilter) {
        strncpy(m_nameFilter, nameFilter, sizeof(m_nameFilter) - 1);
        m_nameFilter[sizeof(m_nameFilter) - 1] = '\0';
    }

    NimBLEDevice::init("kart-dash");
    /* The camera requires a bonded link. Bonding, no MITM, secure connections —
     * keys land in NVS so this is a one-time dance with the camera in
     * Preferences > Connections > Connect Device > GoPro App. */
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setPower(9);

    BaseType_t ok = xTaskCreatePinnedToCore(
        taskTrampoline, "gopro", 6144, this, 3, nullptr, 0);
    if (ok != pdPASS) {
        log_e("GoPro: worker task creation failed");
        return false;
    }
    log_i("GoPro: BLE manager started%s%s",
          m_nameFilter[0] ? ", filter=" : "", m_nameFilter[0] ? m_nameFilter : "");
    return true;
}

void GoProManager::setRecording(bool on) {
    m_desiredRec = on;
}

GoProStatus GoProManager::status() const {
    GoProStatus s;
    portENTER_CRITICAL(&m_mux);
    s = m_st;
    portEXIT_CRITICAL(&m_mux);
    return s;
}

/* ========================================================================= */

void GoProManager::taskTrampoline(void *arg) {
    static_cast<GoProManager *>(arg)->task();
}

void GoProManager::task() {
    for (;;) {
        /* Print anything the notify callback parked for us. Done here because
         * this task can afford to stall between chunks; the callback cannot. */
        drainDump();

        uint32_t now = millis();
        NimBLEClient *client = static_cast<NimBLEClient *>(m_client);

        bool linked  = client && client->isConnected() && m_chCmd && m_chQuery;
        bool wantRec = m_desiredRec;

        if (linked) {
            portENTER_CRITICAL(&m_mux);
            bool isRec = m_st.recording;
            portEXIT_CRITICAL(&m_mux);

            /* Reconcile desired vs. actual shutter state.
             *
             * Deliberately NOT gated on status 8. Open GoPro tells HERO9+
             * clients to wait for "system busy" to clear before sending
             * commands, but on HERO8 status 8 tracks recording itself — it is
             * set for the whole clip, so gating on it would block the stop
             * command forever. The camera accepts shutter-off mid-encode; that
             * is the whole point of the command. */
            if (wantRec != isRec &&
                (now - m_lastShutterMs >= GOPRO_SHUTTER_GAP_MS)) {
                m_lastShutterMs = now;
                const uint8_t *cmd = wantRec ? CMD_SHUTTER_ON : CMD_SHUTTER_OFF;
                if (writeCmd(cmd, 4)) {
                    log_i("GoPro: shutter %s", wantRec ? "START" : "STOP");
                } else {
                    log_w("GoPro: shutter write failed");
                }
            } else if (!wantRec && !isRec) {
                /* Session over and the camera has confirmed it stopped. Drop the
                 * link so the GoPro is free to hit its own auto-off timer — and
                 * so a phone can pair to pull the footage. */
                log_i("GoPro: session over, releasing link");
                client->disconnect();
                teardown();
            } else if (wantRec && (now - m_lastKeepAliveMs >= GOPRO_KEEPALIVE_MS)) {
                /* Heartbeat only while the camera is meant to be working. */
                m_lastKeepAliveMs = now;
                NimBLERemoteCharacteristic *set =
                    static_cast<NimBLERemoteCharacteristic *>(m_chSet);
                if (set) set->writeValue(SET_KEEP_ALIVE, sizeof(SET_KEEP_ALIVE), true);
            }

            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Drop anything half-open before retrying: a client that is still
         * connected but whose characteristics never resolved would otherwise
         * reject every reconnect and wedge the loop. */
        if (m_chCmd || m_chQuery) {
            if (client && client->isConnected()) client->disconnect();
            teardown();
        } else if (client && client->isConnected()) {
            client->disconnect();
        }

        /* Nothing wants the camera — stay off the air completely. Connecting is
         * precisely what wakes a sleeping GoPro, so scanning while idle would
         * defeat the point: the camera must stay asleep until a session starts. */
        if (!wantRec) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (now - m_lastAttemptMs >= GOPRO_RETRY_ACTIVE_MS) {
            m_lastAttemptMs = now;
            if (connectCamera()) log_i("GoPro: linked");
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

bool GoProManager::connectCamera() {
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(80);

    NimBLEScanResults results = scan->getResults(GOPRO_SCAN_MS, false);

    const NimBLEAdvertisedDevice *target = nullptr;
    for (const NimBLEAdvertisedDevice *dev : results) {
        if (!dev) continue;

        bool isGoPro = dev->isAdvertisingService(SVC_CONTROL_QUERY);
        std::string name = dev->getName();
        if (!isGoPro && name.rfind("GoPro", 0) == 0) isGoPro = true;
        if (!isGoPro) continue;

        if (m_nameFilter[0] && name.find(m_nameFilter) == std::string::npos) continue;

        target = dev;
        break;
    }

    if (!target) {
        scan->clearResults();
        return false;
    }

    log_i("GoPro: found '%s' @ %s", target->getName().c_str(),
          target->getAddress().toString().c_str());

    NimBLEClient *client = static_cast<NimBLEClient *>(m_client);
    if (!client) {
        client = NimBLEDevice::createClient();
        if (!client) { scan->clearResults(); return false; }
        client->setConnectionParams(12, 24, 0, 400);
        m_client = client;
    }

    bool connected = client->connect(target);
    scan->clearResults();
    if (!connected) {
        log_w("GoPro: connect failed");
        return false;
    }

    NimBLERemoteService *svc = client->getService(SVC_CONTROL_QUERY);
    if (!svc) {
        log_w("GoPro: control/query service missing");
        client->disconnect();
        return false;
    }

    NimBLERemoteCharacteristic *cmdReq   = svc->getCharacteristic(CH_CMD_REQ);
    NimBLERemoteCharacteristic *cmdRsp   = svc->getCharacteristic(CH_CMD_RSP);
    NimBLERemoteCharacteristic *setReq   = svc->getCharacteristic(CH_SET_REQ);
    NimBLERemoteCharacteristic *setRsp   = svc->getCharacteristic(CH_SET_RSP);
    NimBLERemoteCharacteristic *queryReq = svc->getCharacteristic(CH_QUERY_REQ);
    NimBLERemoteCharacteristic *queryRsp = svc->getCharacteristic(CH_QUERY_RSP);

    if (!cmdReq || !queryReq || !queryRsp) {
        log_w("GoPro: required characteristics missing");
        client->disconnect();
        return false;
    }

    m_acc.active = false;
    m_acc.len    = 0;

    auto cmdCb = [this](NimBLERemoteCharacteristic *, uint8_t *data,
                        size_t len, bool) { this->onNotify(CHAN_CMD, data, len); };
    auto setCb = [this](NimBLERemoteCharacteristic *, uint8_t *data,
                        size_t len, bool) { this->onNotify(CHAN_SET, data, len); };
    auto qryCb = [this](NimBLERemoteCharacteristic *, uint8_t *data,
                        size_t len, bool) { this->onNotify(CHAN_QUERY, data, len); };

    /* Command and settings responses carry an ack we actually want to see: it is
     * the only way to tell "the camera refused this" from "the camera took it
     * and just did not repaint its own screen". */
    if (cmdRsp) cmdRsp->subscribe(true, cmdCb, true);
    if (setRsp) setRsp->subscribe(true, setCb, true);
    if (!queryRsp->subscribe(true, qryCb, true)) {
        log_w("GoPro: query subscribe failed");
        client->disconnect();
        return false;
    }

    m_chCmd   = cmdReq;
    m_chSet   = setReq;
    m_chQuery = queryReq;

    /* One full status dump to prime the dash, then register for pushes so the
     * camera tells us about battery / GPS / recording changes unprompted. */
    queryReq->writeValue(QRY_GET_STATUS,  sizeof(QRY_GET_STATUS),  true);
    queryReq->writeValue(QRY_REG_UPDATES, sizeof(QRY_REG_UPDATES), true);

    /* Plain video, every time. Legacy HERO5-8 mode commands first, then the
     * HERO9+ preset group, so whichever the camera actually implements is the
     * one that lands last. Watch the acks to see which. */
    cmdReq->writeValue(CMD_MODE_VIDEO,          sizeof(CMD_MODE_VIDEO),          true);
    cmdReq->writeValue(CMD_SUBMODE_VIDEO,       sizeof(CMD_SUBMODE_VIDEO),       true);
    cmdReq->writeValue(CMD_PRESET_GROUP_VIDEO,  sizeof(CMD_PRESET_GROUP_VIDEO),  true);
    log_w("GoPro: forced video mode (guards against a remembered timelapse)");

#if defined(GOPRO_PRESET_ID)
    cmdReq->writeValue(CMD_LOAD_PRESET, sizeof(CMD_LOAD_PRESET), true);
    log_w("GoPro: loading preset %u", (unsigned)(GOPRO_PRESET_ID));
#endif

    /* Ask what presets exist. Answered only if this camera speaks the protobuf
     * side of the API — the reply (or the silence) is the actual experiment. */
    queryReq->writeValue(QRY_PRESET_STATUS, sizeof(QRY_PRESET_STATUS), true);

    /* Optional profile. Settings are [len][id][paramLen][value] on the settings
     * characteristic. The value codes are per-model enums, not raw numbers, so
     * they are not hard-coded here — GOPRO_SETTINGS carries whatever id/value
     * pairs you have confirmed for your camera, e.g.
     *
     *   -D GOPRO_SETTINGS="83,1, 2,9, 3,8"     GPS on, resolution, fps
     *
     * Use the settings dump logged below to discover the codes: put the camera
     * in the profile you want by hand, connect, and read off the id/value pairs
     * it reports. */
#if defined(GOPRO_SETTINGS)
    if (setReq) {
        static const uint8_t prof[] = { GOPRO_SETTINGS };
        for (size_t i = 0; i + 1 < sizeof(prof); i += 2) {
            const uint8_t s[4] = { 0x03, prof[i], 0x01, prof[i + 1] };
            setReq->writeValue(s, sizeof(s), true);
            log_i("GoPro: setting %u = %u", (unsigned)prof[i], (unsigned)prof[i + 1]);
        }
    }
#endif

    /* Ask for the current settings so the log shows the camera's actual
     * profile as id/value pairs — the only reliable way to learn this model's
     * enum codes before pinning them with GOPRO_SETTINGS. */
    queryReq->writeValue(QRY_GET_SETTINGS, sizeof(QRY_GET_SETTINGS), true);

    portENTER_CRITICAL(&m_mux);
    m_st.linked = true;
    portEXIT_CRITICAL(&m_mux);

    return true;
}

void GoProManager::teardown() {
    m_chCmd = m_chSet = m_chQuery = nullptr;
    m_acc.active = false;
    m_acc.len    = 0;

    portENTER_CRITICAL(&m_mux);
    m_st.linked     = false;
    m_st.busy       = false;
    m_st.recording  = false;
    m_st.recSeconds = 0;
    portEXIT_CRITICAL(&m_mux);

    log_i("GoPro: link lost");
}

/* Callback-side half of the deferred dump: copy and get out. Never logs. */
void GoProManager::stashDump(uint8_t id, const uint8_t *p, uint16_t len) {
    if (m_dump.ready) return;               /* previous one not printed yet */
    uint16_t n = len < sizeof(m_dump.buf) ? len : sizeof(m_dump.buf);
    memcpy(m_dump.buf, p, n);
    m_dump.len   = n;
    m_dump.id    = id;
    m_dump.ready = true;
}

/* Task-side half: safe to stall here, so the payload goes out in small pieces
 * that the CDC buffer can actually carry. */
void GoProManager::drainDump() {
    if (!m_dump.ready) return;

    uint8_t        id  = m_dump.id;
    const uint8_t *p   = m_dump.buf;
    uint16_t       len = m_dump.len;

    if (id == 0x12) {
        /* Settings dump: id=value pairs, eight per line. */
        String s;
        uint8_t onLine = 0;
        log_w("GoPro settings (id=value):");
        for (uint16_t i = 2; i + 2 <= len; ) {
            uint8_t sid = p[i], vl = p[i + 1];
            if (i + 2 + vl > len) break;
            if (vl >= 1 && vl <= 4) {
                uint32_t v = 0;
                for (uint8_t k = 0; k < vl; k++) v = (v << 8) | p[i + 2 + k];
                s += String(sid); s += "="; s += String(v); s += " ";
                if (++onLine == 8) {
                    log_w("  %s", s.c_str());
                    s = ""; onLine = 0;
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
            }
            i += 2 + vl;
        }
        if (onLine) log_w("  %s", s.c_str());
    } else {
        /* Anything else (notably the 0xF5 preset reply) goes out raw, 16 bytes
         * per line, so it can be decoded off the log by hand. */
        log_w("GoPro: reply id=0x%02X len=%u (raw):", id, (unsigned)len);
        for (uint16_t i = 0; i < len; i += 16) {
            char line[64];
            int  o = 0;
            for (uint16_t k = i; k < len && k < i + 16; k++) {
                o += snprintf(line + o, sizeof(line) - o, "%02X ", p[k]);
            }
            log_w("  %s", line);
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }

    m_dump.len   = 0;
    m_dump.ready = false;
}

bool GoProManager::writeCmd(const uint8_t *data, size_t len) {
    NimBLERemoteCharacteristic *ch =
        static_cast<NimBLERemoteCharacteristic *>(m_chCmd);
    if (!ch) return false;
    return ch->writeValue(data, len, true);
}

/* ---------------------------------------------------------------------------
 * Packet reassembly.
 *
 * GoPro splits responses larger than one ATT payload. The first packet carries
 * a header describing the total length; continuations set bit 7 and hold a
 * rolling counter in the low nibble which we can simply discard.
 *
 *   0b000x xxxx  -> 5-bit length in the low bits          e.g. 08 93 00 ...
 *   0b001x xxxx  -> 13-bit length, low 5 bits are the MSB e.g. 21 5D 13 00 ...
 *   0b010x xxxx  -> 16-bit length in the next two bytes
 *   0b1xxx xxxx  -> continuation of the packet in flight  e.g. 80 01 00 09 ...
 * ------------------------------------------------------------------------- */
void GoProManager::onNotify(Chan chan, const uint8_t *data, size_t len) {
    if (!data || len == 0) return;

    size_t  off = 0;
    uint8_t b0  = data[0];

    if (b0 & 0x80) {
        /* Continuation only belongs to the channel that opened the packet. */
        if (!m_acc.active || m_acc.chan != chan) return;
        off = 1;
    } else {
        m_acc.len    = 0;
        m_acc.active = true;
        m_acc.chan   = chan;
        switch (b0 & 0x60) {
            case 0x00:
                m_acc.expected = b0 & 0x1F;
                off = 1;
                break;
            case 0x20:
                if (len < 2) { m_acc.active = false; return; }
                m_acc.expected = ((uint16_t)(b0 & 0x1F) << 8) | data[1];
                off = 2;
                break;
            case 0x40:
                if (len < 3) { m_acc.active = false; return; }
                m_acc.expected = ((uint16_t)data[1] << 8) | data[2];
                off = 3;
                break;
            default:
                m_acc.active = false;
                return;
        }
        if (m_acc.expected > sizeof(m_acc.buf)) {
            m_acc.active = false;           /* longer than we ever need */
            return;
        }
    }

    while (off < len && m_acc.len < sizeof(m_acc.buf)) {
        m_acc.buf[m_acc.len++] = data[off++];
    }

    if (m_acc.len >= m_acc.expected) {
        parsePayload(chan, m_acc.buf, m_acc.expected);
        m_acc.active = false;
        m_acc.len    = 0;
    }
}

/* Payload is [queryId][status][TLV...] where each TLV is [id][len][value BE]. */
void GoProManager::parsePayload(Chan chan, const uint8_t *p, uint16_t len) {
    if (len < 2) return;

    uint8_t queryId = p[0];
    uint8_t result  = p[1];

    /* Command / settings acks are just [id][status]: 0 = accepted, 1 = error,
     * 2 = invalid parameter. Worth logging loudly — a rejected mode change looks
     * exactly like a successful one from the dash's point of view. */
    if (chan == CHAN_CMD || chan == CHAN_SET) {
        /* The keep-alive ack repeats every few seconds forever. Logging each one
         * floods the CDC buffer and is what was shredding everything else, so
         * settings acks are logged only when the id or status actually changes;
         * command acks are one-offs and always worth seeing. */
        if (chan == CHAN_SET) {
            if (queryId == m_lastSetId && result == m_lastSetStatus) return;
            m_lastSetId     = queryId;
            m_lastSetStatus = result;
        }
        log_w("GoPro: %s 0x%02X -> %s (status %u)",
              chan == CHAN_CMD ? "cmd" : "setting",
              queryId,
              result == 0 ? "OK" : (result == 2 ? "INVALID PARAM" : "ERROR"),
              (unsigned)result);
        return;
    }

    /* 0x12 = reply to the settings dump. Logged rather than parsed: it exists
     * so the camera can tell us its own enum codes, which are per-model and
     * cannot safely be hard-coded. Pair these with GOPRO_SETTINGS to pin a
     * profile. */
    if (queryId == 0x12) {
        if (result != 0x00) return;
        stashDump(queryId, p, len);
        return;
    }

    /* 0x13 = reply to our status dump, 0x53 = reply to the registration,
     * 0x93 = an unsolicited push because something changed. Anything else is
     * either the protobuf preset reply or something undocumented on this model:
     * dump it raw rather than dropping it, because on HERO8 the undocumented
     * cases are exactly the ones worth seeing. */
    if (queryId != 0x13 && queryId != 0x53 && queryId != 0x93) {
        stashDump(queryId, p, len);
        return;
    }
    if (result != 0x00) return;

    bool     recording  = false, busy = false, gpsLock = false;
    bool     haveRec    = false, haveBusy = false, haveGps = false;
    uint8_t  battery    = 255;
    uint32_t recSeconds = 0;
    bool     haveSecs   = false;

    uint16_t i = 2;
    while (i + 2 <= len) {
        uint8_t id  = p[i];
        uint8_t vl  = p[i + 1];
        if (i + 2 + vl > len) break;
        const uint8_t *v = p + i + 2;

        switch (id) {
            case ST_ENCODING:
                if (vl >= 1) { recording = (v[0] != 0); haveRec = true; }
                break;
            case ST_BUSY:
                if (vl >= 1) { busy = (v[0] != 0); haveBusy = true; }
                break;
            case ST_GPS_LOCK:
                if (vl >= 1) { gpsLock = (v[0] != 0); haveGps = true; }
                break;
            case ST_BATTERY_PCT:
                if (vl >= 1) battery = v[0];
                break;
            case ST_REC_SECONDS:
                if (vl >= 1 && vl <= 4) {
                    uint32_t acc = 0;
                    for (uint8_t k = 0; k < vl; k++) acc = (acc << 8) | v[k];
                    recSeconds = acc;
                    haveSecs   = true;
                }
                break;
            default:
                break;
        }
        i += 2 + vl;
    }

    /* Pushes are sparse — they only carry what changed, so merge rather than
     * overwrite, otherwise a duration tick would blank the battery reading. */
    portENTER_CRITICAL(&m_mux);
    if (haveRec)      m_st.recording  = recording;
    if (haveBusy)     m_st.busy       = busy;
    if (haveGps)      m_st.gpsLock    = gpsLock;
    if (battery != 255) m_st.batteryPct = battery;
    if (haveSecs)     m_st.recSeconds = recSeconds;
    portEXIT_CRITICAL(&m_mux);
}
