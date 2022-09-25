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

#define L_DEBUG JP_LOG_DEBUG
#define L_INFO  JP_LOG_INFO // Unfortunately doesn't show up in GUI
#define L_WARN  JP_LOG_WARN
#define L_FATAL JP_LOG_FATAL
#define L_GUI   JP_LOG_GUI

typedef struct VFSInfo VFSInfo;
typedef struct VFSDirInfo VFSDirInfo;
typedef struct fileType {char ext[16]; struct fileType *next;} fileType;
typedef struct fullPath {int volRef; char *dir; struct fullPath *next;} fullPath;

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
    {"prefsVersion", INTTYPE, INTTYPE, 2, NULL, 0},
    {"rootDirs", CHARTYPE, CHARTYPE, 0, "1>/Photos & Videos:1>/Fotos & Videos:/DCIM", 0},
    {"syncThumbnailDir", INTTYPE, INTTYPE, 0, NULL, 0},
    // JPEG picture
    // video (GSM phones)
    // video (CDMA phones)
    // audio caption (GSM phones)
    // audio caption (CDMA phones)
    {"fileTypes", CHARTYPE, CHARTYPE, 0, ".jpg.amr.qcp.3gp.3g2.avi", 0},
    {"useDateModified", INTTYPE, INTTYPE, 0, NULL, 0},
    {"compareContent", INTTYPE, INTTYPE, 0, NULL, 0},
    {"doBackup", INTTYPE, INTTYPE, 1, NULL, 0},
    {"doRestore", INTTYPE, INTTYPE, 1, NULL, 0},
    {"listFiles", INTTYPE, INTTYPE, 0, NULL, 0},
    {"excludeDirs", CHARTYPE, CHARTYPE, 0, "/BLAZER:2>/PALM/Launcher", 0}
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

static const unsigned MAX_VOLUMES = 16;
static const unsigned MIN_DIR_ITEMS = 2;
static const unsigned MAX_DIR_ITEMS = 1024;
static const char *LOCALDIRS[] = {"Internal", "SDCard", "Card"};
static fullPath *rootDirList = NULL;
static fileType *fileTypeList = NULL;
static fullPath *excludeDirList = NULL;
static pi_buffer_t *piBuf, *piBuf2;
static int sd; // the central socket descriptor.
static char lcPath[NAME_MAX];
static char syncLogEntry[128];
static int importantWarning = 0;


static void *mallocLog(size_t size);
int parsePaths(char *paths, fullPath **list, char *text);
int volumeEnumerateIncludeHidden(const int sd, int *numVols, int *volRefs);
int syncVolume(int volRef);
PI_ERR listRemoteFiles(int volRef, const char *dir, const int indent);

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
    int result = EXIT_SUCCESS;
    jp_init();
    jp_pref_init(prefs, NUM_PREFS);
    if (jp_pref_read_rc_file(PREFS_FILE, prefs, NUM_PREFS) < 0)
        jp_logf(L_WARN, "%s: WARNING: Could not read prefs[] from '%s'\n", MYNAME, PREFS_FILE);
    if (jp_pref_write_rc_file(PREFS_FILE, prefs, NUM_PREFS) < 0) // To initialize with defaults, if pref file wasn't existent.
        jp_logf(L_WARN, "%s: WARNING: Could not write prefs[] to '%s'\n", MYNAME, PREFS_FILE);
    jp_get_pref(prefs, 0, &prefsVersion, NULL);
    jp_get_pref(prefs, 1, NULL, (const char **)&rootDirs);
    jp_get_pref(prefs, 2, &syncThumbnailDir, NULL);
    jp_get_pref(prefs, 3, NULL, (const char **)&fileTypes);
    jp_get_pref(prefs, 4, &useDateModified, NULL);
    jp_get_pref(prefs, 5, &compareContent, NULL);
    jp_get_pref(prefs, 6, &doBackup, NULL);
    jp_get_pref(prefs, 7, &doRestore, NULL);
    jp_get_pref(prefs, 8, &listFiles, NULL);
    jp_get_pref(prefs, 9, NULL, (const char **)&excludeDirs);
    if (!result && strlen(rootDirs) > 0)
        result = parsePaths(rootDirs, &rootDirList, "rootDirList");
    for (char *last; !result && (last = strrchr(fileTypes, '.')) >= fileTypes; *last = '\0') {
        if (last > fileTypes && *(last - 1) == '.') // found ".." separator
            last--;
        fileType *ftype;
        if (strlen(last) < sizeof(ftype->ext) && (ftype = mallocLog(sizeof(*ftype)))) {
            strcpy(ftype->ext, last);
            ftype->next = fileTypeList;
            fileTypeList = ftype;
        } else {
            plugin_exit_cleanup();
            result = EXIT_FAILURE;
            break;
        }
        jp_logf(L_DEBUG, "%s: Got fileTypeList item: extension '%s'\n", MYNAME, fileTypeList->ext);
    }
    if (!result && strlen(excludeDirs) > 0)
        result = parsePaths(excludeDirs, &excludeDirList, "excludeDirList");

    if (!result && (result = !(piBuf = pi_buffer_new(32768)) || !(piBuf2 = pi_buffer_new(32768))))
        jp_logf(L_FATAL, "%s: ERROR: Out of memory\n", MYNAME);
    return result;
}

