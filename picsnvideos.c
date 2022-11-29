/*******************************************************************************
 * picsnvideos.c
 *
 * Copyright (C) 2008 by Dan Bodoh
 * Contributor (2022): Ulf Zibis <Ulf.Zibis@CoSoCo.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 ******************************************************************************/

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <utime.h>

#include <pi-dlp.h>
#include <pi-source.h>
#include <pi-util.h>

#include "libplugin.h"
//#include "i18n.h"

#define MYNAME PACKAGE_NAME
#define PCDIR "Media"
#define PREFS_VERSION 3
#define ADDITIONAL_FILES "/#AdditionalFiles"

#define L_DEBUG JP_LOG_DEBUG
#define L_INFO  JP_LOG_WARN // JP_LOG_INFO unfortunately doesn't show up in GUI, so use JP_LOG_WARN.
#define L_WARN  JP_LOG_WARN
#define L_FATAL JP_LOG_FATAL
#define L_GUI   JP_LOG_GUI

typedef struct VFSInfo VFSInfo;
typedef struct VFSDirInfo VFSDirInfo;
typedef struct fullPath {int volRef; char *name; struct fullPath *next;} fullPath;

static const char HELP_TEXT[] =
"JPilot plugin (c) 2008 by Dan Bodoh\n\
Contributor (2022): Ulf Zibis <Ulf.Zibis@CoSoCo.de>\n\
Version: "VERSION"\n\
\n\
Synchronizes media as pictures, videos and audios from\n\
the Pics&Videos storage and SDCard in the Palm with\n\
folder '"PCDIR"' in your JPilot data directory,\n\
usually \"$JPILOT_HOME/.jpilot\".\n\
\n\
For more documentation, bug reports and new versions,\n\
see https://github.com/danbodoh/picsnvideos-jpilot";

static const char *PREFS_FILE = "picsnvideos.rc";
static prefType prefs[] = {
    {"prefsVersion", INTTYPE, INTTYPE, PREFS_VERSION, NULL, 0},
    {"rootDirs", CHARTYPE, CHARTYPE, 0, "1>/Photos & Videos:1>/Fotos & Videos:/DCIM", 0},
    {"syncThumbnailDir", INTTYPE, INTTYPE, 0, NULL, 0},
    // JPEG picture
    // video (GSM phones)
    // video (CDMA phones)
    // audio caption (GSM phones)
    // audio caption (CDMA phones)
    {"fileTypes", CHARTYPE, CHARTYPE, 0, "jpg:amr:qcp:3gp:3g2:avi", 0},
    {"useDateModified", INTTYPE, INTTYPE, 0, NULL, 0},
    {"compareContent", INTTYPE, INTTYPE, 0, NULL, 0},
    {"doBackup", INTTYPE, INTTYPE, 1, NULL, 0},
    {"doRestore", INTTYPE, INTTYPE, 1, NULL, 0},
    {"listFiles", INTTYPE, INTTYPE, 0, NULL, 0},
    {"excludeDirs", CHARTYPE, CHARTYPE, 0, "/BLAZER:2>/PALM/Launcher", 0},
    {"deleteFiles", CHARTYPE, CHARTYPE, 0, NULL, 0},
    {"additionalFiles", CHARTYPE, CHARTYPE, 0, NULL, 0}
};
static const unsigned NUM_PREFS = sizeof(prefs)/sizeof(prefType);
static long prefsVersion;
static char *rootDirs; // becomes freed by jp_free_prefs()
static long syncThumbnailDir;
static char *fileTypes; // becomes freed by jp_free_prefs()
static long useDateModified;
static long compareContent;
static long doBackup;
static long doRestore;
static long listFiles;
static char *excludeDirs; // becomes freed by jp_free_prefs()
static char *deleteFiles; // becomes freed by jp_free_prefs()
static char *additionalFiles; // becomes freed by jp_free_prefs()

static const unsigned MAX_VOLUMES = 16;
static const unsigned MIN_DIR_ITEMS = 2;
static const unsigned MAX_DIR_ITEMS = 1024;
static const char *LOCALDIRS[] = {"/Internal", "/SDCard", "/Card"};
static fullPath *rootDirList = NULL;
static fullPath *fileTypeList = NULL;
static fullPath *excludeDirList = NULL;
static fullPath *deleteFileList = NULL;
static fullPath *additionalFileList = NULL;
static pi_buffer_t *piBuf, *piBuf2;
static int sd; // the central socket descriptor.
static char mediaHome[NAME_MAX];
static char syncLogEntry[128];
static int importantWarning = 0;


static void *mallocLog(size_t size);
static PI_ERR piErrLog(const PI_ERR piErr, const int level, const int volRef, const char *rmPath,
        const char *indent, const char message[], const char comment[]);
int parsePaths(char *paths, fullPath **list, char *text);
void freePathList(fullPath *list);
int volumeEnumerateIncludeHidden(const int sd, int *numVols, int *volRefs);
int syncVolume(int volRef);
char *isoTime(const time_t time);
PI_ERR listRemoteFiles(int volRef, const char *dir, const int indent);
time_t getLocalDate(const char *path);
void setLocalDate(const char *path, const time_t date);
time_t getRemoteDate(const FileRef fileRef, const int volRef, const char *path, const char prefix[]);
void setRemoteDate(FileRef fileRef, const int volRef, const char *path, const time_t date);
static char *localRoot(const unsigned volRef);
int createLocalDir(char *path, const char *dir, const int volRef, const char *rmPath);
PI_ERR createRemoteDir(const int volRef, char *path, const char *dir, const char *lcPath);
int backupFileIfNeeded(const unsigned volRef, const char *rmDir, const char *lcDir, const char *file);
int restoreFile(const char *lcDir, const unsigned volRef, const char *rmDir, const char *file);

// ToDo: replace strcat() by stpcpy()

void plugin_version(int *major_version, int *minor_version) {
    *major_version = 0;
    *minor_version = 99;
}

int plugin_get_name(char *name, int len) {
    //~ snprintf(name, len, "%s %d.%d", MYNAME, PLUGIN_MAJOR, PLUGIN_MINOR);
    strncpy(name, PACKAGE_STRING, len);
    return EXIT_SUCCESS;
}

int plugin_get_help_name(char *name, int len) {
    //~ g_snprintf(name, len, _("About %s"), _(MYNAME)); // With language support.
    snprintf(name, len, "About %s", MYNAME);
    return EXIT_SUCCESS;
}

int plugin_help(char **text, int *width, int *height) {
    // Unfortunately JPilot app tries to free the *text memory,
    // so we must copy the text to new allocated heap memory first.
    if ((*text = mallocLog(strlen(HELP_TEXT) + 1))) {
        strcpy(*text, HELP_TEXT);
    }
    // *text = HELP_TEXT;  // alternative, causes crash !!!
    *height = 0;
    *width = 0;
    return EXIT_SUCCESS;
}

int plugin_startup(jp_startup_info *info) {
    jp_init();
    return EXIT_SUCCESS;
}

