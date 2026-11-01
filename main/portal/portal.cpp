/*
 * paperdiy-reader — Wi-Fi & files portal.
 *
 * HTTP API (all JSON; every POST/PUT must carry "X-Reader: 1", which a
 * cross-site page cannot send without a CORS preflight this server never
 * answers):
 *
 *   GET  /                      the web UI
 *   GET  /api/status            version, Wi-Fi state, storage
 *   GET  /api/scan              networks seen before the setup AP started
 *   POST /api/wifi              {ssid, pass}  save and switch to that network
 *   POST /api/wifi/forget       forget the saved network
 *   POST /api/finish            leave the Wi-Fi screen on the reader
 *   GET  /api/fs/list?vol=&path=
 *   GET  /api/fs/get?vol=&path=[&dl=1]
 *   PUT  /api/fs/put?vol=&path=[&overwrite=1]   raw file body
 *   POST /api/fs/mkdir          {vol, path}
 *   POST /api/fs/move           {vol, from, to}
 *   POST /api/fs/delete         {vol, path}
 *
 * vol is "sd" (the card, rooted at /sdcard) or "int" (internal /books).
 *
 * SPDX-License-Identifier: MIT
 */
#include "portal/portal.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "mdns.h"

#include "hal/hal.h"
#include "hal/storage/hal_storage.h"
#include "net/captive_dns.h"
#include "net/wifi.h"
#include "portal/fs_path.h"
#include "reader/library.h"

static const char* TAG = "portal";

namespace portal {

namespace {

constexpr uint32_t JOIN_TIMEOUT_MS = 15000;
constexpr size_t PUT_MAX_BYTES     = 200u * 1024u * 1024u;
constexpr size_t IO_CHUNK          = 16 * 1024;
constexpr size_t LIST_MAX          = 3000;
constexpr int WALK_MAX_DEPTH       = 10;
constexpr size_t WALK_MAX_ENTRIES  = 10000;

httpd_handle_t g_server = nullptr;
bool g_running          = false;
bool g_mdns             = false;
Info g_info;
SemaphoreHandle_t g_mu  = nullptr; /* guards g_info, g_scan, pending requests */
std::vector<net::ScanResult> g_scan;

volatile bool g_finish     = false;
volatile bool g_req_join   = false;
volatile bool g_req_forget = false;
volatile bool g_req_setup  = false;
volatile uint32_t g_last_hit = 0;

struct Lock {
    Lock()
    {
        xSemaphoreTake(g_mu, portMAX_DELAY);
    }
    ~Lock()
    {
        xSemaphoreGive(g_mu);
    }
};

struct StorageLock {
    StorageLock()
    {
        hal_storage_lock();
    }
    ~StorageLock()
    {
        hal_storage_unlock();
    }
};

void mark()
{
    g_last_hit = millis_();
}

/* ---- responses ------------------------------------------------------ */

const char* statusText(int code)
{
    switch (code) {
        case 200:
            return "200 OK";
        case 400:
            return "400 Bad Request";
        case 403:
            return "403 Forbidden";
        case 404:
            return "404 Not Found";
        case 409:
            return "409 Conflict";
        case 413:
            return "413 Payload Too Large";
        case 507:
            return "507 Insufficient Storage";
        default:
            return "500 Internal Server Error";
    }
}

esp_err_t sendJson(httpd_req_t* req, const std::string& body, int code = 200)
{
    httpd_resp_set_status(req, statusText(code));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body.data(), (ssize_t)body.size());
}

esp_err_t sendErr(httpd_req_t* req, int code, const std::string& msg)
{
    ESP_LOGW(TAG, "%s %s -> %d %s", req->method == HTTP_GET ? "GET" : "POST/PUT", req->uri, code, msg.c_str());
    sendJson(req, "{\"error\":\"" + fs_path::jsonEscape(msg) + "\"}", code);
    return ESP_OK;  // response sent; keep the connection
}

esp_err_t sendOk(httpd_req_t* req)
{
    return sendJson(req, "{\"ok\":true}");
}

/** Mutating requests must carry X-Reader: 1 (CSRF guard). */
bool csrfOk(httpd_req_t* req)
{
    char v[8];
    return httpd_req_get_hdr_value_str(req, "X-Reader", v, sizeof(v)) == ESP_OK && strcmp(v, "1") == 0;
}

/** httpd_req_recv that gives up after a few receive timeouts (stalled client). */
int recvSome(httpd_req_t* req, char* buf, size_t n)
{
    for (int tries = 0; tries < 3; tries++) {
        int got = httpd_req_recv(req, buf, n);
        if (got != HTTPD_SOCK_ERR_TIMEOUT) {
            return got;
        }
    }
    return -1;
}

bool readBody(httpd_req_t* req, std::string& out, size_t limit = 4096)
{
    out.clear();
    if (req->content_len == 0 || req->content_len > limit) {
        return false;
    }
    out.resize(req->content_len);
    size_t off = 0;
    while (off < out.size()) {
        int got = recvSome(req, &out[off], out.size() - off);
        if (got <= 0) {
            return false;
        }
        off += (size_t)got;
    }
    return true;
}

/** Parses a JSON body into a cJSON tree (caller deletes). */
cJSON* jsonBody(httpd_req_t* req)
{
    std::string body;
    if (!readBody(req, body)) {
        return nullptr;
    }
    return cJSON_Parse(body.c_str());
}

std::string jstr(cJSON* root, const char* key)
{
    cJSON* v = cJSON_GetObjectItem(root, key);
    return (cJSON_IsString(v) && v->valuestring) ? std::string(v->valuestring) : std::string();
}

bool queryParam(httpd_req_t* req, const char* key, std::string& out)
{
    out.clear();
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen == 0 || qlen > 2048) {
        return false;
    }
    std::string q(qlen + 1, '\0');
    if (httpd_req_get_url_query_str(req, &q[0], q.size()) != ESP_OK) {
        return false;
    }
    std::string raw(qlen + 1, '\0');
    if (httpd_query_key_value(q.c_str(), key, &raw[0], raw.size()) != ESP_OK) {
        return false;
    }
    raw.resize(strlen(raw.c_str()));
    return fs_path::urlDecode(raw, out);
}

