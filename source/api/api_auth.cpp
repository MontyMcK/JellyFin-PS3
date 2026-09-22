// Authentication, config save/load.

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <io/pad.h>
#include <sysutil/sysutil.h>

#include "jellyfin_api.h"
#include "ui.h"
#include "ui_visuals.h"
#include "timing.h"
#include "jf_paths.h"

volatile bool g_auth_expired = false;

// Per-install DeviceId, generated once and persisted.  Jellyfin keys device
// sessions on this string: with the old fixed "ps3" id, a login from ANY
// machine (an RPCS3 test run, a second console) took over the slot and
// revoked this install's token — which then failed as a silent empty
// library.  Survives logout on purpose so one install keeps one identity.
static char s_device_id[24] = "";

const char *jf_device_id(void) {
    if (s_device_id[0]) return s_device_id;
    FILE *f = fopen(jf_data_path("jellyfin_device_id.txt"), "r");
    if (f) {
        if (fscanf(f, "%23s", s_device_id) != 1) s_device_id[0] = '\0';
        fclose(f);
    }
    if (!s_device_id[0]) {
        u32 a = (u32)time(NULL);
        u32 b = (u32)timing_get_us();
        snprintf(s_device_id, sizeof(s_device_id), "ps3-%08x%08x",
                 a ^ 0x9e3779b9u, b);
        f = fopen(jf_data_path("jellyfin_device_id.txt"), "w");
        if (f) { fprintf(f, "%s\n", s_device_id); fclose(f); }
    }
    return s_device_id;
}

void save_config(void) {
    FILE *f = fopen(jf_data_path("jellyfin_config.txt"), "w");
    if (f) {
        fprintf(f, "%s\n%s\n%s\n%s\n", g_server, g_username, g_token, g_userid);
        fclose(f);
    }
}

int load_config(void) {
    FILE *f = fopen(jf_data_path("jellyfin_config.txt"), "r");
    if (!f) return 0;
    fscanf(f, "%255s\n%63s\n%255s\n%63s\n", g_server, g_username, g_token, g_userid);
    fclose(f);
    int sl = strlen(g_server);
    if (sl > 0 && g_server[sl-1] == '/') g_server[sl-1] = '\0';
    return (g_server[0] && g_token[0]);
}

void jellyfin_logout(void) {
    // Best-effort: tell the server to invalidate this access token.  Ignore the
    // result — we clear local state regardless so the user is always logged out.
    //
    // Skipped when the token is ALREADY known dead (g_auth_expired): the server
    // has revoked it, so the call can only fail, and logging out is exactly the
    // moment the user is least able to wait for a network round trip that
    // cannot succeed.
    if (g_server[0] && g_token[0] && !g_auth_expired) {
        char url[512];
        snprintf(url, sizeof(url), "%s/Sessions/Logout", g_server);
        http_request(1, url, "{}", g_token, responseBuffer, RESPONSE_SIZE);
    }

    // Clear in-memory credentials.  Keep g_server so the user only re-enters
    // their username and password, not the server URL.
    g_token[0]    = '\0';
    g_userid[0]   = '\0';
    g_username[0] = '\0';

    // Remove the saved config so a restart does not auto-login.
    remove(jf_data_path("jellyfin_config.txt"));

    // The dead-token flag belongs to the session that just ended; leaving it
    // set would send the NEXT login straight back to the login screen.
    g_auth_expired = false;
}

// The saved session is dead server-side (a 401 came back on a request that
// carried our token — http.cpp sets g_auth_expired).  Jellyfin revokes a
// device's token whenever the same DeviceId authenticates again, so this
// happens for ordinary reasons: signing in from another client that reuses
// the id, a server restore, an admin revoking sessions.
//
// Until now nothing consumed that flag, and the symptom was silent and
// baffling: every API call 401s, so the library comes back EMPTY while the
// same account on a PC shows everything, and no amount of navigating helps
// because the app never re-authenticates.  Treat it as what it is — a logged
// out session — and send the user back to the login screen.
void jellyfin_session_expired(void) {
    g_auth_expired = false;
    g_token[0]  = '\0';
    g_userid[0] = '\0';
    remove(jf_data_path("jellyfin_config.txt"));

    drawHeader();
    drawTTF(40, 96,  "Session expired", 20, XMB_TEXT);
    drawTTF(40, 128,
            "The server rejected this device's saved login, so the library "
            "could not be read.", 14, XMB_TEXT_DIM);
    drawTTF(40, 152,
            "This usually means another client signed in and took over the "
            "session.", 14, XMB_TEXT_DIM);
    drawTTF(40, 188, "X: sign in again", 16, XMB_ACCENT);
    flip();

    init_btns();
    while (running) {
        sysUtilCheckCallback();
        poll_buttons();
        if (BTN_PRESSED(cross) || BTN_PRESSED(circle)) return;
    }
}