int plugin_sync(int socket) {
    sd = socket;

    // Read and process preferences.
    jp_pref_init(prefs, NUM_PREFS);
    if (jp_pref_read_rc_file(PREFS_FILE, prefs, NUM_PREFS) < 0)
        jp_logf(L_WARN, "%s: WARNING: Could not read prefs[] from '%s'\n", MYNAME, PREFS_FILE);
    if (jp_pref_write_rc_file(PREFS_FILE, prefs, NUM_PREFS) < 0) // If pref file wasn't existent ...
        jp_logf(L_WARN, "%s: WARNING: Could not write prefs[] to '%s'\n", MYNAME, PREFS_FILE); // initialize with defaults.
    jp_get_pref(prefs, 0, &prefsVersion, NULL);
    if (prefsVersion != PREFS_VERSION) {
        jp_logf(L_FATAL, "%s: ERROR: Version of preferences file '%s' must be %d, please update it!\n", MYNAME, PREFS_FILE, PREFS_VERSION);
        return EXIT_FAILURE;
    }
    jp_get_pref(prefs, 1, NULL, (const char **)&rootDirs);
    jp_get_pref(prefs, 2, &syncThumbnailDir, NULL);
    jp_get_pref(prefs, 3, NULL, (const char **)&fileTypes);
    jp_get_pref(prefs, 4, &useDateModified, NULL);
    jp_get_pref(prefs, 5, &compareContent, NULL);
    jp_get_pref(prefs, 6, &doBackup, NULL);
    jp_get_pref(prefs, 7, &doRestore, NULL);
    jp_get_pref(prefs, 8, &listFiles, NULL);
    jp_get_pref(prefs, 9, NULL, (const char **)&excludeDirs);
    jp_get_pref(prefs, 10, NULL, (const char **)&deleteFiles);
    jp_get_pref(prefs, 11, NULL, (const char **)&additionalFiles);
    if (    parsePaths(rootDirs, &rootDirList, "rootDirs") != EXIT_SUCCESS ||
            parsePaths(fileTypes, &fileTypeList, "fileTypes") != EXIT_SUCCESS ||
            parsePaths(excludeDirs, &excludeDirList, "excludeDirs") != EXIT_SUCCESS ||
            parsePaths(deleteFiles, &deleteFileList, "deleteFiles") != EXIT_SUCCESS ||
            parsePaths(additionalFiles, &additionalFileList, "additionalFiles") != EXIT_SUCCESS ||
            !(piBuf = pi_buffer_new(32768)) || !(piBuf2 = pi_buffer_new(32768))) {
        jp_logf(L_FATAL, "%s: ERROR: Out of memory\n", MYNAME);
        return EXIT_FAILURE;
    }

    // Use $JPILOT_HOME/.jpilot/ or current directory for PCDIR.
    if (jp_get_home_file_name(PCDIR, mediaHome, NAME_MAX) < 0) {
        jp_logf(L_WARN, "%s: WARNING: Could not get $JPILOT_HOME path, so using current directory.\n", MYNAME);
        strcpy(mediaHome, "./"PCDIR);
    }
    if (listFiles)
        jp_logf(L_INFO, "%s: List all files from the Palm device to the terminal, needs: 'jpilot -d'\n", MYNAME);
    else {
        jp_logf(L_INFO, "%s: Start syncing with '%s ...'\n", MYNAME, mediaHome);
        // Check if there are any file types loaded.
        if (!fileTypeList) {
            jp_logf(L_FATAL, "%s: ERROR: Could not find any file types from '%s'; No media synced.\n", MYNAME, PREFS_FILE);
            return EXIT_FAILURE;
        }
    }

    // Get list of the volumes on the pilot.
    int volRefs[MAX_VOLUMES];
    int volumes = MAX_VOLUMES;
    if (volumeEnumerateIncludeHidden(sd, &volumes, volRefs) < 0) {
        jp_logf(L_FATAL, "%s: ERROR: Could not find any VFS volumes; No files to sync or list.\n", MYNAME);
        return EXIT_FAILURE;
    }

    // Scan all the volumes for media and backup them.
    int result = EXIT_FAILURE;
    PI_ERR piErr;
    for (int i=0; i<volumes; i++) {
        if (listFiles) { // List all files from the Palm device, but don't sync.
            if (listRemoteFiles(volRefs[i], "/", 1) < 0)  goto Continue;
        } else if ((piErr = syncVolume(volRefs[i])) < -2) {
            snprintf(syncLogEntry, sizeof(syncLogEntry),
                    "%s:  WARNING: Could not find any media on volume %d; No media synced.\n", MYNAME, volRefs[i]);
            jp_logf(L_WARN, syncLogEntry);
            dlp_AddSyncLogEntry (sd, syncLogEntry);
            goto Continue;
        } else if (piErr < 0) {
            snprintf(syncLogEntry, sizeof(syncLogEntry),
                    "%s:  WARNING: Errors occured on volume %d; Some media may not be synced.\n", MYNAME, volRefs[i]);
            jp_logf(L_WARN, syncLogEntry);
            dlp_AddSyncLogEntry (sd, syncLogEntry);
        }
        result = EXIT_SUCCESS;
Continue:
    }

    // Process deleteFileList ...
    if (deleteFileList)
        jp_logf(L_INFO, "%s: Delete files from pref 'deleteFiles' ...\n", MYNAME);
    for (fullPath *item = deleteFileList; item; item = item->next) {
        if (item->name[0] != '/')
            jp_logf(L_WARN, "%s:     WARNING: Missing '/' at start of file '%s' on volume %d, not deleting it.\n", MYNAME, item->name, item->volRef);
        else if ((piErrLog(dlp_VFSFileDelete(sd, item->volRef, item->name),
                L_FATAL, item->volRef, item->name, "    ", ": Not deleted remote file","")) >= 0)
            jp_logf(L_INFO, "%s:     Deleted remote file '%s' on volume %d\n", MYNAME, item->name, item->volRef);
    }

    // Process additionalFileList ...
    if (additionalFileList)
        jp_logf(L_INFO, "%s: Sync files from pref 'additionalFiles' with '%s/VOLUME%s ...'\n", MYNAME, mediaHome, ADDITIONAL_FILES);
    for (fullPath *item = additionalFileList; item; item = item->next) {
        jp_logf(L_DEBUG, "%s:  Sync additional file: item->volRef=%d, item->name='%s'\n", MYNAME, item->volRef, item->name);
        if (item->name[0] != '/') {
            jp_logf(L_WARN, "%s:     WARNING: Missing '/' at start of additional file '%s' on volume %d, not syncing it.\n", MYNAME, item->name, item->volRef);
            continue;
        }
        char *lcDir = localRoot(item->volRef);
        if (!lcDir || createLocalDir(lcDir, ADDITIONAL_FILES, -1, ""))  continue;
        //~ jp_logf(L_DEBUG, "%s:     lcDir='%s', getLocalDate(lcDir)='%s'\n", MYNAME, lcDir, isoTime(getLocalDate(lcDir)));
        char *fname = strrchr(item->name, '/');
        FileRef fileRef = 0;
        if ((piErr = dlp_VFSFileOpen(sd, item->volRef, item->name, vfsModeRead, &fileRef)) >= 0 && doBackup) { // Backup file ...
            time_t parentDate = 0;
            unsigned long attr = 0;
            dlp_VFSFileGetAttributes(sd, fileRef, &attr);
            dlp_VFSFileClose(sd, fileRef);
            if (attr & vfsFileAttrDirectory)
                createLocalDir(lcDir, item->name, item->volRef, "");
            else {
                *fname++ = '\0'; // truncate dir part from item->name
                //~ jp_logf(L_DEBUG, "%s:     new item->name='%s', fname='%s'\n", MYNAME, item->name, fname);
                if (!*(item->name) || !createLocalDir(lcDir, item->name, item->volRef, "")) {
                    parentDate = getLocalDate(lcDir);
                    backupFileIfNeeded(item->volRef, item->name, lcDir, fname);
                    //~ jp_logf(L_DEBUG, "%s:     lcDir='%s', parentDate='%s'\n", MYNAME, lcDir, isoTime(parentDate));
                    if (parentDate)  setLocalDate(lcDir, parentDate); // recover parent dir date. // ToDo: maybe do by BackupFileIfNeeded()
                }
            }
        } else if (!fileRef && doRestore) { // Restore file ...
            char rmDir[NAME_MAX] = "", lcRoot[NAME_MAX];
            strcpy(lcRoot, lcDir);
            struct stat fstat;
            int statErr;
            if ((statErr = stat(strcat(lcDir, item->name), &fstat))) {
                piErrLog(piErr, L_FATAL, item->volRef, item->name, "    ", ": Could not find remote file","");
                jp_logf(L_FATAL, "%s:     ERROR %d: Could not read status of '%s'; No sync possible!\n", MYNAME, statErr, lcDir);
                result = EXIT_FAILURE;
            } else if (S_ISDIR(fstat.st_mode))
                createRemoteDir(item->volRef, rmDir, item->name, lcRoot);
            else {
                *fname++ = *(strrchr(lcDir, '/')) = '\0'; // truncate from fname again.
                if (!*(item->name) || createRemoteDir(item->volRef, rmDir, item->name, lcRoot) >= 0)
                    restoreFile(lcDir, item->volRef, rmDir, fname);
            }
        } else if (doRestore) {
            jp_logf(L_WARN, "%s:     WARNING: Remote file '%s' on volume %d already exists. To replace, first delete it.\n", MYNAME, item->name, item->volRef);
        }
        if (fileRef)  dlp_VFSFileClose(sd, fileRef);
    }

    if (!listFiles || additionalFileList)
        jp_logf(L_DEBUG, "%s: Sync done -> result=%d\n", MYNAME, result);
    if (result != EXIT_SUCCESS)
        dlp_AddSyncLogEntry (sd, "Synchronization of Media was incomplete.\n");
    if (importantWarning) {
        // Avoids bug <https://github.com/desrod/pilot-link/issues/11>, as then the file "Album.db" is created, so the dir is not empty anymore.
        jp_logf(L_WARN, "\n%s: IMPORTANT WARNING: Now open once the Media app on your Palm device to avoid crash (signal SIGCHLD) on next HotSync !!!\n\n", MYNAME);
        dlp_AddSyncLogEntry (sd, MYNAME": IMPORTANT WARNING: Now open once the Media app to avoid crash with JPilot on next HotSync !!!\n");
    }
    return result;
}