/* ---- volumes and paths -------------------------------------------- */

const char* volRoot(const std::string& vol)
{
    if (vol == "sd") {
        return hal_storage_sd_available() ? STORAGE_SD_BASE : nullptr;
    }
    if (vol == "int") {
        return hal_storage_books_dir(STORE_INTERNAL);
    }
    return nullptr;
}

/** Resolves vol + rel into @p abs, or sends the error response. */
bool resolveOrFail(httpd_req_t* req, const std::string& vol, const std::string& rel, unsigned flags,
                   std::string& abs)
{
    const char* root = volRoot(vol);
    if (!root) {
        sendErr(req, 404, vol == "sd" ? "no SD card" : "unknown volume");
        return false;
    }
    std::string why;
    if (!fs_path::resolve(root, rel, flags, abs, &why)) {
        sendErr(req, 400, why);
        return false;
    }
    return true;
}

bool isDir(const std::string& p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool exists(const std::string& p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

/** Depth-first listing of everything under @p dir (files before their dir). */
bool walk(const std::string& dir, int depth, std::vector<std::pair<std::string, bool> >& out)
{
    if (depth > WALK_MAX_DEPTH || out.size() > WALK_MAX_ENTRIES) {
        return false;
    }
    DIR* d = opendir(dir.c_str());
    if (!d) {
        return false;
    }
    std::vector<std::string> subdirs;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            continue;
        }
        std::string p = dir + "/" + e->d_name;
        if (e->d_type == DT_DIR) {
            subdirs.push_back(p);
        } else {
            out.emplace_back(p, false);
        }
    }
    closedir(d);
    for (const std::string& s : subdirs) {
        if (!walk(s, depth + 1, out)) {
            return false;
        }
        out.emplace_back(s, true);
    }
    return true;
}

std::string pctEncode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || strchr("-._~", c)) {
            o.push_back((char)c);
        } else {
            o.push_back('%');
            o.push_back(hex[c >> 4]);
            o.push_back(hex[c & 15]);
        }
    }
    return o;
}

const char* mimeFor(const std::string& name)
{
    const char* ext = strrchr(name.c_str(), '.');
    if (!ext) {
        return "application/octet-stream";
    }
    if (!strcasecmp(ext, ".epub")) return "application/epub+zip";
    if (!strcasecmp(ext, ".txt")) return "text/plain; charset=utf-8";
    if (!strcasecmp(ext, ".png")) return "image/png";
    if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(ext, ".json")) return "application/json";
    return "application/octet-stream";
}

