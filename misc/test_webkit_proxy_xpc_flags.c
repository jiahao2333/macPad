#include <assert.h>
#include <string.h>

static int MacWSFlagsAreZero(const char *value) {
    if (!value || !*value) return 1;
    if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
        value += 2;
    if (!*value) return 1;
    while (*value) {
        if (*value != '0') return 0;
        value++;
    }
    return 1;
}

static int MacWSHasEnvironmentKey(const char *entry, const char *key) {
    if (!entry || !key) return 0;
    while (*key && *entry == *key) { entry++; key++; }
    return !*key && *entry == '=';
}

static const char *MacWSEnvironmentValue(char *envp[], const char *key) {
    for (char **entry = envp; entry && *entry; entry++) {
        if (!MacWSHasEnvironmentKey(*entry, key)) continue;
        const char *value = *entry;
        while (*value && *value != '=') value++;
        return *value == '=' ? value + 1 : (const char *)0;
    }
    return (const char *)0;
}

static char *outenv[16];

static void build(char **envp, int isWebKit) {
    const char *incoming = MacWSEnvironmentValue(envp, "XPC_FLAGS");
    const char *peerEnv = MacWSEnvironmentValue(envp, "MACWS_WK_PEER");
    const int isUniquePeer = peerEnv &&
        strstr(peerEnv, "com.macwsguide.wk.i.") != 0;
    static char webkitFlags[] = "XPC_FLAGS=8019";
    static char uniqueFlags[] = "XPC_FLAGS=0";
    int replaceZeroFlags = isWebKit && !isUniquePeer &&
        MacWSFlagsAreZero(incoming);
    int normalizeUniqueFlags = isWebKit && isUniquePeer;
    int n = 0;
    for (char **entry = envp; entry && *entry; entry++) {
        if ((replaceZeroFlags || normalizeUniqueFlags) &&
            MacWSHasEnvironmentKey(*entry, "XPC_FLAGS"))
            continue;
        outenv[n++] = *entry;
    }
    if (replaceZeroFlags) outenv[n++] = webkitFlags;
    else if (normalizeUniqueFlags) outenv[n++] = uniqueFlags;
    outenv[n] = 0;
}

int main(void) {
    assert(MacWSFlagsAreZero(0));
    assert(MacWSFlagsAreZero(""));
    assert(MacWSFlagsAreZero("0"));
    assert(MacWSFlagsAreZero("0x0"));
    assert(MacWSFlagsAreZero("0X00"));
    assert(!MacWSFlagsAreZero("8019"));
    assert(!MacWSFlagsAreZero("1"));
    assert(!MacWSFlagsAreZero("0x1"));
    char *zero[] = {
        "XPC_FLAGS=0x0",
        "XPC_SERVICE_NAME=com.apple.macosbooter.WebKit.WebContent",
        "KEEP=1",
        0
    };
    build(zero, 1);
    assert(!strcmp(outenv[0], "XPC_SERVICE_NAME=com.apple.macosbooter.WebKit.WebContent"));
    assert(!strcmp(outenv[1], "KEEP=1"));
    assert(!strcmp(outenv[2], "XPC_FLAGS=8019"));
    assert(outenv[3] == 0);
    char *kept[] = {"XPC_FLAGS=8019", "KEEP=1", 0};
    build(kept, 1);
    assert(!strcmp(outenv[0], "XPC_FLAGS=8019"));
    assert(!strcmp(outenv[1], "KEEP=1"));
    assert(outenv[2] == 0);
    char *nonwebkit[] = {"XPC_FLAGS=0x0", "KEEP=1", 0};
    build(nonwebkit, 0);
    assert(!strcmp(outenv[0], "XPC_FLAGS=0x0"));
    assert(!strcmp(outenv[1], "KEEP=1"));
    assert(outenv[2] == 0);
    char *unique_zero[] = {
        "XPC_FLAGS=0x0",
        "MACWS_WK_PEER=com.macwsguide.wk.i.0123456789abcdef0123456789abcdef",
        "KEEP=1",
        0
    };
    build(unique_zero, 1);
    assert(!strcmp(outenv[0],
        "MACWS_WK_PEER=com.macwsguide.wk.i.0123456789abcdef0123456789abcdef"));
    assert(!strcmp(outenv[1], "KEEP=1"));
    assert(!strcmp(outenv[2], "XPC_FLAGS=0"));
    assert(outenv[3] == 0);
    char *unique_managed[] = {
        "XPC_FLAGS=8019",
        "MACWS_WK_PEER=com.macwsguide.wk.i.0123456789abcdef0123456789abcdef",
        "KEEP=1",
        0
    };
    build(unique_managed, 1);
    assert(!strcmp(outenv[0],
        "MACWS_WK_PEER=com.macwsguide.wk.i.0123456789abcdef0123456789abcdef"));
    assert(!strcmp(outenv[1], "KEEP=1"));
    assert(!strcmp(outenv[2], "XPC_FLAGS=0"));
    assert(outenv[3] == 0);
    return 0;
}