int plugin_post_sync(void) {
    pi_buffer_free(piBuf);
    pi_buffer_free(piBuf2);
    freePathList(rootDirList);
    freePathList(fileTypeList);
    freePathList(excludeDirList);
    freePathList(deleteFileList);
    freePathList(additionalFileList);
    jp_free_prefs(prefs, NUM_PREFS); // Calling this in plugin_exit_cleanup() causes crash from free().
    jp_logf(L_DEBUG, "%s: plugin_post_sync -> done.\n", MYNAME);
    return EXIT_SUCCESS;
}

// ToDo: Rename picsnvideos ./. media
// ToDo: Reorder functions

/* Log OOM error on malloc(). */
static void *mallocLog(size_t size) {
    void *p;
    if (!(p = malloc(size)))
        jp_logf(L_FATAL, "%s: ERROR: Out of memory\n", MYNAME);
    return p;
}

static char *errString(const int isPiErr, const int err, const int level, const char message[]) { //, const int ignore1, const int ignore2) {
    static char string[128];
    PI_ERR piOSErr = (isPiErr && err == PI_ERR_DLP_PALMOS) ? pi_palmos_error(sd) : 0;
    //~ PI_ERR piOSErr = piErr == PI_ERR_DLP_PALMOS ? pi_palmos_error(sd) : 0;
    switch (piOSErr) {
        case 10760 : message = ": Not found the file"; break;
        case 10761 : message = ": The volume № is invalid;"; break;
        case 10765 : message = ": Can't delete non-empty directory"; break;
        case 10767 : message = ": No space left on volume"; break;
    }
    snprintf(string, sizeof(string), "%s%s %d%s", piOSErr ? "PalmOS " : "", level == L_FATAL ? "ERROR" : "WARNING", piOSErr ? piOSErr : err, message);
    return string;
}

static PI_ERR piErrLog(const PI_ERR piErr, const int level, const int volRef, const char *rmPath,
        const char *indent, const char message[], const char comment[]) {
    if (piErr < 0) {
        jp_logf(level, "%s: %s%s '%s' on volume %d%s\n", MYNAME, indent, errString(1, piErr, level, message), rmPath, volRef, comment);
    }
    return piErr;
}

int parsePaths(char *paths, fullPath **list, char *text) {
    for (char *last; *paths != '\0'; *--last = '\0') {
        if (!*(last = (last = strrchr(paths, ':')) ? last + 1 : paths)) {
            jp_logf(L_WARN, "%s: WARNING: Empty name in %s.\n", MYNAME, text);
            continue;
        }
        fullPath *item;
        if ((item = mallocLog(sizeof(*item)))) {
            char *separator = strchr(last, '>');
            if (separator) {
                *separator = '\0';
                item->volRef = atoi(last);
                item->name = separator + 1;
            } else {
                item->volRef = -1;
                item->name = last;
            }
            item->next = *list;
            (*list) = item;
        } else {
            freePathList(*list);
            (*list) = NULL;
            return EXIT_FAILURE;
        }
        jp_logf(L_DEBUG, "%s: Got %s item: '%s' for Volume %d\n", MYNAME, text, (*list)->name, (*list)->volRef);
        if (last == paths)
            break;
    }
    return EXIT_SUCCESS;
}

void freePathList(fullPath *list) {
    for (fullPath *item; (item = list);) {
        list = list->next;
        free(item);
    }
}

time_t getLocalDate(const char *path) {
    struct stat fstat;
    int statErr = stat(path, &fstat);
    if (statErr) {
        jp_logf(L_WARN, "%s:       WARNING: Could not get date of file '%s', statErr=%d\n", MYNAME, path, statErr);
        return 0;
    }
    return fstat.st_mtime;
}

void setLocalDate(const char *path, const time_t date) {
    struct stat fstat;
    int statErr = stat(path, &fstat);
    if ((statErr = stat(path, &fstat)))
        jp_logf(L_WARN, "%s:       WARNING: Could not set date of file '%s', statErr=%d\n", MYNAME, path, statErr);
    else {
        struct utimbuf utim;
        utim.actime = (time_t)fstat.st_atime;
        utim.modtime = date;
        statErr = utime(path, &utim);
        jp_logf(L_DEBUG, "%s:       setLocalDate(path='%s', date='%s') ---> done!\n", MYNAME, path, isoTime(date));
    }
}

time_t getRemoteDate(FileRef fileRef, const int volRef, const char *path, const char prefix[]) {
    time_t date = 0;
    int close = !fileRef && dlp_VFSFileOpen(sd, volRef, path, vfsModeRead, &fileRef) >= 0;
    // 'date modified' seems to be ignored by PalmOS
    PI_ERR piErr = dlp_VFSFileGetDate(sd, fileRef, useDateModified ? vfsFileDateModified : vfsFileDateCreated, &date);
    if (piErr < 0) {
        if (prefix) // for listRemoteFiles()
            jp_logf(L_DEBUG, "%s WARNING: No 'date %s from   %s\n", prefix, useDateModified ? "modified'":"created' ", path);
        else  piErrLog(piErr, L_WARN, volRef, path, "      ", useDateModified ?
                ": Could not get 'date modified' of file":": Could not get 'date created' of file","");
    }
    if (close)  dlp_VFSFileClose(sd, fileRef);
    return date;
}

void setRemoteDate(FileRef fileRef, const int volRef, const char *path, const time_t date) {
    int close = !fileRef && dlp_VFSFileOpen(sd, volRef, path, vfsModeReadWrite, &fileRef) >= 0;
    // Set both dates of the file (DateCreated is displayed in Media App on Palm device); must not be before 1980, otherwise PalmOS error.
    piErrLog(dlp_VFSFileSetDate(sd, fileRef, vfsFileDateCreated, date), L_WARN, volRef, path, "      ", ": Could not set 'date created' of file","");
    piErrLog(dlp_VFSFileSetDate(sd, fileRef, vfsFileDateModified, date), L_WARN, volRef, path, "      ", ": Could not set 'date modified' of file","");
    if (close)  dlp_VFSFileClose(sd, fileRef);
}

/*
 * *path becomes extended by *dir as successfully created.
 * If *dir is non-NULL, it should start with "/" and *path should be already existent and start with "/" or "./".
 * If *dir is NULL, only the last element of *path may be non-existent and *path should start with "/" or "./".
 * If *rmPath should be in sync with *path.
 * EXIT_SUCCESS is returned on success, otherwise EXIT_FAILURE.
 * Caller should allocate and free *path value.
 */