static void trim(char *s) {
    int len = strlen(s);
    while (len > 0 && (s[len-1]==' '||s[len-1]=='\n'||s[len-1]=='\r'||s[len-1]=='\t'))
        s[--len] = '\0';
    char *p = s;
    while (*p==' '||*p=='\n'||*p=='\r'||*p=='\t') p++;
    if (p != s) memmove(s, p, strlen(p)+1);
}

int do_login(void) {
    char password[64] = "";
    if (get_input(g_username, sizeof(g_username), "Username", false) != 1) return 0;
    if (get_input(password,   sizeof(password),   "Password", true)  != 1) return 0;
    trim(g_username);
    trim(password);

    drawHeader();
    drawTTF(40, 96, "Signing in...", 18, XMB_TEXT);
    {
        char line[320];
        snprintf(line, sizeof(line), "%s  \xC2\xB7  %s", g_username, g_server);
        drawTTF(40, 128, line, 14, XMB_TEXT_DIM);
    }
    flip();

    char url[512], body[256];
    snprintf(url,  sizeof(url),  "%s/Users/AuthenticateByName", g_server);
    snprintf(body, sizeof(body), "{\"Username\":\"%s\",\"Pw\":\"%s\"}",
             g_username, password);

    {
        // Diagnostic trace of the sign-in request.  /dev_hdd0/tmp is readable
        // by anyone on the LAN through webMAN's anonymous FTP, so the request
        // body must NEVER be written verbatim: it carried the password in
        // plaintext.  Log the URL, the username and the body length only.
        FILE *dbg = fopen("/dev_hdd0/tmp/jf_debug.txt", "w");
        if (dbg) {
            fprintf(dbg, "URL: %s\nBody(%d): {\"Username\":\"%s\",\"Pw\":\"<redacted>\"}\n",
                    url, (int)strlen(body), g_username);
            fclose(dbg);
        }
    }

    int status = http_request(1, url, body, NULL, responseBuffer, RESPONSE_SIZE);

    if (status == 200) {
        json_get_string(responseBuffer, "AccessToken", g_token,  sizeof(g_token));
        json_get_string(responseBuffer, "Id",          g_userid, sizeof(g_userid));
        if (g_token[0]) { save_config(); return 1; }
    }

    {
        FILE *dbg = fopen("/dev_hdd0/tmp/jf_debug.txt", "a");
        if (dbg) {
            fprintf(dbg, "Status: %d\nResponse(%d): %s\n",
                    status, (int)strlen(responseBuffer), responseBuffer);
            fclose(dbg);
        }
    }

    drawHeader();
    drawTTF(40, 96, "Couldn't sign in", 22, XMB_TEXT, true);
    {
        const char *why;
        if      (status == 401) why = "Wrong username or password.";
        else if (status == 404) why = "Wrong server URL or path.";
        else if (status == 400) why = "Bad request - check your credentials.";
        else if (status ==  -1) why = "Could not reach the server.";
        else                    why = "The server returned an unexpected error.";
        char line[96];
        snprintf(line, sizeof(line), "%s  (status %d)", why, status);
        drawTTF(40, 136, line, 16, XMB_TEXT_DIM);
    }
    if (responseBuffer[0]) {
        char snippet[64]; snprintf(snippet, sizeof(snippet), "%.60s", responseBuffer);
        drawTTF(40, 164, snippet, 13, XMB_TEXT_FAINT);
    }
    {
        static const Hint h[] = {{'X',"Try again"}};
        draw_hints_bar(h, 1);
    }
    flip();

    init_btns();
    while (running) {
        sysUtilCheckCallback();
        poll_buttons();
        if (BTN_PRESSED(cross)) return 0;
    }
    return 0;
}