int plugin_sync(int socket) {
    int volRefs[MAX_VOLUMES];
    int volumes = MAX_VOLUMES;
    sd = socket;

    jp_logf(L_GUI, "%s: Start syncing ...", MYNAME);
    jp_logf(L_DEBUG, "\n");

    // Get list of the volumes on the pilot.
    if (volumeEnumerateIncludeHidden(sd, &volumes, volRefs) < 0) {
        jp_logf(L_FATAL, "\n%s: ERROR: Could not find any VFS volumes; no media synced.\n", MYNAME);
        return EXIT_FAILURE;
    }
    // Use $JPILOT_HOME/.jpilot/ or current directory for PCDIR.
    if (jp_get_home_file_name(PCDIR, lcPath, NAME_MAX) < 0) {
        jp_logf(L_WARN, "\n%s: WARNING: Could not get $JPILOT_HOME path, so using './%s'.\n", MYNAME, PCDIR);
        strcpy(lcPath, PCDIR);
    } else {
        jp_logf(L_GUI, " with '%s'\n", lcPath);
    }
    // Check if there are any file types loaded.
    if (!fileTypeList) {
        jp_logf(L_FATAL, "%s: ERROR: Could not find any file types from '%s'; No media synced.\n", MYNAME, PREFS_FILE);
        return EXIT_FAILURE;
    }

    // Scan all the volumes for media and backup them.
    int result = EXIT_FAILURE;
    for (int i=0; i<volumes; i++) {
        if (listFiles) { // List all files from the Palm device, but don't sync.
            if (listRemoteFiles(volRefs[i], "/", 1) >= 0)
                result = EXIT_SUCCESS;
            continue;
        }
        PI_ERR volResult;
        if ((volResult = syncVolume(volRefs[i])) < -2) {
            snprintf(syncLogEntry, sizeof(syncLogEntry),
                    "%s:  WARNING: Could not find any media on volume %d; No media synced.\n", MYNAME, volRefs[i]);
            jp_logf(L_WARN, syncLogEntry);
            dlp_AddSyncLogEntry (sd, syncLogEntry);
            goto Continue;
        } else if (volResult < 0) {
            snprintf(syncLogEntry, sizeof(syncLogEntry),
                    "%s:  WARNING: Errors occured on volume %d; Some media may not be synced.\n", MYNAME, volRefs[i]);
            jp_logf(L_WARN, syncLogEntry);
            dlp_AddSyncLogEntry (sd, syncLogEntry);
        }
        result = EXIT_SUCCESS;
Continue:
    }
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

int plugin_exit_cleanup(void) {
    pi_buffer_free(piBuf);
    pi_buffer_free(piBuf2);
    for (fileType *item; (item = fileTypeList);) {
        fileTypeList = fileTypeList->next;
        free(item);
    }
    for (fullPath *item; (item = excludeDirList);) {
        excludeDirList = excludeDirList->next;
        free(item);
    }
    jp_free_prefs(prefs, NUM_PREFS);
    return EXIT_SUCCESS;
}

// ToDo: Rename picsnvideos ./. media

/* Log OOM error on malloc(). */
static void *mallocLog(size_t size) {
    void *p;
    if (!(p = malloc(size)))
        jp_logf(L_FATAL, "%s: ERROR: Out of memory\n", MYNAME);
    return p;
}

int parsePaths(char *paths, fullPath **list, char *text) {
    for (char *last; ; *--last = '\0') {
        last = (last = strrchr(paths, ':')) ? last + 1 : paths;
        fullPath *path;
        if ((path = mallocLog(sizeof(*path)))) {
            char *separator = strchr(last, '>');
            if (separator) {
                *separator = '\0';
                path->volRef = atoi(last);
                path->dir = separator + 1;
            } else {
                path->volRef = -1;
                path->dir = last;
            }
            path->next = *list;
            (*list) = path;
        } else {
            plugin_exit_cleanup();
            return EXIT_FAILURE;
        }
        jp_logf(L_DEBUG, "%s: Got %s item: dir '%s' on Volume %d\n", MYNAME, text, (*list)->dir, (*list)->volRef);
        if (last == paths)
            break;
    }
    return EXIT_SUCCESS;
}

int createDir(char *path, const char *dir) {
    if (dir)  strcat(strcat(path, "/"), dir);
    int result;
    if ((result = mkdir(path, 0777)) && errno != EEXIST) {
        jp_logf(L_FATAL, "%s:     ERROR: Could not create directory %s\n", MYNAME, path);
        return result;
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

    if (createDir(strcpy(path, lcPath), NULL))  return NULL;
    // Get indicator of which card.
    if (dlp_VFSVolumeInfo(sd, volRef, &volInfo) < 0) {
        jp_logf(L_FATAL, "%s:     ERROR: Could not get volume info from volRef %d\n", MYNAME, volRef);
        return NULL;
    }
    if (volInfo.mediaType == pi_mktag('T', 'F', 'F', 'S')) {
        if (createDir(path, LOCALDIRS[0]))  return NULL;
    } else if (volInfo.mediaType == pi_mktag('s', 'd', 'i', 'g')) {
        if (createDir(path, LOCALDIRS[1]))  return NULL;
    } else {
        sprintf(path+strlen(path), "/%s%d", LOCALDIRS[2], volInfo.slotRefNum);
        if (createDir(path, NULL))  return NULL;
    }
    return path; // must not be free'd by caller
}

int enumerateOpenDir(const int volRef, FileRef dirRef, const char *rmDir, VFSDirInfo dirInfos[]) {
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
        //~ jp_logf(L_DEBUG, "%s:      Enumerate remote dir '%s', dirRef=%8lx, itr=%4lx, dirItems=%d\n", MYNAME, rmDir, dirRef, itr, dirItems);
        if ((piErr = dlp_VFSDirEntryEnumerate(sd, dirRef, &itr, &dirItems, dirInfos)) < 0) {
            // Crashes on empty directory (see: <https://github.com/desrod/pilot-link/issues/11>):
            // Further research is neccessary (see: <https://github.com/juddmon/jpilot/issues/41>):
            // - Why in case of i.e. setting dirItems=4, itr != 0, even if there are more than 4 files?
            // - Why then on SDCard itr == 1888 in the first loop, so out of allowed range?
            jp_logf(L_FATAL, "%s:      Enumerate ERROR: %4d; rmDir=%s, dirRef=%8lx, itr=%4lx, dirItems=%d\n", MYNAME, piErr, rmDir, dirRef, itr, dirItems);
            if (piErr == PI_ERR_DLP_PALMOS)
                jp_logf(L_FATAL, "%s:      ERROR: PalmOS error: %d.\n", MYNAME, pi_palmos_error(sd));
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

int enumerateDir(const int volRef, const char *rmDir, VFSDirInfo dirInfos[]) {
    FileRef dirRef;
    PI_ERR piErr;
    if ((piErr = dlp_VFSFileOpen(sd, volRef, rmDir, vfsModeRead, &dirRef)) < 0) {
        jp_logf(L_FATAL, "%s:       ERROR: %d; Could not open dir '%s' on volume %d\n", MYNAME, piErr, rmDir, volRef);
        if (piErr == PI_ERR_DLP_PALMOS)
            jp_logf(L_FATAL, "%s:      ERROR: PalmOS error: %d.\n", MYNAME, pi_palmos_error(sd));
        return piErr;
    } else {
        int dirItems = enumerateOpenDir(volRef, dirRef, rmDir, dirInfos);
        dlp_VFSFileClose(sd, dirRef);
        return dirItems;
    }
}

int cmpExcludeDirList(const int volRef, const char *dname) {
    int result = 1;
    for (fullPath *item = excludeDirList; dname && item; item = item->next) {
        if ((item->volRef < 0 || volRef == item->volRef) && !(result = strcmp(dname, item->dir)))  break;
    }
    return result;
}

char *isoTime(const time_t *time) {
    static char isoTime[20];
    strftime(isoTime, 20, "%F %T", localtime (time));
    return isoTime;
}

/* List root directories for developing purposes. */
PI_ERR listRemoteFiles(const int volRef, const char *rmDir, const int depth) {
    FileRef fileRef;
    VFSDirInfo dirInfos[MAX_DIR_ITEMS];
    char prefix[] = MYNAME":                 ";
    prefix[MIN(sizeof(prefix) - 1, strlen(MYNAME) + 2 + depth)] = '\0';

    if (!cmpExcludeDirList(volRef, rmDir))  return -1; // avoid bug <https://github.com/desrod/pilot-link/issues/11>
    int dirItems = enumerateDir(volRef, rmDir, dirInfos);
    jp_logf(L_DEBUG, "%s%d remote files in '%s' on Volume %d ...\n", prefix, dirItems, rmDir, volRef);
    for (int i = 0; i < dirItems; i++) {
        char child[NAME_MAX];
        int filesize = 0;
        //~ time_t dateCre = 0, dateMod = 0;
        time_t dateCre = 0;

        strncat(strncat(strcpy(child, strcmp(rmDir, "/") ? rmDir : ""), "/", NAME_MAX-1), dirInfos[i].name, NAME_MAX-1);
        //~ strncat(strcmp(rmDir, "/") ? strncat(strcat(child, rmDir), "/", NAME_MAX-1) : child, dirInfos[i].name, NAME_MAX-1); // for paths without '/' at start
        if (dlp_VFSFileOpen(sd, volRef, child, vfsModeRead, &fileRef) < 0) {
            jp_logf(L_DEBUG, "%s WARNING: Cannot get size/date from %s\n", prefix, dirInfos[i].name);
        } else {
            if (!(dirInfos[i].attr & vfsFileAttrDirectory) && (dlp_VFSFileSize(sd, fileRef, &filesize) < 0))
                jp_logf(L_DEBUG, "%s WARNING: Could not get size of     %s\n", prefix, dirInfos[i].name);
            if (dlp_VFSFileGetDate(sd, fileRef, vfsFileDateCreated, &dateCre) < 0)
                jp_logf(L_DEBUG, "%s WARNING: No 'date created' from    %s\n", prefix, dirInfos[i].name);
            //~ if (dlp_VFSFileGetDate(sd, fileRef, vfsFileDateModified, &dateMod) < 0) // seems to be ignored by Palm
                //~ jp_logf(L_WARN, "%s WARNING: No 'date modified' from   %s\n", prefix, dirInfos[i].name);
            dlp_VFSFileClose(sd, fileRef);
        }
        //~ jp_logf(L_DEBUG, "%s 0x%02x%10d %s %s %s\n", prefix, dirInfos[i].attr, filesize, isoTime(&dateCre), isoTime(&dateMod), dirInfos[i].name);
        jp_logf(L_DEBUG, "%s 0x%02x%10d %s %s\n", prefix, dirInfos[i].attr, filesize, isoTime(&dateCre), dirInfos[i].name);
        if (dirInfos[i].attr & vfsFileAttrDirectory && depth < listFiles) {
            listRemoteFiles(volRef, child, depth + 1);
        }
    }
    //~ jp_logf(L_DEBUG, "%sList remote files in '%s' on volume %d done -> dirItems=%d\n", prefix, rmDir, volRef, dirItems);
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
            jp_logf(L_FATAL, "\n%s:       ERROR: File read error: %d; aborting at %d bytes left.\n", MYNAME, readsize, remaining - buf->used);
            if (fileRef && readsize == PI_ERR_DLP_PALMOS)
                jp_logf(L_FATAL, "%s:       ERROR: PalmOS error: %d.\n", MYNAME, pi_palmos_error(sd));
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
            jp_logf(L_FATAL, "\n%s:       ERROR: File write error: %d; aborting at %d bytes left, offset=%d.\n", MYNAME, writesize, remaining - offset, offset);
            if (fileRef && writesize == PI_ERR_DLP_PALMOS)
                jp_logf(L_FATAL, "%s:       ERROR: PalmOS error: %d.\n", MYNAME, pi_palmos_error(sd));
            return writesize;
        }
    }
    return (int)buf->used;
}

int fileCompare(FileRef fileRef, FILE *fileP, int filesize) {
    int result;
    for (int todo = filesize; todo > 0; todo -= piBuf->used) {
        if (fileRead(fileRef, NULL, piBuf, todo) < 0 || fileRead(0, fileP, piBuf2, todo) < 0 || piBuf->used != piBuf2->used) {
            jp_logf(L_FATAL, "%s:       ERROR: reading files for comparison, so assuming different ...\n", MYNAME);
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
    char rmPath[strlen(rmDir) + strlen(file) + 2];
    char lcPath[strlen(lcDir) + strlen(file) + 4]; // prepare for possible rename
    FileRef fileRef;
    FILE *fileP;
    int filesize; // also serves as error return code

    strcat(strcat(strcpy(rmPath, rmDir), "/"), file);
    strcat(strcat(strcpy(lcPath, lcDir), "/"), file);

    if (dlp_VFSFileOpen(sd, volRef, rmPath, vfsModeRead, &fileRef) < 0) {
        jp_logf(L_FATAL, "%s:       ERROR: Could not open remote file '%s' on volume %d for reading.\n", MYNAME, rmPath, volRef);
        return -1;
    }
    if (dlp_VFSFileSize(sd, fileRef, &filesize) < 0) {
        jp_logf(L_WARN, "%s:       WARNING: Could not get size of '%s' on volume %d, so anyway backup it.\n", MYNAME, rmPath, volRef);
        filesize = 0;
    }

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
                if (dlp_VFSFileSeek(sd, fileRef, vfsOriginBeginning, 0) < 0) {
                    jp_logf(L_FATAL, "%s:       ERROR: On file seek; So can not copy '%s', aborting ...\n", MYNAME, file);
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
    jp_logf(L_GUI, "%s:      Backup '%s', size %d ...", MYNAME, rmPath, filesize);
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
        jp_logf(L_GUI, " OK\n");
        time_t date;
        // Get the date on that the picture was created.
        if (dlp_VFSFileGetDate(sd, fileRef, useDateModified ? vfsFileDateModified : vfsFileDateCreated, &date) < 0) {
            jp_logf(L_WARN, "%s:       WARNING: Cannot get date of file '%s' on volume %d\n", MYNAME, rmPath, volRef);
            statErr = 0; // reset old state
        // And set the destination file modified time to that date.
        } else if (!(statErr = stat(lcPath, &fstat))) {
            //jp_logf(L_DEBUG, "%s:       modified: %s", MYNAME, ctime(&date));
            struct utimbuf utim;
            utim.actime = (time_t)fstat.st_atime;
            utim.modtime = date;
            statErr = utime(lcPath, &utim);
        }
        if (statErr) {
            jp_logf(L_WARN, "%s:       WARNING: Could not set date of file '%s', ErrCode=%d\n", MYNAME, lcPath, statErr);
        }
    }
Exit:
    dlp_VFSFileClose(sd, fileRef);
    jp_logf(L_DEBUG, "%s:       Backup file size / copy result: %d, statErr=%d\n", MYNAME, filesize, statErr);
    return filesize;
}

/*
 * Restore a file to the remote Palm device.
 */
int restoreFile(const unsigned volRef, const char *lcDir, const char *rmDir, const char *file) {
    char lcPath[strlen(lcDir) + strlen(file) + 2];
    char rmPath[strlen(rmDir) + strlen(file) + 2];
    FILE *fileP;
    FileRef fileRef;
    int filesize; // also serves as error return
    PI_ERR piErr;

    strcat(strcat(strcpy(lcPath, lcDir), "/"), file);
    strcat(strcat(strcpy(rmPath, rmDir), "/"), file);

    struct stat fstat;
    int statErr;
    if ((statErr = stat(lcPath, &fstat))) {
        jp_logf(L_FATAL, "%s:       ERROR: %d; Could not read status of %s.\n", MYNAME, statErr, lcPath);
        return -1;
    }
    filesize = fstat.st_size;
    if (!(fileP = fopen(lcPath, "r"))) {
        jp_logf(L_FATAL, "%s:       ERROR: Could not open %s for reading %d bytes,\n", MYNAME, lcPath, filesize);
        return -1;
    }
    if ((piErr = dlp_VFSFileOpen(sd, volRef, rmPath, vfsModeReadWrite | vfsModeCreate, &fileRef)) < 0) { // May not work on DLP,
    //~ // ... then first create file with:
    //~ if ((piErr = dlp_VFSFileCreate(sd, volRef, rmPath)) < 0) {
        //~ jp_logf(L_FATAL, "%s:       ERROR: %d; Could not create remote file '%s' on volume %d.\n", MYNAME, piErr, rmPath, volRef);
        //~ if (piErr == PI_ERR_DLP_PALMOS)
            //~ jp_logf(L_FATAL, "%s:        ERROR: PalmOS error: %d.\n", MYNAME, pi_palmos_error(sd));
        //~ filesize = -1; // remember error
        //~ goto Exit;
    //~ }
    //~ if ((piErr = dlp_VFSFileOpen(sd, volRef, rmPath, vfsModeReadWrite, &fileRef)) < 0) {
    //~ if ((piErr = dlp_VFSFileOpen(sd, volRef, rmPath, vfsModeWrite, &fileRef)) < 0) { // See: https://github.com/desrod/pilot-link/issues/10
        jp_logf(L_FATAL, "%s:       ERROR: %d; Could not open remote file '%s' on volume %d for read/writing.\n", MYNAME, piErr, rmPath, volRef);
        if (piErr == PI_ERR_DLP_PALMOS)
            jp_logf(L_FATAL, "%s:       ERROR: PalmOS error: %d.\n", MYNAME, pi_palmos_error(sd));
        filesize = -1; // remember error
        goto Exit;
    }

    // Copy file.
    jp_logf(L_GUI, "%s:      Restore '%s', size %d ...", MYNAME, lcPath, filesize);
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
    if (filesize < 0) {
        dlp_VFSFileClose(sd, fileRef);
        if ((piErr = dlp_VFSFileDelete(sd, volRef, rmPath))) { // remove the partially created file
            jp_logf(L_FATAL, "%s:       ERROR: %d; Not deleted remote file '%s'\n", MYNAME, piErr, rmPath);
            if (piErr == PI_ERR_DLP_PALMOS)
                jp_logf(L_FATAL, "%s:       ERROR: PalmOS error: %d.\n", MYNAME, pi_palmos_error(sd));
        } else
            jp_logf(L_WARN, "%s:       WARNING: Deleted incomplete remote file '%s' on volume %d\n", MYNAME, rmPath, volRef);
    } else {
        jp_logf(L_GUI, " OK\n");
        time_t date = fstat.st_mtime;
        // Set both dates of the file (DateCreated is displayed in Media App on Palm device); must not be before 1980, otherwise PalmOS error.
        //~ if ((piErr = dlp_VFSFileSetDate(sd, fileRef, vfsFileDateCreated, date + 31557384)) < 0 ||
        //~ if ((piErr = dlp_VFSFileSetDate(sd, fileRef, vfsFileDateCreated, date)) < 0 ||
                //~ (piErr = dlp_VFSFileSetDate(sd, fileRef, vfsFileDateModified, date)) < 0) {
            //~ jp_logf(L_WARN, "%s:      WARNING: %d Could not set date of remote file '%s' on volume %d\n", MYNAME, piErr, rmPath, volRef);
            //~ if (piErr == PI_ERR_DLP_PALMOS)
                //~ jp_logf(L_FATAL, "%s:      ERROR: PalmOS error: %d.\n", MYNAME, pi_palmos_error(sd));
        //~ }
        if ((piErr = dlp_VFSFileSetDate(sd, fileRef, vfsFileDateCreated, date)) < 0) {
            jp_logf(L_WARN, "%s:      WARNING: %d Could not set created date of remote file '%s' on volume %d\n", MYNAME, piErr, rmPath, volRef);
            if (piErr == PI_ERR_DLP_PALMOS)
                jp_logf(L_FATAL, "%s:      ERROR: PalmOS error: %d.\n", MYNAME, pi_palmos_error(sd));
        }
        if ((piErr = dlp_VFSFileSetDate(sd, fileRef, vfsFileDateModified, date)) < 0) { // but modified date seems to be ignored
            jp_logf(L_WARN, "%s:      WARNING: %d Could not set modified date of remote file '%s' on volume %d\n", MYNAME, piErr, rmPath, volRef);
            if (piErr == PI_ERR_DLP_PALMOS)
                jp_logf(L_FATAL, "%s:      ERROR: PalmOS error: %d.\n", MYNAME, pi_palmos_error(sd));
        }
    }
    dlp_VFSFileClose(sd, fileRef);

Exit:
    fclose(fileP);
    jp_logf(L_DEBUG, "%s:       Restore file size / copy result: %d, statErr=%d\n", MYNAME, filesize, statErr);
    return filesize;
}

int casecmpFileTypeList(const char *fname) {
    char *ext = strrchr(fname, '.');
    for (fileType *item = fileTypeList; ext && item; item = item->next) {
        if (*(item->ext + 1) != '.') { // not has ".." separator
            if (!strcasecmp(ext, item->ext))  return 1; // backup & restore
        } else {
            if (!strcasecmp(ext, item->ext + 1))  return 0; // only backup
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
    PI_ERR result = 0, piOSErr;

    if (name) {
        rmAlbum = strcat(strcat(strcpy(rmTmp ,rmRoot), "/"), name);
        if (!cmpExcludeDirList(volRef, rmAlbum))  return result;
        if (dirP) { // indicates, that we are in restore-only mode, so need to create a new remote album dir.
            jp_logf(L_DEBUG, "%s:    Try to create dir '%s' on volume %d\n", MYNAME, rmAlbum, volRef);
            if ((result = dlp_VFSDirCreate(sd, volRef, rmAlbum)) < 0) {
                if (result == PI_ERR_DLP_PALMOS && (piOSErr = pi_palmos_error(sd)) != 10758) { // File already exists.
                    jp_logf(L_FATAL, "%s:    ERROR %d: Could not create dir '%s' on volume %d\n", MYNAME, result, rmAlbum, volRef);
                    jp_logf(L_FATAL, "%s:    ERROR: PalmOS error: %d.\n", MYNAME, piOSErr);
                    return -2;
                }
            }
            importantWarning = 1;
            dirItems = -1; // prevent search on remote album
        }
        jp_logf(L_DEBUG, "%s:    Try to open dir '%s' on volume %d\n", MYNAME, rmAlbum, volRef);
        if (dlp_VFSFileOpen(sd, volRef, rmAlbum, vfsModeReadWrite, &dirRef) < 0) { // mode "Write" for setting date later
            jp_logf(L_FATAL, "%s:    ERROR: Could not open dir '%s' on volume %d\n", MYNAME, rmAlbum, volRef);
            return -2;
        }
        lcAlbum = strcpy(lcTmp, lcRoot);
        jp_logf(L_DEBUG, "%s:    Try to create dir '%s' in '%s'\n", MYNAME, name, lcRoot);
        if (createDir(lcAlbum, name)) {
            result = -2;
            goto Exit1;
        }
        if (dirP) { // indicates, that we are in restore-only mode, so need to set the date of the new created remote album dir.
            if ((statErr = stat(lcAlbum, &fstat))) {
                jp_logf(L_FATAL, "%s:    ERROR: %d; Could not read status of %s; No sync possible!\n", MYNAME, statErr, lcAlbum);
                result = -2;
                goto Exit1;
            }
            time_t date = fstat.st_mtime;
            // Set the date that the picture was created (not the file), aka modified time.
            if ((result = dlp_VFSFileSetDate(sd, dirRef, vfsFileDateModified, date)) < 0 ||
                    (result = dlp_VFSFileSetDate(sd, dirRef, vfsFileDateCreated, date)) < 0) {
                jp_logf(L_WARN, "%s:    WARNING: %d; Could not set date of remote file '%s' on volume %d\n", MYNAME, result, rmAlbum, volRef);
                if (result == PI_ERR_DLP_PALMOS)
                    jp_logf(L_FATAL, "%s:    ERROR: PalmOS error: %d.\n", MYNAME, pi_palmos_error(sd));
            }
        }
        jp_logf(L_DEBUG, "%s:    Try to open dir '%s' in '%s'\n", MYNAME, name, lcRoot);
        if (!(dirP = opendir(lcAlbum))) {
            jp_logf(L_FATAL, "%s:    ERROR: Could not open dir '%s' on '%s'\n", MYNAME, name, lcRoot);
            result = -2;
            goto Exit1;
        }
    } else {
        rmAlbum = (char *)rmRoot;
        lcAlbum = (char *)lcRoot;
        if (!cmpExcludeDirList(volRef, rmAlbum))  return result;
    }
    jp_logf(L_GUI, "%s:    Sync album '%s' in '%s' on volume %d ...\n", MYNAME, name ? name : ".", rmRoot, volRef);
    if (!dirItems) // We are in backup mode !
        dirItems = enumerateOpenDir(volRef, dirRef, rmAlbum, dirInfos);
    jp_logf(L_DEBUG, "%s:     Now first search of local files, which to restore ...\n", MYNAME);
    // First iterate over all the local files in the album dir, to prevent from back-storing renamed files,
    // so only looking for remotely unknown files ... and then restore them.
    for (struct dirent *entry; doRestore && (entry = readdir(dirP));) {
        jp_logf(L_DEBUG, "%s:      Found local file: '%s' type=%d\n", MYNAME, entry->d_name, entry->d_type);
        //~ // Intentionally remove the remote file for debugging purpose:
        //~ if (!strncasecmp(entry->d_name, "New_", 4)) { // Copy must exist locally, delete it after.
        //~ if (!strncasecmp(entry->d_name, "Thumb_", 6)) { // Copy must exist locally, delete it after.
        //~ if (!strcasecmp(entry->d_name, "Thumb_3746557696.thb")) { // Copy must exist locally, delete it after.
            //~ PI_ERR piErr;
            //~ char toDelete[NAME_MAX];
            //~ strcat(strcat(strcpy(toDelete ,rmAlbum), "/"), entry->d_name);
            //~ if ((piErr = dlp_VFSFileDelete(sd, volRef, toDelete))) {
                //~ jp_logf(L_FATAL, "%s:       ERROR: %d; Not deleted remote file '%s'\n", MYNAME, piErr, toDelete);
                //~ if (piErr == PI_ERR_DLP_PALMOS)
                    //~ jp_logf(L_FATAL, "%s:       ERROR: PalmOS error: %d\n", MYNAME, pi_palmos_error(sd));
            //~ } else
                //~ jp_logf(L_WARN, "%s:       WARNING: Deleted remote file '%s' on volume %d\n", MYNAME, toDelete, volRef);
            //~ }
        char lcAlbumPath[NAME_MAX];
        if ((statErr = stat(strcat(strcat(strcpy(lcAlbumPath, lcAlbum), "/"), entry->d_name), &fstat))) {
            jp_logf(L_FATAL, "%s:      ERROR: %d; Could not read status of %s; No sync possible!\n", MYNAME, statErr, lcAlbumPath);
            result = MIN(result, -1);
            continue;
        }
        if (!S_ISREG(fstat.st_mode) // use fstat to follow symlinks; (entry->d_type != DT_REG) doesn't do this
                || strlen(entry->d_name) < 2
                || casecmpFileTypeList(entry->d_name) <= 0
                || !cmpRemote(dirInfos, dirItems, entry->d_name)) {
            continue;
        }
        //~ jp_logf(L_DEBUG, "%s:      Restore local file: '%s' to '%s'\n", MYNAME, entry->d_name, rmAlbum);
        int restoreResult = restoreFile(volRef, lcAlbum, rmAlbum, entry->d_name);
        result = MIN(result, restoreResult);
    }
    jp_logf(L_DEBUG, "%s:     Now search of %d remote files, which to backup ...\n", MYNAME, dirItems);
    // Iterate over all the remote files in the album dir, looking for un-synced files.
    for (int i=0; doBackup && i<dirItems; i++) {
        char *fname = dirInfos[i].name;
        jp_logf(L_DEBUG, "%s:      Found remote file '%s' attributes=%x\n", MYNAME, fname, dirInfos[i].attr);
        // Grab only regular files, but ignore the 'read only' and 'archived' bits,
        // and only with known extensions.
        if (dirInfos[i].attr & (
                vfsFileAttrHidden      |
                vfsFileAttrSystem      |
                vfsFileAttrVolumeLabel |
                vfsFileAttrDirectory   |
                vfsFileAttrLink)
                || strlen(fname) < 2
                || casecmpFileTypeList(fname) < 0) {
            continue;
        }
        //~ jp_logf(L_DEBUG, "%s:      Backup remote file: '%s' to '%s'\n", MYNAME, fname, lcAlbum);
        int backupResult = backupFileIfNeeded(volRef, rmAlbum, lcAlbum, fname);
        result = MIN(result, backupResult);
    }
    if (name)  closedir(dirP);
    else  rewinddir(dirP);
Exit1:
    if (name)  dlp_VFSFileClose(sd, dirRef);
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
        char *rootDir = item->dir;
        VFSDirInfo dirInfos[MAX_DIR_ITEMS];

        // Open the remote root directory.
        FileRef dirRef;
        if (dlp_VFSFileOpen(sd, volRef, rootDir, vfsModeRead, &dirRef) < 0) {
            jp_logf(L_DEBUG, "%s:   Root '%s' does not exist on volume %d\n", MYNAME, rootDir, volRef);
            continue;
        }
        jp_logf(L_DEBUG, "%s:   Opened root '%s' on volume %d\n", MYNAME, rootDir, volRef);
        rootResult = 0;

        // Open the local root directory.
        char *lcRoot;
        if (!(lcRoot = localRoot(volRef)))
            goto Continue;
        DIR *dirP;
        if (!(dirP = opendir(lcRoot))) {
            jp_logf(L_DEBUG, "%s:   Root '%s' does not exist on '%s'\n", MYNAME, lcRoot + strlen(lcPath) + 1, lcPath);
            goto Continue;
        }
        jp_logf(L_DEBUG, "%s:   Opened local root '%s' on '%s'\n", MYNAME, lcRoot + strlen(lcPath) + 1, lcPath);

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
            jp_logf(L_DEBUG, "%s:   Enumerate root '%s', dirRef=%8lx, itr=%4lx, batch=%d, dirItems=%d\n", MYNAME, rootDir, dirRef, itr, batch, dirItems);
            PI_ERR enRes;
            if ((enRes = dlp_VFSDirEntryEnumerate(sd, dirRef, &itr, &batch, dirInfos + dirItems)) < 0) {
                // Crashes on empty directory (see: <https://github.com/juddmon/jpilot/issues/??>):
                // Further research is neccessary (see: <https://github.com/juddmon/jpilot/issues/41> ):
                // - Why in case of i.e. setting batch=4, itr == vfsIteratorStop, even if there are more than 4 files?
                // - For workaround and additional bug on SDCard volume, see at syncAlbum()
                jp_logf(L_FATAL, "%s:   Enumerate ERROR: enRes=%4d, dirRef=%8lx, itr=%4lx, batch=%d\n", MYNAME, enRes, dirRef, itr, batch);
                rootResult = -3;
                break;
            } else {
                jp_logf(L_DEBUG, "%s:   Enumerate OK: enRes=%4d, dirRef=%8lx, itr=%4lx, batch=%d\n", MYNAME, enRes, dirRef, itr, batch);
            }
            jp_logf(L_DEBUG, "%s:   Now search for remote albums on Volume %d in '%s' to sync ...\n", MYNAME, volRef, rootDir);
            for (int i = dirItems; i < (dirItems + batch); i++) {
                jp_logf(L_DEBUG, "%s:    Found remote album candidate '%s' in '%s'; attributes=%x\n", MYNAME,  dirInfos[i].name, rootDir, dirInfos[i].attr);
                // Treo 650 has #Thumbnail dir that is not an album
                if (dirInfos[i].attr & vfsFileAttrDirectory && (syncThumbnailDir || strcmp(dirInfos[i].name, "#Thumbnail"))) {
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
            jp_logf(L_DEBUG, "%s:    Found local album candidate '%s' in '%s'; type %d\n", MYNAME, entry->d_name, lcRoot + strlen(lcPath) + 1, entry->d_type);
            //~ // Intentionally remove a remote album for debugging purpose:
            //~ if (!strncasecmp(entry->d_name, "New_", 4)) { // Copy must exist locally, delete it after.
                //~ PI_ERR piErr;
                //~ char toDelete[NAME_MAX];
                //~ strcat(strcat(strcpy(toDelete ,rootDir), "/"), entry->d_name);
                //~ if ((piErr = dlp_VFSFileDelete(sd, volRef, toDelete))) {
                    //~ jp_logf(L_FATAL, "%s:     ERROR: %d; Not deleted remote album '%s'\n", MYNAME, piErr, toDelete);
                    //~ if (piErr == PI_ERR_DLP_PALMOS)
                        //~ jp_logf(L_FATAL, "%s:     ERROR: PalmOS error: %d\n", MYNAME, pi_palmos_error(sd));
                //~ } else
                    //~ jp_logf(L_WARN, "%s:     WARNING: Deleted remote album '%s' on volume %d\n", MYNAME, toDelete, volRef);
                //~ }
            struct stat fstat;
            int statErr;
            char lcAlbum[NAME_MAX];
            if ((statErr = stat(strcat(strcat(strcpy(lcAlbum, lcRoot), "/"), entry->d_name), &fstat))) {
                jp_logf(L_FATAL, "%s:    ERROR: %d; Could not read status of %s; No sync possible!\n", MYNAME, statErr, lcAlbum);
                result = MIN(result, -2);
                continue;
            }
            if (!S_ISDIR(fstat.st_mode) // use fstat to follow symlinks; (entry->d_type != DT_Dir) doesn't do this
                    || !strcmp(entry->d_name, ".")
                    || !strcmp(entry->d_name, "..")
                    || !(syncThumbnailDir || strcmp(entry->d_name, "#Thumbnail"))
                    || !cmpRemote(dirInfos, dirItems, entry->d_name)) {
                continue;
            }
            jp_logf(L_DEBUG, "%s:    Found real local album '%s' in '%s'\n", MYNAME, entry->d_name, lcRoot + strlen(lcPath) + 1);
            PI_ERR albumResult = syncAlbum(volRef, 0, rootDir, dirP, lcRoot, entry->d_name);
            result = MIN(result, albumResult);
        }

        closedir(dirP);
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
    jp_logf(L_DEBUG, "%s: dlp_VFSVolumeEnumerate piErr code %d, found %d volumes\n", MYNAME, piErr, *numVols);
    // On the Centro, Treo 650 and maybe more, it appears that the first non-hidden volRef is 2, and the hidden volRef is 1.
    // Let's poke around to see, if there is really a volRef 1 that's hidden from the dlp_VFSVolumeEnumerate().
    if (piErr < 0)  *numVols = 0; // On Error reset numVols
    for (int i=0; i<*numVols; i++) { // Search for volume 1
        jp_logf(L_DEBUG, "%s: *numVols=%d, volRefs[%d]=%d\n", MYNAME, *numVols, i, volRefs[i]);
        if (volRefs[i]==1)
            goto Exit; // No need to search for hidden volume
    }
    if (dlp_VFSVolumeInfo(sd, 1, &volInfo) >= 0 && volInfo.attributes & vfsVolAttrHidden) {
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
    jp_logf(L_DEBUG, "%s: volumeEnumerateIncludeHidden found %d volumes -> piErr=%d\n", MYNAME, *numVols, piErr);
    return piErr;
}