int createLocalDir(char *path, const char *dir, const int volRef, const char *rmPath) {
    jp_logf(L_DEBUG, "%s:     createLocalDir(path='%s', dir='%s', volRef=%d, rmPath='%s')\n", MYNAME, path, dir, volRef, rmPath);
    char *pathBase = path + strlen(path), *subDir = NULL, parent[NAME_MAX], rmDir[NAME_MAX];
    strcpy(parent, path);

    if (dir) {
        stpcpy(pathBase, dir);
        if ((subDir = strchr(dir + 1, '/')))
            *(strchr(pathBase + 1, '/')) = '\0';
    } else if ((dir = strrchr(path, '/'))) {
        parent[dir - path] = '\0';
    } else {
        strcpy(parent, "."); // ToDo: in this case do not reset parent's date
    }
    stpcpy(stpcpy(rmDir, rmPath), pathBase);
    time_t parentDate = strcmp(parent, ".") && strcmp(strrchr(parent, '/'), ADDITIONAL_FILES) ? getLocalDate(parent) : 0; // skip in case
    jp_logf(L_DEBUG, "%s:     path='%s', subDir='%s', parent='%s', parentDate='%s', rmDir='%s'\n", MYNAME, path, subDir, parent, isoTime(parentDate), rmDir);
    int result = mkdir(path, 0777);
    if (!result) {
        jp_logf(L_INFO, "%s:     Created directory '%s'\n", MYNAME, path);
        if (parentDate)  setLocalDate(parent, parentDate); // Recover date of parent path, because mkdir() changed it. // ToDo: maybe do always, at least for lcRoot
    } else if (errno != EEXIST) {
        jp_logf(L_FATAL, "%s:     ERROR %d: Could not create directory %s\n", MYNAME, errno, path);
        *(strrchr(path, '/')) = '\0'; // truncate *path
        return result;
    }
    time_t date = volRef >= 0 && rmDir[0] ? getRemoteDate(0, volRef, rmDir, NULL) : 0;
    //~ jp_logf(L_DEBUG, "%s:     path='%s', date='%s', volRef=%d, rmDir='%s'\n", MYNAME, path, isoTime(date), volRef, rmDir);
    if (date)  setLocalDate(path, date); // do always (repair local Media/Internal from /Photos & Videos if initial single sync on #AdditionalFiles)
    if (subDir) {
        return createLocalDir(path, subDir, volRef, rmDir);
    }
    return EXIT_SUCCESS;
}

/*
 * *path becomes extended by *dir as successfully created.
 * If *dir is non-NULL, it should start with "/" and *path should be already existent and start with "/".
 * If *dir is NULL, only the last element of *path may be non-existent and *path should start with "/".
 * If *lcPath should be in sync with *path.
 * 0 is returned on success, otherwise negative PI_ERR.
 * Caller should allocate and free *path value.
 */
PI_ERR createRemoteDir(const int volRef, char *path, const char *dir, const char *lcPath) {
    jp_logf(L_DEBUG, "%s:     createRemoteDir(volRef=%d, path='%s', dir='%s', lcPath='%s')\n", MYNAME, volRef, path, dir, lcPath);
    char *pathBase = path + strlen(path), *subDir = NULL, lcDir[NAME_MAX];
    if (dir) {
        stpcpy(pathBase, dir);
        if ((subDir = strchr(dir + 1, '/')))
            *(strchr(pathBase + 1, '/')) = '\0';
    }
    stpcpy(stpcpy(lcDir, lcPath), pathBase);
    PI_ERR piErr = dlp_VFSDirCreate(sd, volRef, path);
    int piOSErr = piErr == PI_ERR_DLP_PALMOS ? pi_palmos_error(sd) : 0;
    if (piErr >= 0) {
        jp_logf(L_INFO, "%s:     Created directory '%s' on volume %d\n", MYNAME, path, volRef);
        importantWarning = 1;
        time_t date = getLocalDate(lcDir);
        if (date)  setRemoteDate(0, volRef, path, date); // set remote dir date, if really created
    } else if (piOSErr != 10758) { // File not already existing.
        jp_logf(L_FATAL, "%s:     %s: Could not create dir '%s' on volume %d\n", MYNAME, errString(1, piErr, L_FATAL, ""), path, volRef);
        *(strrchr(path, '/')) = '\0'; // truncate *path
        return piErr;
    }
    if (subDir) {
        return createRemoteDir(volRef, path, subDir, lcDir);
    }
    return 0;
}

/*
 * Return directory name on the PC, where the albums should be stored. Returned string is of the form
 * "$JPILOT_HOME/.jpilot/$PCDIR/VolumeRoot/". Directories in the path are created as needed.
 * Null is returned if out of memory.
 * Caller should free return value.
 */
static char *localRoot(const unsigned volRef) {
    static char path[NAME_MAX];
    VFSInfo volInfo;
    PI_ERR piErr;

    if (createLocalDir(strcpy(path, mediaHome), NULL, -1, ""))  return NULL;
    // Get indicator of which card.
    if ((piErr = dlp_VFSVolumeInfo(sd, volRef, &volInfo)) < 0) {
        jp_logf(L_FATAL, "%s:     %s Could not get info from volume %d\n", MYNAME, errString(1, piErr, L_FATAL, ""), volRef);
        return NULL;
    }
    if (volInfo.mediaType == pi_mktag('T', 'F', 'F', 'S')) {
        if (createLocalDir(path, LOCALDIRS[0], -1, ""))  return NULL;
    } else if (volInfo.mediaType == pi_mktag('s', 'd', 'i', 'g')) {
        if (createLocalDir(path, LOCALDIRS[1], -1, ""))  return NULL;
    } else {
        sprintf(path+strlen(path), "%s%d", LOCALDIRS[2], volInfo.slotRefNum);
        if (createLocalDir(path, NULL, -1, ""))  return NULL;
    }
    return path; // must not be free'd by caller as it's a static array
}

int enumerateOpenDir(const int volRef, const FileRef dirRef, const char *rmDir, VFSDirInfo dirInfos[]) {
    int dirItems_init = MIN_DIR_ITEMS, dirItems;
    PI_ERR piErr;

    // Iterate over all the files in the remote dir.
    //~ enum dlpVFSFileIteratorConstants itr = vfsIteratorStart; // doesn't work because of type mismatch bug <https://github.com/juddmon/jpilot/issues/39>
    //~ while (itr != (unsigned long)vfsIteratorStop) { // doesn't work because of bug <https://github.com/juddmon/jpilot/issues/39>
    //~ while (itr != (unsigned)vfsIteratorStop) { // doesn't work because of bug <https://github.com/juddmon/jpilot/issues/41>
    unsigned long itr = (unsigned long)vfsIteratorStart;
    int loops = 16; // for debugging
    for (; (dirItems = dirItems_init) <= MAX_DIR_ITEMS && dirItems > 0; dirItems_init *= 2) { // WORKAROUND
        if (--loops < 0)  break; // for debugging
        //~ jp_logf(L_DEBUG, "%s:      Enumerate remote dir '%s': dirRef=%8lx, itr=%4lx, dirItems=%d\n", MYNAME, rmDir, dirRef, itr, dirItems);
        if ((piErr = piErrLog(dlp_VFSDirEntryEnumerate(sd, dirRef, &itr, &dirItems, dirInfos),
                L_FATAL, volRef, rmDir, "     ", ": Could not enumerate dir","")) < 0) {
            // Crashes on empty directory (see: <https://github.com/desrod/pilot-link/issues/11>):
            // Further research is neccessary (see: <https://github.com/juddmon/jpilot/issues/41>):
            // - Why in case of i.e. setting dirItems=4, itr != 0, even if there are more than 4 files?
            // - Why then on SDCard itr == 1888 in the first loop, so out of allowed range?
            //~ jp_logf(L_DEBUG, "%s:      Enumerate ERROR %4d: dirRef=%8lx, itr=%4lx, dirItems=%d\n", MYNAME, piErr, dirRef, itr, dirItems);
            return piErr;
        }
        //~ jp_logf(L_DEBUG, "%s:      Enumerate OK: piErr=%4d, dirRef=%8lx, itr=%4lx, dirItems=%d\n", MYNAME, piErr, dirRef, itr, dirItems);
        //~ for (int i = dirItems_init==MIN_DIR_ITEMS ? 0 : dirItems_init/2; i < dirItems; i++) {
            //~ jp_logf(L_DEBUG, "%s:       dirItem %3d: '%s' attributes %x\n", MYNAME, i, dirInfos[i].name, dirInfos[i].attr);
        //~ }
        if (dirItems < dirItems_init)
            break;
        itr = (unsigned long)vfsIteratorStart; // workaround, reset itr for next loop, if it wrongly was -1 or 1888
    }
    if (dirItems >= MAX_DIR_ITEMS)
        jp_logf(L_FATAL, "%s:      Enumerate OVERFLOW: There seem to be more than %d dir items in '%s'!\n", MYNAME, MAX_DIR_ITEMS, rmDir);
    return dirItems;
}

