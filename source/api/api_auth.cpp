// Authentication, config save/load.

#include <stdio.h>
#include <string.h>

#include <io/pad.h>
#include <sysutil/sysutil.h>

#include "jellyfin_api.h"
#include "ui.h"

void save_config(void) {
    FILE *f = fopen("/dev_hdd0/tmp/jellyfin_config.txt", "w");
    if (f) {
        fprintf(f, "%s\n%s\n%s\n%s\n", g_server, g_username, g_token, g_userid);
        fclose(f);
    }
}

int load_config(void) {
    FILE *f = fopen("/dev_hdd0/tmp/jellyfin_config.txt", "r");
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
    if (g_server[0] && g_token[0]) {
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
    remove("/dev_hdd0/tmp/jellyfin_config.txt");
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
    drawText(40, 100, "Logging in...");
    drawTextf(40, 130, "Server: %s", g_server);
    drawTextf(40, 155, "User:   %s", g_username);
    flip();

    char url[512], body[256];
    snprintf(url,  sizeof(url),  "%s/Users/AuthenticateByName", g_server);
    snprintf(body, sizeof(body), "{\"Username\":\"%s\",\"Pw\":\"%s\"}",
             g_username, password);

    {
        FILE *dbg = fopen("/dev_hdd0/tmp/jf_debug.txt", "w");
        if (dbg) {
            fprintf(dbg, "URL: %s\nBody(%d): %s\n", url, (int)strlen(body), body);
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
    drawText(40, 100, "Login Failed!");
    drawTextf(40, 130, "Status: %d", status);
    if      (status == 401) drawText(40, 155, "Wrong username or password");
    else if (status == 404) drawText(40, 155, "Wrong server URL or path");
    else if (status == 400) drawText(40, 155, "Bad request (check credentials)");
    else if (status ==  -1) drawText(40, 155, "Could not reach server");
    if (responseBuffer[0]) {
        char snippet[64]; snprintf(snippet, sizeof(snippet), "%.60s", responseBuffer);
        drawTextf(40, 180, "%s", snippet);
    }
    drawText(40, 230, "Press X to try again");
    flip();

    init_btns();
    while (running) {
        sysUtilCheckCallback();
        poll_buttons();
        if (BTN_PRESSED(cross)) return 0;
    }
    return 0;
}