/* ---- handlers: UI and status -------------------------------------- */

/* web/index.html, embedded by main/CMakeLists.txt (EMBED_TXTFILES). */
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

esp_err_t hIndex(httpd_req_t* req)
{
    mark();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // EMBED_TXTFILES appends a NUL terminator; do not send it.
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

void addSpace(std::string& j, const char* key, store_id_t store)
{
    uint64_t total = 0;
    uint64_t freeb = 0;
    char buf[96];
    if (hal_storage_space(store, &total, &freeb)) {
        snprintf(buf, sizeof(buf), "\"%s\":{\"total\":%llu,\"free\":%llu}", key, (unsigned long long)total,
                 (unsigned long long)freeb);
    } else {
        snprintf(buf, sizeof(buf), "\"%s\":null", key);
    }
    j += buf;
}

esp_err_t hStatus(httpd_req_t* req)
{
    mark();
    Info in;
    {
        Lock l;
        in = g_info;
    }
    static const char* kMode[] = {"off", "joining", "lan", "setup"};
    std::string j = "{";
    j += "\"version\":\"" + fs_path::jsonEscape(esp_app_get_description()->version) + "\",";
    j += "\"mode\":\"" + std::string(kMode[(int)in.mode]) + "\",";
    j += "\"saved_ssid\":\"" + fs_path::jsonEscape(hal.settings.sta_ssid) + "\",";
    j += "\"ip\":\"" + fs_path::jsonEscape(net::staIp()) + "\",";
    j += "\"ap\":\"" + fs_path::jsonEscape(in.ap_ssid) + "\",";
    j += "\"message\":\"" + fs_path::jsonEscape(in.message) + "\",";
    char b[32];
    snprintf(b, sizeof(b), "\"battery\":%d,", hal.batteryPercent());
    j += b;
    {
        StorageLock sl;
        addSpace(j, "sd", STORE_SD);
        j += ",";
        addSpace(j, "int", STORE_INTERNAL);
    }
    j += "}";
    return sendJson(req, j);
}

esp_err_t hScan(httpd_req_t* req)
{
    mark();
    std::string j = "[";
    {
        Lock l;
        for (size_t i = 0; i < g_scan.size(); i++) {
            char b[48];
            snprintf(b, sizeof(b), ",\"rssi\":%d,\"secure\":%s}", g_scan[i].rssi, g_scan[i].secure ? "true" : "false");
            j += (i ? ",{\"ssid\":\"" : "{\"ssid\":\"") + fs_path::jsonEscape(g_scan[i].ssid) + "\"" + b;
        }
    }
    j += "]";
    return sendJson(req, j);
}

esp_err_t hWifi(httpd_req_t* req)
{
    mark();
    if (!csrfOk(req)) {
        return sendErr(req, 403, "missing X-Reader header");
    }
    cJSON* root = jsonBody(req);
    if (!root) {
        return sendErr(req, 400, "bad request");
    }
    std::string ssid = jstr(root, "ssid");
    std::string pass = jstr(root, "pass");
    cJSON_Delete(root);
    if (!net::validCredentials(ssid, pass)) {
        return sendErr(req, 400, "network name must be 1-32 bytes; password empty or 8-63 characters");
    }
    {
        Lock l;
        snprintf(hal.settings.sta_ssid, sizeof(hal.settings.sta_ssid), "%s", ssid.c_str());
        snprintf(hal.settings.sta_pass, sizeof(hal.settings.sta_pass), "%s", pass.c_str());
        g_req_join = true;  // performed by poll() on the main task
    }
    hal.settingsSave();
    return sendOk(req);
}

esp_err_t hForget(httpd_req_t* req)
{
    mark();
    if (!csrfOk(req)) {
        return sendErr(req, 403, "missing X-Reader header");
    }
    g_req_forget = true;
    return sendOk(req);
}

esp_err_t hFinish(httpd_req_t* req)
{
    mark();
    if (!csrfOk(req)) {
        return sendErr(req, 403, "missing X-Reader header");
    }
    g_finish = true;
    return sendOk(req);
}

/* ---- handlers: files ---------------------------------------------- */

esp_err_t hList(httpd_req_t* req)
{
    mark();
    std::string vol, rel, abs;
    queryParam(req, "vol", vol);
    queryParam(req, "path", rel);
    if (!resolveOrFail(req, vol, rel, fs_path::ALLOW_ROOT, abs)) {
        return ESP_OK;
    }

    struct Ent {
        std::string name;
        bool dir;
        uint32_t size;
    };
    std::vector<Ent> ents;
    bool truncated = false;
    {
        StorageLock sl;
        DIR* d = opendir(abs.c_str());
        if (!d) {
            return sendErr(req, 404, "folder not found");
        }
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            if (e->d_name[0] == '.' || !strcasecmp(e->d_name, "System Volume Information")) {
                continue;  // hidden: caches, temp uploads, OS clutter
            }
            if (ents.size() >= LIST_MAX) {
                truncated = true;
                break;
            }
            Ent en{e->d_name, e->d_type == DT_DIR, 0};
            if (!en.dir) {
                struct stat st;
                if (stat((abs + "/" + en.name).c_str(), &st) == 0) {
                    en.size = (uint32_t)st.st_size;
                }
            }
            ents.push_back(std::move(en));
        }
        closedir(d);
    }
    std::sort(ents.begin(), ents.end(), [](const Ent& a, const Ent& b) {
        if (a.dir != b.dir) {
            return a.dir;
        }
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    std::string j;
    j.reserve(64 + ents.size() * 48);
    j += "{\"truncated\":";
    j += truncated ? "true" : "false";
    j += ",\"entries\":[";
    for (size_t i = 0; i < ents.size(); i++) {
        char b[48];
        snprintf(b, sizeof(b), "\",\"dir\":%s,\"size\":%u}", ents[i].dir ? "true" : "false", (unsigned)ents[i].size);
        j += i ? ",{\"name\":\"" : "{\"name\":\"";
        j += fs_path::jsonEscape(ents[i].name);
        j += b;
    }
    j += "]}";
    return sendJson(req, j);
}

esp_err_t hGet(httpd_req_t* req)
{
    mark();
    std::string vol, rel, abs, dl;
    queryParam(req, "vol", vol);
    queryParam(req, "path", rel);
    queryParam(req, "dl", dl);
    if (!resolveOrFail(req, vol, rel, 0, abs)) {
        return ESP_OK;
    }
    FILE* f = fopen(abs.c_str(), "rb");
    if (!f || isDir(abs)) {
        if (f) {
            fclose(f);
        }
        return sendErr(req, 404, "file not found");
    }
    std::string name = fs_path::baseName(abs);
    httpd_resp_set_type(req, mimeFor(name));
    std::string disp = (dl == "1" ? "attachment" : "inline") + std::string("; filename*=UTF-8''") + pctEncode(name);
    httpd_resp_set_hdr(req, "Content-Disposition", disp.c_str());
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    std::vector<char> buf(IO_CHUNK);
    esp_err_t err = ESP_OK;
    for (;;) {
        size_t n = fread(buf.data(), 1, buf.size(), f);
        if (n == 0) {
            break;
        }
        err = httpd_resp_send_chunk(req, buf.data(), (ssize_t)n);
        if (err != ESP_OK) {
            break;
        }
        mark();
    }
    fclose(f);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, nullptr, 0);
    }
    return err;
}