int enumerateDir(const int volRef, const char *rmDir, VFSDirInfo dirInfos[]) { // ToDo: combine with upper function
    FileRef dirRef;
    PI_ERR piErr = dlp_VFSFileOpen(sd, volRef, rmDir, vfsModeRead, &dirRef);
    if (piErrLog(piErr, L_FATAL, volRef, rmDir, "      ", ": Could not open dir","") < 0)  return piErr;
    int dirItems = enumerateOpenDir(volRef, dirRef, rmDir, dirInfos);
    dlp_VFSFileClose(sd, dirRef);
    return dirItems;
}

int cmpExcludeDirList(const int volRef, const char *dname) {
    int result = 1;
    for (fullPath *item = excludeDirList; dname && item; item = item->next) {
        if ((item->volRef < 0 || volRef == item->volRef) && !(result = strcmp(dname, item->name)))  break;
    }
    return result;
}

char *isoTime(const time_t time) {
    static char isoTime[20];
    strftime(isoTime, 20, "%F %T", localtime (&time));
    return isoTime;
}

/* List remote files and directories recursivly up to given depth. */
PI_ERR listRemoteFiles(const int volRef, const char *rmDir, const int depth) {
    FileRef fileRef;
    VFSDirInfo dirInfos[MAX_DIR_ITEMS];
    char prefix[] = MYNAME":                 ";

    prefix[MIN(sizeof(prefix) - 1, strlen(MYNAME) + 1 + depth)] = '\0';
    if (!cmpExcludeDirList(volRef, rmDir))  return -1; // avoid bug <https://github.com/desrod/pilot-link/issues/11>
    int dirItems = enumerateDir(volRef, rmDir, dirInfos);
    jp_logf(L_DEBUG, "%s%d remote files in '%s' on Volume %d ...\n", prefix, dirItems, rmDir, volRef);
    for (int i = 0; i < dirItems; i++) {
        char child[NAME_MAX];
        int filesize = 0;
        time_t date = 0;

        strncat(strncat(strcpy(child, strcmp(rmDir, "/") ? rmDir : ""), "/", NAME_MAX-1), dirInfos[i].name, NAME_MAX-1);
        //~ strncat(strcmp(rmDir, "/") ? strncat(strcat(child, rmDir), "/", NAME_MAX-1) : child, dirInfos[i].name, NAME_MAX-1); // for paths without '/' at start
        if (dlp_VFSFileOpen(sd, volRef, child, vfsModeRead, &fileRef) < 0) {
            jp_logf(L_DEBUG, "%s WARNING: Cannot get size/date from %s\n", prefix, dirInfos[i].name);
        } else {
            if (!(dirInfos[i].attr & vfsFileAttrDirectory) && (dlp_VFSFileSize(sd, fileRef, &filesize) < 0))
                jp_logf(L_DEBUG, "%s WARNING: Could not get size   of   %s\n", prefix, dirInfos[i].name);
            date = getRemoteDate(fileRef, 0, dirInfos[i].name, prefix);
            dlp_VFSFileClose(sd, fileRef);
        }
        //~ jp_logf(L_DEBUG, "%s 0x%02x%10d %s %s %s\n", prefix, dirInfos[i].attr, filesize, isoTime(&dateCre), isoTime(&dateMod), dirInfos[i].name);
        jp_logf(L_DEBUG, "%s 0x%02x%10d %s %s\n", prefix, dirInfos[i].attr, filesize, isoTime(date), dirInfos[i].name);
        if (dirInfos[i].attr & vfsFileAttrDirectory && depth < listFiles) {
            listRemoteFiles(volRef, child, depth + 1);
        }
    }
    return (PI_ERR)dirItems;
}

int fileRead(FileRef fileRef, FILE *fileP, pi_buffer_t *buf, int remaining) {
    buf->used = 0;
    for (int readsize = 0, todo = remaining > buf->allocated ? buf->allocated : remaining; todo > 0; todo -= readsize) {
        if (fileRef) {
            readsize = dlp_VFSFileRead(sd, fileRef, buf, todo);
            //readsize = dlp_VFSFileRead(sd, fileRef, buf, buf->allocated); // works too, but is very slow
        } else if (fileP) {
            readsize = fread(buf->data + buf->used, 1, todo, fileP);
            buf->used += (size_t)readsize;
        }
        if (readsize < 0) {
            jp_logf(L_FATAL, "\n%s:       %s on file read, aborting at %d bytes left.\n",
                    MYNAME, errString(fileRef, readsize, L_FATAL, ""), remaining - buf->used);
            return readsize;
        }
    }
    return (int)buf->used;
}

int fileWrite(FileRef fileRef, FILE *fileP, pi_buffer_t *buf, int remaining) {
    for (int writesize = 0, offset = 0; offset < buf->used; offset += writesize) {
        if (fileRef) {
            writesize = dlp_VFSFileWrite(sd, fileRef, buf->data + offset, buf->used - offset);
        } else if (fileP) {
            writesize = fwrite(buf->data + offset, 1, buf->used - offset, fileP);
        }
        if (writesize < 0) {
            jp_logf(L_FATAL, "\n%s:       %s on file write, aborting at %d bytes left.\n",
                    MYNAME, errString(fileRef, writesize, L_FATAL, ""), remaining - offset);
            return writesize;
        }
    }
    return (int)buf->used;
}

int fileCompare(FileRef fileRef, FILE *fileP, int filesize) {
    int result;
    for (int todo = filesize; todo > 0; todo -= piBuf->used) {
        if (fileRead(fileRef, NULL, piBuf, todo) < 0 || fileRead(0, fileP, piBuf2, todo) < 0 || piBuf->used != piBuf2->used) {
            jp_logf(L_FATAL, "%s:       ERROR reading files for comparison, so assuming different ...\n", MYNAME);
            jp_logf(L_DEBUG, "%s:       filesize=%d, todo=%d, piBuf->used=%d, piBuf2->used=%d\n", MYNAME, filesize, todo, piBuf->used, piBuf2->used);
            result = -1; // remember error
            break;
        }
        if ((result = memcmp(piBuf->data, piBuf2->data, piBuf->used))) {
            break; // Files have different content.
        }
    }
    return result;
}

/*
 * Backup a file from the Palm device, if not existent or different.
 */
