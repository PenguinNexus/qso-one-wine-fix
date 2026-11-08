/* No-op replacement for flutter_local_notifications_windows.dll.
   Wine lacks WinRT ToastNotificationManager; the real DLL throws an
   uncaught winrt::hresult_error (REGDB_E_CLASSNOTREG) inside init(). */
#include <windows.h>
#include <stdbool.h>
#define EXPORT __declspec(dllexport)

/* Lets qso-one-wine-fix.sh recognise an already-installed stub. */
__attribute__((used)) static const char marker[] = "QSO-WINE-FIX notifications stub";

static int dummy_plugin;

EXPORT void *createPlugin(void) { return &dummy_plugin; }
EXPORT void disposePlugin(void *p) { (void)p; }
EXPORT bool init(void *p, char *a, char *b, char *c, char *d, void *cb) { return true; }
EXPORT bool showNotification(void *p, int id, char *xml, void *bindings) { return true; }
EXPORT bool scheduleNotification(void *p, int id, char *xml, long long t) { return true; }
EXPORT int updateNotification(void *p, int id, void *bindings) { return 0; }
EXPORT void cancelAll(void *p) {}
EXPORT void cancelNotification(void *p, int id) {}
EXPORT void *getActiveNotifications(void *p, int *size) { if (size) *size = 0; return NULL; }
EXPORT void *getPendingNotifications(void *p, int *size) { if (size) *size = 0; return NULL; }
EXPORT void freeDetailsArray(void *p) {}
EXPORT void freeLaunchDetails(void *d) {}
EXPORT bool hasPackageIdentity(void) { return false; }
EXPORT bool isValidXml(char *xml) { return true; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID x) { return TRUE; }