esp_err_t hPut(httpd_req_t* req)
{
    mark();
    if (!csrfOk(req)) {
        return sendErr(req, 403, "missing X-Reader header");
    }
    std::string vol, rel, abs, ow;
    queryParam(req, "vol", vol);
    queryParam(req, "path", rel);
    queryParam(req, "overwrite", ow);
    if (!resolveOrFail(req, vol, rel, 0, abs)) {
        return ESP_FAIL;  // body not read: drop the connection
    }
    if (req->content_len == 0) {
        sendErr(req, 400, "empty file");
        return ESP_FAIL;
    }
    if (req->content_len > PUT_MAX_BYTES) {
        sendErr(req, 413, "file too large (200 MB max)");
        return ESP_FAIL;
    }
    const std::string dir = fs_path::dirName(abs);
    if (!isDir(dir)) {
        sendErr(req, 404, "folder not found");
        return ESP_FAIL;
    }
    if (exists(abs) && ow != "1") {
        sendErr(req, 409, "a file with that name already exists");
        return ESP_FAIL;
    }
    uint64_t total = 0;
    uint64_t freeb = 0;
    store_id_t store = (vol == "sd") ? STORE_SD : STORE_INTERNAL;
    if (hal_storage_space(store, &total, &freeb) && freeb < (uint64_t)req->content_len + 65536) {
        sendErr(req, 507, "not enough free space");
        return ESP_FAIL;
    }

    // Stream into a hidden temp file next to the target, then swap it in:
    // a failed or interrupted upload never damages an existing file.
    char tmpname[32];
    snprintf(tmpname, sizeof(tmpname), "/.up-%08lx.tmp", (unsigned long)esp_random());
    const std::string tmp = dir + tmpname;
    FILE* out = fopen(tmp.c_str(), "wb");
    if (!out) {
        sendErr(req, 500, "cannot create file");
        return ESP_FAIL;
    }
    std::vector<char> buf(IO_CHUNK);
    size_t remaining = req->content_len;
    bool ok          = true;
    int code         = 500;
    const char* why  = "write failed";
    while (remaining > 0) {
        int got = recvSome(req, buf.data(), std::min(remaining, buf.size()));
        if (got <= 0) {
            ok   = false;
            code = 400;
            why  = "upload interrupted";
            break;
        }
        if (fwrite(buf.data(), 1, (size_t)got, out) != (size_t)got) {
            ok   = false;
            code = (errno == ENOSPC) ? 507 : 500;
            why  = (errno == ENOSPC) ? "card full" : "write failed";
            break;
        }
        remaining -= (size_t)got;
        mark();
    }
    if (ok && (fflush(out) != 0 || fsync(fileno(out)) != 0)) {
        ok = false;
    }
    if (fclose(out) != 0) {
        ok = false;
    }
    if (!ok) {
        remove(tmp.c_str());
        sendErr(req, code, why);
        return remaining > 0 ? ESP_FAIL : ESP_OK;
    }

    {
        StorageLock sl;
        if (exists(abs)) {
            if (isDir(abs)) {
                remove(tmp.c_str());
                return sendErr(req, 409, "a folder with that name exists");
            }
            library::bookForgotten(abs);  // new content: old caches are wrong
            remove(abs.c_str());
        }
        if (rename(tmp.c_str(), abs.c_str()) != 0) {
            remove(tmp.c_str());
            return sendErr(req, 500, "could not save file");
        }
    }
    ESP_LOGI(TAG, "saved %s (%u bytes)", abs.c_str(), (unsigned)req->content_len);
    return sendOk(req);
}