int backupFileIfNeeded(const unsigned volRef, const char *rmDir, const char *lcDir, const char *file) {
    jp_logf(L_DEBUG, "%s:      backupFileIfNeeded(volRef=%d, rmDir='%s', lcDir='%s', file='%s')\n", MYNAME, volRef, rmDir, lcDir, file);
    char rmPath[strlen(rmDir) + strlen(file) + 2];
    char lcPath[strlen(lcDir) + strlen(file) + 4]; // prepare for possible rename
    FileRef fileRef;
    FILE *fileP;
    int filesize; // also serves as error return code

    strcat(strcat(strcpy(rmPath, rmDir), "/"), file);
    strcat(strcat(strcpy(lcPath, lcDir), "/"), file);

    if (piErrLog(dlp_VFSFileOpen(sd, volRef, rmPath, vfsModeRead, &fileRef),
            L_FATAL, volRef, rmPath, "      ", ": Could not open remote file","") < 0)
        return -1;
    else if (piErrLog(dlp_VFSFileSize(sd, fileRef, &filesize),
            L_WARN, volRef, rmPath, "      ", ": Could not get size of", ", so anyway backup it.") < 0)
        filesize = 0;

    struct stat fstat;
    int statErr = stat(lcPath, &fstat);
    if (!statErr) {
        int equal = 0;
        if (fstat.st_size != filesize) {
            jp_logf(L_WARN, "%s:       WARNING: File '%s' already exists, but has different size %d vs. %d,\n", MYNAME, lcPath, fstat.st_size, filesize);
        } else if (!compareContent) {
            equal = 1;
        } else {
            FILE *fileP;
            if (!(fileP = fopen(lcPath, "r"))) {
                jp_logf(L_WARN, "%s:       WARNING: Cannot open %s for comparing %d bytes, so may have different content,\n", MYNAME, lcPath, filesize);
            } else {
                if (!(equal = !fileCompare(fileRef, fileP, filesize)))
                    jp_logf(L_WARN, "%s:       WARNING: File '%s' already exists, but has different content,\n", MYNAME, lcPath);
                fclose(fileP);
                if (piErrLog(dlp_VFSFileSeek(sd, fileRef, vfsOriginBeginning, 0),
                        L_FATAL, volRef, file, "      ", ": Could not rewind file", ", so can not copy it, aborting ...") < 0) {
                    filesize = -1; // remember error
                    goto Exit;
                }
            }
        }
        if (equal) {
            jp_logf(L_DEBUG, "%s:       File '%s' already exists, not copying it.\n", MYNAME, lcPath);
            goto Exit;
        }
        // Find alternative destination file name, which not alredy exists, by inserting a number.
        char *insert = strrchr(lcPath, '.');
        for (char *i = lcPath + strlen(lcPath); i >= insert; i--)  *(i + 2) = *i;
        *insert++ = '_';  *insert = '1';
        for (; !stat(lcPath, &fstat); (*insert)++) {; // increment number by 1
            if (*insert >= '9') {
                jp_logf(L_WARN, "%s:               and even file '%s' already exists, so no new backup for '%s'.\n", MYNAME, lcPath, file);
                filesize = -1; // remember error
                goto Exit;
            }
        }
        jp_logf(L_WARN, "%s:               so backup to '%s'.\n", MYNAME, lcPath);
    }
    // File has not already been synced, backup it.
    if (!(fileP = fopen(lcPath, "wx"))) {
        jp_logf(L_FATAL, "%s:       ERROR: Cannot open %s for writing %d bytes!\n", MYNAME, lcPath, filesize);
        filesize = -1; // remember error
        goto Exit;
    }
    // Copy file.
    jp_logf(L_INFO, "%s:      Backup '%s', size %d ...", MYNAME, rmPath, filesize);
    for (int remaining = filesize; remaining > 0; remaining -= piBuf->used) {
        if (fileRead(fileRef, NULL, piBuf, remaining) < 0)  {
            filesize = -1; // remember error
            break;
        }
        if (fileWrite(0, fileP, piBuf, remaining) < 0) {
            filesize = -1; // remember error
            break;
        }
    }
    fclose(fileP);
    if (filesize < 0) {
        unlink(lcPath); // remove the partially created file
        jp_logf(L_WARN, "%s:       WARNING: Deleted incomplete local file '%s'\n", MYNAME, lcPath);
    } else {
        jp_logf(L_INFO, " OK\n");
        // Get the date on that the picture was created.
        time_t date = getRemoteDate(fileRef, volRef, rmPath, NULL);
        if (date)  setLocalDate(lcPath, date);
    }
Exit:
    dlp_VFSFileClose(sd, fileRef);
    jp_logf(L_DEBUG, "%s:       Backup file size / copy result: %d, statErr=%d\n", MYNAME, filesize, statErr);
    return filesize;
}

/*
 * Restore a file to the remote Palm device.
 */
int restoreFile(const char *lcDir, const unsigned volRef, const char *rmDir, const char *file) {
    jp_logf(L_DEBUG, "%s:      restoreFile(lcDir='%s', volRef=%d, rmDir='%s', file='%s')\n", MYNAME, lcDir, volRef, rmDir, file);
    char lcPath[strlen(lcDir) + strlen(file) + 2];
    char rmPath[strlen(rmDir) + strlen(file) + 2];
    FILE *fileP;
    FileRef fileRef;
    int filesize; // also serves as error return

    strcat(strcat(strcpy(lcPath, lcDir), "/"), file);
    strcat(strcat(strcpy(rmPath, rmDir), "/"), file);

    struct stat fstat;
    int statErr;
    if ((statErr = stat(lcPath, &fstat))) {
        jp_logf(L_FATAL, "%s:       ERROR %d: Could not read status of %s.\n", MYNAME, statErr, lcPath);
        return -1;
    }
    filesize = fstat.st_size;
    if (!(fileP = fopen(lcPath, "r"))) {
        jp_logf(L_FATAL, "%s:       ERROR: Could not open %s for reading %d bytes,\n", MYNAME, lcPath, filesize);
        return -1;
    }
    if (piErrLog(dlp_VFSFileOpen(sd, volRef, rmPath, vfsModeReadWrite | vfsModeCreate, &fileRef),
            L_FATAL, volRef, rmPath, "      ", ": Could not open remote file", " for read/writing.") < 0) { // May not work on DLP,
    //~ // ... then first create file with:
    //~ if (piErrLog(dlp_VFSFileCreate(sd, volRef, rmPath), L_FATAL, volRef, rmPath, "      ", ": Could not create remote file","") < 0 ||
            //~ (piErrLog(dlp_VFSFileOpen(sd, volRef, rmPath, vfsModeWrite, &fileRef), // See: https://github.com/desrod/pilot-link/issues/10
            //~ (piErrLog(dlp_VFSFileOpen(sd, volRef, rmPath, vfsModeReadWrite, &fileRef),
            //~ L_FATAL, volRef, rmPath, "      ", ": Could not open remote file", " for read/writing.") < 0) {
        filesize = -1; // remember error
        goto Exit;
    }
    // Copy file.
    jp_logf(L_INFO, "%s:      Restore '%s', size %d ...", MYNAME, lcPath, filesize);
    for (int remaining = filesize; remaining > 0; remaining -= piBuf->used) {
        if (fileRead(0, fileP, piBuf, remaining) < 0) {
            filesize = -1; // remember error
            break;
        }
        if (fileWrite(fileRef, NULL, piBuf, remaining) < 0) {
            filesize = -1; // remember error
            break;
        }
    }
    setRemoteDate(fileRef, volRef, rmPath, fstat.st_mtime);
    dlp_VFSFileClose(sd, fileRef);
    if (filesize < 0) { // close and remove the partially created file
        if (piErrLog(dlp_VFSFileDelete(sd, volRef, rmPath), L_FATAL, volRef, rmPath, "      ", ": Not deleted remote file","") >= 0)
            jp_logf(L_WARN, "%s:       WARNING: Deleted incomplete remote file '%s' on volume %d\n", MYNAME, rmPath, volRef);
    } else
        jp_logf(L_INFO, " OK\n");

Exit:
    fclose(fileP);
    jp_logf(L_DEBUG, "%s:       Restore file size / copy result: %d, statErr=%d\n", MYNAME, filesize, statErr);
    return filesize;
}

int casecmpFileTypeList(const char *fname) {
    char *ext = strrchr(fname, '.');
    for (fullPath *item = fileTypeList; ext && item; item = item->next) {
        if (*(item->name) != '-') {
            if (!strcasecmp(ext + 1, item->name))  return 1; // backup & restore
        } else {
            if (!strcasecmp(ext + 1, item->name + 1))  return 0; // only backup
        }
    }
    return -1; // no match, no sync
}

