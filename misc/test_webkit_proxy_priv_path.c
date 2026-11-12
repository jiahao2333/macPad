#include <assert.h>
#include <string.h>

static int MacWSEndsWith(const char *s, const char *suffix) {
    if (!s || !suffix) return 0;
    unsigned n = 0, m = 0;
    while (s[n]) n++;
    while (suffix[m]) m++;
    if (m > n) return 0;
    s += n - m;
    while (m) {
        if (*s != *suffix) return 0;
        s++;
        suffix++;
        m--;
    }
    return 1;
}

static int MacWSPathHasSlash(const char *s) {
    if (!s) return 0;
    while (*s) {
        if (*s == '/') return 1;
        s++;
    }
    return 0;
}


int main(void) {
    assert(MacWSEndsWith("/var/jb/WebContentProxy.priv", ".priv"));
    assert(!MacWSEndsWith("/var/jb/WebContentProxy", ".priv"));
    assert(!MacWSEndsWith(".priv", ".privx"));
    assert(MacWSEndsWith(".priv", ".priv"));
    assert(MacWSPathHasSlash("/var/jb/WebContentProxy"));
    assert(!MacWSPathHasSlash("WebContentProxy"));
    return 0;
}