esp_err_t hMkdir(httpd_req_t* req)
{
    mark();
    if (!csrfOk(req)) {
        return sendErr(req, 403, "missing X-Reader header");
    }
    cJSON* root = jsonBody(req);
    if (!root) {
        return sendErr(req, 400, "bad request");
    }
    std::string vol = jstr(root, "vol"), rel = jstr(root, "path"), abs;
    cJSON_Delete(root);
    if (!resolveOrFail(req, vol, rel, 0, abs)) {
        return ESP_OK;
    }
    StorageLock sl;
    if (exists(abs)) {
        return sendErr(req, 409, "already exists");
    }
    if (mkdir(abs.c_str(), 0775) != 0) {
        return sendErr(req, 500, "could not create folder");
    }
    return sendOk(req);
}

esp_err_t hMove(httpd_req_t* req)
{
    mark();
    if (!csrfOk(req)) {
        return sendErr(req, 403, "missing X-Reader header");
    }
    cJSON* root = jsonBody(req);
    if (!root) {
        return sendErr(req, 400, "bad request");
    }
    std::string vol = jstr(root, "vol"), from = jstr(root, "from"), to = jstr(root, "to");
    cJSON_Delete(root);
    std::string a, b;
    if (!resolveOrFail(req, vol, from, 0, a) || !resolveOrFail(req, vol, to, 0, b)) {
        return ESP_OK;
    }
    if (a == b) {
        return sendOk(req);
    }
    if (b.compare(0, a.size() + 1, a + "/") == 0) {
        return sendErr(req, 400, "cannot move a folder into itself");
    }
    StorageLock sl;
    if (!exists(a)) {
        return sendErr(req, 404, "not found");
    }
    if (exists(b)) {
        return sendErr(req, 409, "the destination already exists");
    }
    if (!isDir(fs_path::dirName(b))) {
        return sendErr(req, 404, "destination folder not found");
    }
    // Books keep their reading progress: collect them before the rename.
    std::vector<std::pair<std::string, bool> > files;
    const bool dir = isDir(a);
    if (dir && !walk(a, 0, files)) {
        return sendErr(req, 400, "folder too large to move");
    }
    if (rename(a.c_str(), b.c_str()) != 0) {
        return sendErr(req, 500, "could not move");
    }
    if (dir) {
        for (const auto& f : files) {
            if (!f.second) {
                library::bookMoved(f.first, b + f.first.substr(a.size()));
            }
        }
    } else {
        library::bookMoved(a, b);
    }
    return sendOk(req);
}