int cmpRemote(VFSDirInfo dirInfos[], int dirItems, const char *fname) {
    int result = 1;
    for (int i=0; i<dirItems; i++) {
        if (!(result = strcmp(fname, dirInfos[i].name)))  break;
    }
    return result;
}

/*
 * Synchonize a remote album with the matching local album and backup or restore the containing files in them.
 */
PI_ERR syncAlbum(const unsigned volRef, FileRef dirRef, const char *rmRoot, DIR *dirP, const char *lcRoot, const char *name) {
    char rmTmp[name ? strlen(rmRoot) + strlen(name) + 2 : 0], *rmAlbum;
    char lcTmp[name ? strlen(lcRoot) + strlen(name) + 2 : 0], *lcAlbum;
    VFSDirInfo dirInfos[MAX_DIR_ITEMS];
    int dirItems = 0;
    struct stat fstat;
    int statErr;
    PI_ERR result = 0;

    if (name) {
        rmAlbum = strcat(strcat(strcpy(rmTmp ,rmRoot), "/"), name);
        if (!cmpExcludeDirList(volRef, rmAlbum))  return result;
        if (dirP) // indicates, that we are in restore-only mode, so
            dirItems = -1; // prevent search on remote album
        lcAlbum = strcpy(lcTmp, lcRoot);
        if (createLocalDir(lcAlbum, rmAlbum + strlen(rmRoot), dirP ? -1 : volRef, rmRoot)) { // in restore-only mode don't try to recover date
            return -2;
        } else if (!(dirP = opendir(lcAlbum))) {
            jp_logf(L_FATAL, "%s:    ERROR: Could not open dir '%s' on '%s'\n", MYNAME, name, lcRoot);
            return -2;
        }
        if (createRemoteDir(volRef, rmAlbum, NULL, lcAlbum) < 0) {
            result = -2;
            goto Exit1;
        } else if (piErrLog(dlp_VFSFileOpen(sd, volRef, rmAlbum, vfsModeRead, &dirRef),
                L_FATAL, volRef, rmAlbum, "   ", ": Could not open dir", "") < 0) {
            result = -2;
            goto Exit1;
        }
    } else {
        rmAlbum = (char *)rmRoot;
        lcAlbum = (char *)lcRoot;
        if (!cmpExcludeDirList(volRef, rmAlbum))  return result;
    }
    jp_logf(L_INFO, "%s:    Sync album '%s' in '%s' on volume %d ...\n", MYNAME, name ? name : ".", rmRoot, volRef);
    if (!dirItems) // We are in backup mode !
        dirItems = enumerateOpenDir(volRef, dirRef, rmAlbum, dirInfos);
    jp_logf(L_DEBUG, "%s:     Now first search of local files, which to restore ...\n", MYNAME);
    // First iterate over all the local files in the album dir, to prevent from back-storing renamed files,
    // so only looking for remotely unknown files ... and then restore them.
    for (struct dirent *entry; doRestore && (entry = readdir(dirP));) {
        jp_logf(L_DEBUG, "%s:      Found local file: '%s' type=%d\n", MYNAME, entry->d_name, entry->d_type);
        char lcAlbumPath[NAME_MAX];
        if ((statErr = stat(strcat(strcat(strcpy(lcAlbumPath, lcAlbum), "/"), entry->d_name), &fstat))) {
            jp_logf(L_FATAL, "%s:      ERROR %d: Could not read status of %s; No sync possible!\n", MYNAME, statErr, lcAlbumPath);
            result = MIN(result, -1);
            continue;
        }
        if (S_ISREG(fstat.st_mode) // use fstat to follow symlinks; (entry->d_type != DT_REG) doesn't do this
                && strlen(entry->d_name) > 2
                && casecmpFileTypeList(entry->d_name) > 0
                && cmpRemote(dirInfos, dirItems, entry->d_name)) {
            //~ jp_logf(L_DEBUG, "%s:      Restore local file: '%s' to '%s'\n", MYNAME, entry->d_name, rmAlbum);
            int restoreResult = restoreFile(lcAlbum, volRef, rmAlbum, entry->d_name);
            result = MIN(result, restoreResult);
        }
    }
    jp_logf(L_DEBUG, "%s:     Now search of %d remote files, which to backup ...\n", MYNAME, dirItems);
    // Iterate over all the remote files in the album dir, looking for un-synced files.
    for (int i=0; doBackup && i<dirItems; i++) {
        char *fname = dirInfos[i].name;
        jp_logf(L_DEBUG, "%s:      Found remote file '%s' attributes=%x\n", MYNAME, fname, dirInfos[i].attr);
        // Grab only regular files, but ignore the 'read only' and 'archived' bits,
        // and only with known extensions.
        if (!(dirInfos[i].attr & (
                vfsFileAttrHidden      |
                vfsFileAttrSystem      |
                vfsFileAttrVolumeLabel |
                vfsFileAttrDirectory   |
                vfsFileAttrLink))
                && strlen(fname) > 1
                && casecmpFileTypeList(fname) >= 0) {
            //~ jp_logf(L_DEBUG, "%s:      Backup remote file: '%s' to '%s'\n", MYNAME, fname, lcAlbum);
            int backupResult = backupFileIfNeeded(volRef, rmAlbum, lcAlbum, fname);
            result = MIN(result, backupResult);
        }
    }
    time_t date = getRemoteDate(dirRef, volRef, rmAlbum, NULL);
    if (doBackup && date)  setLocalDate(lcAlbum, date); // always recover folder date from remote // ToDo: maybe do by BackupFileIfNeeded()
    if (name)  dlp_VFSFileClose(sd, dirRef);
Exit1:
    if (name)  closedir(dirP);
    else  rewinddir(dirP);
    jp_logf(L_DEBUG, "%s:    Album '%s' done -> result=%d\n", MYNAME,  rmAlbum, result);
    return result;
}

/*
 *  Backup all albums from volume volRef.
 */
