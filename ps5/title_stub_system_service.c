/* Link-time stub naming an import from the console's libSceSystemService.
 *
 * The payload SDK's stub for that library does not list
 * sceSystemServiceHideSplashScreen or sceSystemServiceLoadExec, so the title
 * converter refuses a title that imports them. ps5/title_build.sh builds this into a stub shared object,
 * exactly as the driver project does for its libSceAgc imports; the body is
 * never run, the real function comes from the system module at load.
 */

int sceSystemServiceHideSplashScreen(void) { return -1; }
int sceSystemServiceLoadExec(const char* path, char* const argv[]) { return -1; }