esp_err_t hDelete(httpd_req_t* req)
{
    mark();
    if (!csrfOk(req)) {
        return sendErr(req, 403, "missing X-Reader header");
    }
    cJSON* root = jsonBody(req);
    if (!root) {
        return sendErr(req, 400, "bad request");
    }
    std::string vol = jstr(root, "vol"), rel = jstr(root, "path"), abs;
    cJSON_Delete(root);
    if (!resolveOrFail(req, vol, rel, 0, abs)) {  // never the volume root
        return ESP_OK;
    }
    StorageLock sl;
    if (!exists(abs)) {
        return sendErr(req, 404, "not found");
    }
    if (!isDir(abs)) {
        library::bookForgotten(abs);
        return remove(abs.c_str()) == 0 ? sendOk(req) : sendErr(req, 500, "could not delete");
    }
    std::vector<std::pair<std::string, bool> > all;
    if (!walk(abs, 0, all)) {
        return sendErr(req, 400, "folder too large to delete here");
    }
    for (const auto& f : all) {
        if (f.second) {
            rmdir(f.first.c_str());
        } else {
            library::bookForgotten(f.first);
            remove(f.first.c_str());
        }
    }
    return rmdir(abs.c_str()) == 0 ? sendOk(req) : sendErr(req, 500, "folder not fully deleted");
}

/** Setup mode: send unknown URLs (OS connectivity checks) to the UI. */
esp_err_t h404(httpd_req_t* req, httpd_err_code_t)
{
    mark();
    if (g_info.mode != Mode::Setup) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, "Redirecting to the reader", HTTPD_RESP_USE_STRLEN);  // iOS wants a body
    return ESP_OK;
}

const httpd_uri_t kRoutes[] = {
    {"/", HTTP_GET, hIndex, nullptr},
    {"/api/status", HTTP_GET, hStatus, nullptr},
    {"/api/scan", HTTP_GET, hScan, nullptr},
    {"/api/wifi", HTTP_POST, hWifi, nullptr},
    {"/api/wifi/forget", HTTP_POST, hForget, nullptr},
    {"/api/finish", HTTP_POST, hFinish, nullptr},
    {"/api/fs/list", HTTP_GET, hList, nullptr},
    {"/api/fs/get", HTTP_GET, hGet, nullptr},
    {"/api/fs/put", HTTP_PUT, hPut, nullptr},
    {"/api/fs/mkdir", HTTP_POST, hMkdir, nullptr},
    {"/api/fs/move", HTTP_POST, hMove, nullptr},
    {"/api/fs/delete", HTTP_POST, hDelete, nullptr},
};

/* ---- lifecycle ------------------------------------------------------ */

bool startServer()
{
    if (g_server) {
        return true;
    }
    httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers  = 16;
    cfg.stack_size        = 10240;
    cfg.lru_purge_enable  = true;
    cfg.max_open_sockets  = 7;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    cfg.keep_alive_enable = true;
    if (httpd_start(&g_server, &cfg) != ESP_OK) {
        g_server = nullptr;
        return false;
    }
    for (const httpd_uri_t& r : kRoutes) {
        httpd_register_uri_handler(g_server, &r);
    }
    httpd_register_err_handler(g_server, HTTPD_404_NOT_FOUND, h404);
    return true;
}

void stopServer()
{
    if (g_server) {
        httpd_stop(g_server);
        g_server = nullptr;
    }
}

void startMdns()
{
    if (g_mdns || mdns_init() != ESP_OK) {
        return;
    }
    mdns_hostname_set("reader");
    mdns_instance_name_set("PaperDIY reader");
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    g_mdns = true;
}

void stopMdns()
{
    if (g_mdns) {
        mdns_free();
        g_mdns = false;
    }
}