PI_ERR syncVolume(int volRef) {
    PI_ERR rootResult = -3, result = 0;

    jp_logf(L_DEBUG, "%s:  Searching roots on volume %d\n", MYNAME, volRef);
    for (fullPath *item = rootDirList; item; item = item->next) {
        if ((item->volRef >= 0 && volRef != item->volRef))
            continue;
        char *rootDir = item->name;
        VFSDirInfo dirInfos[MAX_DIR_ITEMS];

        // Open the remote root directory.
        FileRef dirRef;
        if (piErrLog(dlp_VFSFileOpen(sd, volRef, rootDir, vfsModeRead, &dirRef), L_DEBUG, volRef, rootDir, "  ", ": Root", "; seems not to exist.") < 0)
            continue;
        jp_logf(L_DEBUG, "%s:   Opened remote root '%s' on volume %d\n", MYNAME, rootDir, volRef);
        rootResult = 0;

        // Open the local root directory.
        char *lcRoot = localRoot(volRef);
        if (!lcRoot)
            goto Continue;
        DIR *dirP;
        if (!(dirP = opendir(lcRoot))) {
            jp_logf(L_DEBUG, "%s:   Root '%s' does not exist on '%s'\n", MYNAME, lcRoot + strlen(mediaHome), mediaHome);
            goto Continue;
        }
        jp_logf(L_DEBUG, "%s:   Opened local root '%s' on '%s'\n", MYNAME, lcRoot + strlen(mediaHome), mediaHome);

        // Fetch the unfiled album, which is simply the root dir, and sync it.
        // Apparently the Treo 650 can store media in the root dir, as well as in album dirs.
        result = syncAlbum(volRef, dirRef, rootDir, dirP, lcRoot, NULL);

        // Iterate through the remote root directory, looking for things that might be albums.
        int dirItems = 0;
        //~ enum dlpVFSFileIteratorConstants itr = vfsIteratorStart;
        //~ while (itr != vfsIteratorStop) { // doesn't work because of type mismatch bug <https://github.com/juddmon/jpilot/issues/39>
        unsigned long itr = (unsigned long)vfsIteratorStart;
        for (int batch; (enum dlpVFSFileIteratorConstants)itr != vfsIteratorStop; dirItems += batch) {
            batch = MIN(MAX_DIR_ITEMS / 2, MAX_DIR_ITEMS - dirItems);
            jp_logf(L_DEBUG, "%s:   Enumerate root '%s': dirRef=%8lx, itr=%4lx, batch=%d, dirItems=%d\n", MYNAME, rootDir, dirRef, itr, batch, dirItems);
            PI_ERR piErr;
            if ((piErr = piErrLog(dlp_VFSDirEntryEnumerate(sd, dirRef, &itr, &batch, dirInfos + dirItems),
                    L_FATAL, volRef, rootDir, "  ", ": Could not enumerate dir", "")) < 0) {
                // Crashes on empty directory (see: <https://github.com/juddmon/jpilot/issues/??>):
                // Further research is neccessary (see: <https://github.com/juddmon/jpilot/issues/41> ):
                // - Why in case of i.e. setting batch=4, itr == vfsIteratorStop, even if there are more than 4 files?
                // - For workaround and additional bug on SDCard volume, see at syncAlbum()
                //~ jp_logf(L_DEBUG, "%s:   Enumerate ERROR %4d: dirRef=%8lx, itr=%4lx, batch=%d\n", MYNAME, piErr, dirRef, itr, batch);
                rootResult = -3;
                break;
            //~ } else {
                //~ jp_logf(L_DEBUG, "%s:   Enumerate OK %4d, dirRef=%8lx, itr=%4lx, batch=%d\n", MYNAME, piErr, dirRef, itr, batch);
            }
            jp_logf(L_DEBUG, "%s:   Now search for remote albums on Volume %d in '%s' to sync ...\n", MYNAME, volRef, rootDir);
            for (int i = dirItems; i < (dirItems + batch); i++) {
                jp_logf(L_DEBUG, "%s:    Found remote album candidate '%s' in '%s'; attributes=%x\n", MYNAME,  dirInfos[i].name, rootDir, dirInfos[i].attr);
                if (dirInfos[i].attr & vfsFileAttrDirectory
                        && (syncThumbnailDir || strcmp(dirInfos[i].name, "#Thumbnail"))) { // Treo 650 has #Thumbnail dir that is not an album
                    jp_logf(L_DEBUG, "%s:    Found real remote album '%s' in '%s'\n", MYNAME, dirInfos[i].name, rootDir);
                    PI_ERR albumResult = syncAlbum(volRef, 0, rootDir, NULL, lcRoot, dirInfos[i].name);
                    result = MIN(result, albumResult);
                }
            }
        }

        // Now iterate over all the local files in the album dir. To prevent from back-storing renamed files,
        // only looking for remotely unknown albums ... and then restore them.
        jp_logf(L_DEBUG, "%s:   Now search for local albums in '%s' to restore ...\n", MYNAME, lcRoot);
        for (struct dirent *entry; (entry = readdir(dirP));) {
            jp_logf(L_DEBUG, "%s:    Found local album candidate '%s' in '%s'; type %d\n", MYNAME, entry->d_name, lcRoot + strlen(mediaHome) + 1, entry->d_type);
            struct stat fstat;
            int statErr;
            char lcAlbum[NAME_MAX];
            if ((statErr = stat(strcat(strcat(strcpy(lcAlbum, lcRoot), "/"), entry->d_name), &fstat))) {
                jp_logf(L_FATAL, "%s:    ERROR %d: Could not read status of %s; No sync possible!\n", MYNAME, statErr, lcAlbum);
                result = MIN(result, -2);
                continue;
            }
            if (S_ISDIR(fstat.st_mode) // use fstat to follow symlinks; (entry->d_type != DT_Dir) doesn't do this
                    && strcmp(entry->d_name, ".")
                    && strcmp(entry->d_name, "..")
                    && (syncThumbnailDir || strcmp(entry->d_name, "#Thumbnail")) // Treo 650 has #Thumbnail dir that is not an album
                    && strcmp(entry->d_name, ADDITIONAL_FILES + 1)
                    && cmpRemote(dirInfos, dirItems, entry->d_name)) {
                jp_logf(L_DEBUG, "%s:    Found real local album '%s' in '%s'\n", MYNAME, entry->d_name, lcRoot + strlen(mediaHome) + 1);
                PI_ERR albumResult = syncAlbum(volRef, 0, rootDir, dirP, lcRoot, entry->d_name);
                result = MIN(result, albumResult);
            }
        }

        closedir(dirP);
        // Reset date of lcRoot
        time_t date = getRemoteDate(dirRef, volRef, rootDir, NULL);
        if (date)  setLocalDate(lcRoot, date);
Continue:
        dlp_VFSFileClose(sd, dirRef);
    }
    jp_logf(L_DEBUG, "%s:  Volume %d done -> rootResult=%d, result=%d\n", MYNAME,  volRef, rootResult, result);
    return rootResult + result;
}

/***********************************************************************
 *
 * Function:      volumeEnumerateIncludeHidden
 *
 * Summary:       Drop-in replacement for dlp_VFSVolumeEnumerate().
 *                Attempts to include hidden volumes in the list,
 *                so that we also get the device's BUILTIN volume.
 *                Dan Bodoh, May 2, 2008
 *
 * Parameters:
 *  sd            --> socket descriptor
 *  volume_count  <-> on input, size of volumes; on output
 *                    number of volumes on Palm
 *  volumes       <-- volume reference numbers
 *
 * Returns:       <-- same as dlp_VFSVolumeEnumerate()
 *
 ***********************************************************************/
int volumeEnumerateIncludeHidden(const int sd, int *numVols, int *volRefs) {
    PI_ERR   piErr;
    VFSInfo  volInfo;

    // piErr on Treo 650:
    // -301 : PalmOS Error. Probably no volume (SDCard) found, but maybe hidden volume 1 exists
    //    4 : At least one volume found, but maybe additional hidden volume 1 exists
    piErr = dlp_VFSVolumeEnumerate(sd, numVols, volRefs);
    jp_logf(L_DEBUG, "%s: dlp_VFSVolumeEnumerate(): %s; found %d volumes\n", MYNAME, errString(1, piErr, L_DEBUG, ""), *numVols);
    // On the Centro, Treo 650 and maybe more, it appears that the first non-hidden volRef is 2, and the hidden volRef is 1.
    // Let's poke around to see, if there is really a volRef 1 that's hidden from the dlp_VFSVolumeEnumerate().
    if (piErr < 0)  *numVols = 0; // On Error reset numVols
    for (int i=0; i<*numVols; i++) { // Search for volume 1
        jp_logf(L_DEBUG, "%s: *numVols=%d, volRefs[%d]=%d\n", MYNAME, *numVols, i, volRefs[i]);
        if (volRefs[i]==1)
            goto Exit; // No need to search for hidden volume
    }
    if (piErrLog(dlp_VFSVolumeInfo(sd, 1, &volInfo), L_FATAL, 1, "", "", ": Could not find info","") >= 0 && volInfo.attributes & vfsVolAttrHidden) {
        jp_logf(L_DEBUG, "%s: Found hidden volume 1\n", MYNAME);
        if (*numVols < MAX_VOLUMES)  (*numVols)++;
        else {
            jp_logf(L_FATAL, "%s: ERROR: Volumes > %d were discarded\n", MYNAME, MAX_VOLUMES);
        }
        for (int i = (*numVols)-1; i > 0; i--) { // Move existing volRefs
            jp_logf(L_DEBUG, "%s: *numVols=%d, volRefs[%d]=%d, volRefs[%d]=%d\n", MYNAME, *numVols, i-1, volRefs[i-1], i, volRefs[i]);
            volRefs[i] = volRefs[i-1];
        }
        volRefs[0] = 1;
        if (piErr < 0)
            piErr = 4; // fake dlp_VFSVolumeEnumerate() with 1 volume return value
    }
Exit:
    jp_logf(L_DEBUG, "%s: volumeEnumerateIncludeHidden(): Found %d volumes -> piErr=%d\n", MYNAME, *numVols, piErr);
    return piErr;
}