void setInfo(Mode m, const std::string& url, const std::string& msg)
{
    Lock l;
    g_info.mode    = m;
    g_info.url     = url;
    g_info.message = msg;
    g_info.ssid    = hal.settings.sta_ssid;
}

void teardown()
{
    net::dnsStop();
    stopServer();
    stopMdns();
    net::stop();
}

/** Setup access point with a captive page. */
bool bringUpSetup(const std::string& msg)
{
    teardown();
    // Scan first: the list is shown on the setup page (scanning while the AP
    // is up would hop channels under the connected phone).
    std::vector<net::ScanResult> found = net::scan();
    {
        Lock l;
        g_scan.swap(found);
    }
    char id[8];
    hal.getShortId(id, sizeof(id));
    std::string ssid = std::string("PaperDIY-") + id;
    std::string ip;
    if (!net::startAp(ssid.c_str(), &ip)) {
        setInfo(Mode::Off, "", "could not start the access point");
        return false;
    }
    // Advertise the captive page via DHCP option 114 as well.
    if (esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF")) {
        static char uri[32];
        snprintf(uri, sizeof(uri), "http://%s", ip.c_str());
        esp_netif_dhcps_stop(nif);
        esp_netif_dhcps_option(nif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, uri, strlen(uri));
        esp_netif_dhcps_start(nif);
    }
    net::dnsStart(inet_addr(ip.c_str()));
    if (!startServer()) {
        teardown();
        setInfo(Mode::Off, "", "could not start the web server");
        return false;
    }
    {
        Lock l;
        g_info.ap_ssid = ssid;
    }
    setInfo(Mode::Setup, "http://" + ip, msg);
    return true;
}

/** Joins the saved network; falls back to setup on failure. */
bool bringUpLan()
{
    teardown();
    setInfo(Mode::Joining, "", "");
    std::string ip;
    net::JoinError err = net::JoinError::None;
    if (!net::joinSta(hal.settings.sta_ssid, hal.settings.sta_pass, JOIN_TIMEOUT_MS, &ip, &err)) {
        return bringUpSetup(std::string("Couldn't join \"") + hal.settings.sta_ssid + "\": " +
                            net::joinErrorText(err));
    }
    startMdns();
    if (!startServer()) {
        teardown();
        setInfo(Mode::Off, "", "could not start the web server");
        return false;
    }
    setInfo(Mode::Lan, "http://" + ip, "");
    return true;
}

}  // namespace

/* ================================================================== */

esp_err_t start()
{
    if (!g_mu) {
        g_mu = xSemaphoreCreateMutex();
    }
    if (g_running) {
        return ESP_OK;
    }
    g_finish     = false;
    g_req_join   = false;
    g_req_forget = false;
    g_req_setup  = false;
    mark();
    if (!net::init()) {
        return ESP_FAIL;
    }
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    bool ok  = hal.settings.sta_ssid[0] ? bringUpLan() : bringUpSetup("");
    g_running = ok;
    return ok ? ESP_OK : ESP_FAIL;
}

void stop()
{
    if (!g_running && !g_server) {
        net::stop();
        return;
    }
    teardown();
    setInfo(Mode::Off, "", "");
    g_running = false;
    ESP_LOGI(TAG, "portal down");
}

void forceSetup()
{
    g_req_setup = true;
}

bool poll()
{
    if (!g_running) {
        return false;
    }
    if (g_req_forget) {
        g_req_forget = false;
        {
            Lock l;
            hal.settings.sta_ssid[0] = 0;
            hal.settings.sta_pass[0] = 0;
        }
        hal.settingsSave();
        bringUpSetup("Saved network forgotten.");
        return true;
    }
    if (g_req_join) {
        // Two steps so the reader can draw "Connecting..." before the join
        // blocks the main loop for up to 15 s.
        static bool shown = false;
        if (!shown) {
            shown = true;
            setInfo(Mode::Joining, "", "");
            return true;
        }
        shown      = false;
        g_req_join = false;
        vTaskDelay(pdMS_TO_TICKS(500));  // let the HTTP reply reach the browser
        bringUpLan();
        return true;
    }
    if (g_req_setup) {
        g_req_setup = false;
        bringUpSetup("");
        return true;
    }
    return false;
}

const Info& info()
{
    return g_info;
}

bool isRunning()
{
    return g_running;
}

bool finishRequested()
{
    return g_finish;
}

uint32_t idleMs()
{
    return millis_() - g_last_hit;
}

}  // namespace portal
