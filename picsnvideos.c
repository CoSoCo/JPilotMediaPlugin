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

#define MYNAME "Pics&Videos"
#define PCDIR "Media"

#define L_DEBUG JP_LOG_DEBUG
#define L_INFO  JP_LOG_INFO // Unfortunately doesn't show up in GUI
#define L_WARN  JP_LOG_WARN
#define L_FATAL JP_LOG_FATAL
#define L_GUI   JP_LOG_GUI

typedef struct VFSInfo VFSInfo;
typedef struct VFSDirInfo VFSDirInfo;
typedef struct fileType {char ext[16]; struct fileType *next;} fileType;

static const char HELP_TEXT[] =
"JPilot plugin (c) 2008 by Dan Bodoh\n\
Contributor (2022): Ulf Zibis <Ulf.Zibis@CoSoCo.de>\n\
Version: "VERSION"\n\
\n\
Synchronizes media as pictures, videos and audios from the\n\
Pics&Videos storage in the Palm and from SDCard with\n\
folder '"PCDIR"' in your JPilot data directory,\n\
usually \"$JPILOT_HOME/.jpilot\".\n\
\n\
For more documentation, bug reports and new versions,\n\
see https://github.com/danbodoh/picsnvideos-jpilot";

static const unsigned MAX_VOLUMES = 16;
static const unsigned MIN_DIR_ITEMS = 2;
static const unsigned MAX_DIR_ITEMS = 1024;
static const char *ROOTDIRS[] = {"Photos & Videos", "Fotos & Videos", "DCIM"};
static const char *LOCALDIRS[] = {"Internal", "SDCard", "Card"};
static char lcPath[NAME_MAX];
static const char *PREFS_FILE = "picsnvideos.rc";
static prefType prefs[] = {
    {"syncThumbnailDir", INTTYPE, INTTYPE, 0, NULL, 0},
    // JPEG picture
    // video (GSM phones)
    // video (CDMA phones)
    // audio caption (GSM phones)
    // audio caption (CDMA phones)
    {"fileTypes", CHARTYPE, CHARTYPE, 0, ".jpg.3gp.3g2.amr.qcp" , 256},
    {"useDateModified", INTTYPE, INTTYPE, 0, NULL, 0},
    {"compareContent", INTTYPE, INTTYPE, 0, NULL, 0},
    {"doBackup", INTTYPE, INTTYPE, 1, NULL, 0},
    {"doRestore", INTTYPE, INTTYPE, 1, NULL, 0}
};
static const unsigned NUM_PREFS = sizeof(prefs)/sizeof(prefType);
static long syncThumbnailDir;
static char *fileTypes; // becomes freed by jp_free_prefs()
static long useDateModified;
static long compareContent;
static long doBackup;
static long doRestore;
static fileType *fileTypeList = NULL;
static pi_buffer_t *piBuf, *piBuf2;
static int importantWarning = 0;
static char syncLogEntry[128];

void *mallocLog(size_t);
int volumeEnumerateIncludeHidden(const int, int *, int *);
int syncVolume(const int, int);

void plugin_version(int *major_version, int *minor_version) {
    *major_version = 0;
    *minor_version = 99;
}

int plugin_get_name(char *name, int len) {
    //~ snprintf(name, len, "%s %d.%d", MYNAME, PLUGIN_MAJOR, PLUGIN_MINOR);
    snprintf(name, len, "%s %s", MYNAME, VERSION);
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
    if (jp_get_pref(prefs, 0, &syncThumbnailDir, NULL) < 0)
        jp_logf(L_WARN, "%s: WARNING: Could not read pref '%s' from prefs[]\n", MYNAME, prefs[0].name);
    if (jp_get_pref(prefs, 1, NULL, (const char **)&fileTypes) < 0)
        jp_logf(L_WARN, "%s: WARNING: Could not read pref '%s' from prefs[]\n", MYNAME, prefs[1].name);
    if (jp_get_pref(prefs, 2, &useDateModified, NULL) < 0)
        jp_logf(L_WARN, "%s: WARNING: Could not read pref '%s' from prefs[]\n", MYNAME, prefs[2].name);
    if (jp_get_pref(prefs, 3, &compareContent, NULL) < 0)
        jp_logf(L_WARN, "%s: WARNING: Could not read pref '%s' from prefs[]\n", MYNAME, prefs[3].name);
    if (jp_get_pref(prefs, 4, &doBackup, NULL) < 0)
        jp_logf(L_WARN, "%s: WARNING: Could not read pref '%s' from prefs[]\n", MYNAME, prefs[4].name);
    if (jp_get_pref(prefs, 5, &doRestore, NULL) < 0)
        jp_logf(L_WARN, "%s: WARNING: Could not read pref '%s' from prefs[]\n", MYNAME, prefs[5].name);
    if (jp_pref_write_rc_file(PREFS_FILE, prefs, NUM_PREFS) < 0) // To initialize with defaults, if pref file wasn't existent.
        jp_logf(L_WARN, "%s: WARNING: Could not write prefs[] to '%s'\n", MYNAME, PREFS_FILE);
    for (char *last; (last = strrchr(fileTypes, '.')) >= fileTypes; *last = 0) {
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
    }
    jp_free_prefs(prefs, NUM_PREFS);
    if (!result && (result = !(piBuf = pi_buffer_new(65536)) || !(piBuf2 = pi_buffer_new(65536))))
    //~ if (!result && (result = !(piBuf = pi_buffer_new(32768)) || !(piBuf2 = pi_buffer_new(32768))))
        jp_logf(L_FATAL, "%s: ERROR: Out of memory\n", MYNAME);
    return result;
}

int plugin_sync(int sd) {
    int volRefs[MAX_VOLUMES];
    int volumes = MAX_VOLUMES;

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
        PI_ERR volResult;
        if ((volResult = syncVolume(sd, volRefs[i])) < -2) {
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
        jp_logf(L_WARN, "\n%s: IMPORTANT WARNING: Now open once the Media App on your Palm device to avoid crash (signal SIGCHLD) on next HotSync !!!\n\n", MYNAME);
        dlp_AddSyncLogEntry (sd, MYNAME": IMPORTANT WARNING: Now open once the Media App to avoid crash with JPilot on next HotSync !!!\n");
        // Avoids bug <https://github.com/desrod/pilot-link/issues/10>, as then the file "Album.db" is created, so the dir is not empty anymore.
    }

    return result;
}

int plugin_exit_cleanup(void) {
    for (fileType *tmp; (tmp = fileTypeList);) {
        fileTypeList = fileTypeList->next;
        free(tmp);
    }
    pi_buffer_free(piBuf);
    pi_buffer_free(piBuf2);
    return EXIT_SUCCESS;
}

// ToDo: Rename picsnvideos ./. media

void *mallocLog(size_t size) {
    void *p;
    if (!(p = malloc(size)))
        jp_logf(L_FATAL, "%s: ERROR: Out of memory\n", MYNAME);
    return p;
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
char *localRoot(const int sd, const unsigned volRef) {
    char *path = mallocLog(NAME_MAX);
    VFSInfo volInfo;

    if (!path)  return path;
    if (createDir(strcpy(path, lcPath), NULL))  goto Exit;
    // Get indicator of which card.
    if (dlp_VFSVolumeInfo(sd, volRef, &volInfo) < 0) {
        jp_logf(L_FATAL, "%s:     ERROR: Could not get volume info from volRef %d\n", MYNAME, volRef);
        goto Exit;
    }
    if (volInfo.mediaType == pi_mktag('T', 'F', 'F', 'S')) {
        if (createDir(path, LOCALDIRS[0]))  goto Exit;
    } else if (volInfo.mediaType == pi_mktag('s', 'd', 'i', 'g')) {
        if (createDir(path, LOCALDIRS[1]))  goto Exit;
    } else {
        sprintf(path+strlen(path), "/%s%d", LOCALDIRS[2], volInfo.slotRefNum);
        if (createDir(path, NULL))  goto Exit;
    }
    return path; // must be free'd by caller
Exit:
    free(path);
    return NULL;
}

// ToDo: Replace filesize by todo or sizeleft
int fileRead(const int sd, FileRef fileRef, FILE *stream, pi_buffer_t *buf, int filesize) {
    pi_buffer_clear(buf);
    for (int readsize = -1, todo = filesize > buf->allocated ? buf->allocated : filesize; todo > 0; todo -= readsize) {
        if (fileRef) {
            readsize = dlp_VFSFileRead(sd, fileRef, buf, todo);
            //readsize = dlp_VFSFileRead(sd, fileRef, buf, buf->allocated); // works too, but is very slow
        } else if (stream) {
            readsize = fread(buf->data + buf->used, 1, todo, stream);
            buf->used += readsize;
        }
        if (readsize < 0) {
            jp_logf(L_FATAL, "\n%s:       ERROR: File read error: %d; aborting at %d bytes left.\n", MYNAME, readsize, filesize - buf->used);
            if (readsize == PI_ERR_DLP_PALMOS)
                jp_logf(L_FATAL, "%s:       ERROR: PalmOS error: %d.\n", MYNAME, pi_palmos_error(sd));
            return readsize;
        }
    }
    return (int)buf->used;
}

// ToDo: Replace filesize by todo or sizeleft
int fileWrite(const int sd, FileRef fileRef, FILE *stream, pi_buffer_t *buf, int filesize) {
    for (int writesize = 0, offset = 0; offset < buf->used; offset += writesize) {
        if (fileRef) {
            writesize = dlp_VFSFileWrite(sd, fileRef, buf->data + offset, buf->used - offset);
        } else if (stream) {
            writesize = fwrite(buf->data + offset, 1, buf->used - offset, stream);
        }
        if (writesize < 0) {
            jp_logf(L_FATAL, "\n%s:       ERROR: File write error: %d; aborting at %d bytes left.\n", MYNAME, writesize, filesize - buf->used - offset);
            if (writesize == PI_ERR_DLP_PALMOS)
                jp_logf(L_FATAL, "%s:       ERROR: PalmOS error: %d.\n", MYNAME, pi_palmos_error(sd));
            return writesize;
        }
    }
    return (int)buf->used;
}

int fileCompare(const int sd, FileRef fileRef, FILE *stream, int filesize) {
    int result;
    for (int todo = filesize; todo > 0; todo -= piBuf->used) {
        if (fileRead(sd, fileRef, NULL, piBuf, todo) < 0 || fileRead(0, 0, stream, piBuf2, todo) < 0 || piBuf->used != piBuf2->used) {
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
int backupFileIfNeeded(const int sd, const unsigned volRef, const char *rmDir, const char *lcDir, const char *file) {
    char rmPath[strlen(rmDir) + strlen(file) + 2];
    char lcPath[strlen(lcDir) + strlen(file) + 4]; // prepare for possible rename
    FileRef fileRef;
    int filesize; // also serves as error return code

    strcat(strcat(strcpy(rmPath, rmDir), "/"), file);
    strcat(strcat(strcpy(lcPath, lcDir), "/"), file);

    if (dlp_VFSFileOpen(sd, volRef, rmPath, vfsModeRead, &fileRef) < 0) {
        jp_logf(L_FATAL, "%s:       ERROR: Could not open remote file '%s' on volume %d for reading.\n", MYNAME, rmPath, volRef);
        return -1;
    }
    if (dlp_VFSFileSize(sd, fileRef, (int *)(&filesize)) < 0) {
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
            FILE *lcStream;
            if (!(lcStream = fopen(lcPath, "r"))) {
                jp_logf(L_WARN, "%s:       WARNING: Cannot open %s for comparing %d bytes, so may have different content,\n", MYNAME, lcPath, filesize);
            } else {
                if (!(equal = !fileCompare(sd, fileRef, lcStream, filesize)))
                    jp_logf(L_WARN, "%s:       WARNING: File '%s' already exists, but has different content,\n", MYNAME, lcPath);
                fclose(lcStream);
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
        jp_logf(L_WARN, "%s:               so backup '%s' to '%s'.\n", MYNAME, file, lcPath);
    }
    // File has not already been synced, backup it.
    FILE *lcStream;
    if (!(lcStream = fopen(lcPath, "wx"))) {
        jp_logf(L_FATAL, "%s:       ERROR: Cannot open %s for writing %d bytes!\n", MYNAME, lcPath, filesize);
        filesize = -1; // remember error
        goto Exit;
    }
    // Copy file.
    jp_logf(L_GUI, "%s:      Backing up %s ...", MYNAME, lcPath);
    for (int readsize; filesize > 0; filesize -= piBuf->used) {
        pi_buffer_clear(piBuf);
        // ToDo: Use fileRead() and fileWrite()
        if ((readsize = dlp_VFSFileRead(sd, fileRef, piBuf, (filesize > piBuf->allocated ? piBuf->allocated : filesize))) < 0)  {
        //if (dlp_VFSFileRead(sd, fileRef, piBuf, piBuf->allocated) < 0)  { // works too, but is very slow
            jp_logf(L_FATAL, "\n%s:       ERROR: File read error; aborting at %d bytes left.\n", MYNAME, filesize);
            if (readsize == PI_ERR_DLP_PALMOS)
                jp_logf(L_FATAL, "%s:       ERROR: PalmOS error: %d.\n", MYNAME, pi_palmos_error(sd));
            filesize = readsize; // remember error
            break;
        }
        for (int writesize, offset = 0; offset < piBuf->used; offset += writesize) {
            if ((writesize = fwrite(piBuf->data + offset, 1, piBuf->used - offset, lcStream)) < 0) {
                jp_logf(L_FATAL, "\n%s:       ERROR: File write error; aborting at %d bytes left.\n", MYNAME, filesize - offset);
                filesize = writesize; // remember error; breaks the outer loop
                break;
            }
        }
    }
    fclose(lcStream);
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
int restoreFile(const int sd, const unsigned volRef, const char *lcDir, const char *rmDir, const char *file) {
    char lcPath[strlen(lcDir) + strlen(file) + 2];
    char rmPath[strlen(rmDir) + strlen(file) + 2];
    FILE *stream;
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
    if (!(stream = fopen(lcPath, "r"))) {
        jp_logf(L_FATAL, "%s:       ERROR: Could not open %s for reading %d bytes,\n", MYNAME, lcPath, filesize);
        return -1;
    }
    if ((piErr = dlp_VFSFileOpen(sd, volRef, rmPath, vfsModeReadWrite | vfsModeCreate, &fileRef)) < 0) { // May not work on DLP, then first create file with:
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
    jp_logf(L_GUI, "%s:      Restoring '%s' ...", MYNAME, rmPath);
    for (; filesize > 0; filesize -= piBuf->used) {
        if (fileRead(0, 0, stream, piBuf, filesize) < 0) {
            filesize = -1; // remember error
            break;
        }
        if (fileWrite(sd, fileRef, NULL, piBuf, filesize) < 0) {
            filesize = -1; // remember error
            break;
        }
        //~ jp_logf(L_DEBUG, "\n%s:       filesize=%d, piBuf->used=%d", MYNAME, filesize, piBuf->used);
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
        if ((piErr = dlp_VFSFileSetDate(sd, fileRef, vfsFileDateModified, date)) < 0 ||
                (piErr = dlp_VFSFileSetDate(sd, fileRef, vfsFileDateCreated, date)) < 0) {
            jp_logf(L_WARN, "%s:      WARNING: %d Could not set date of remote file '%s' on volume %d\n", MYNAME, piErr, rmPath, volRef);
            if (piErr == PI_ERR_DLP_PALMOS)
                jp_logf(L_FATAL, "%s:      ERROR: PalmOS error: %d.\n", MYNAME, pi_palmos_error(sd));
        }
    }
    dlp_VFSFileClose(sd, fileRef);

Exit:
    fclose(stream);
    jp_logf(L_DEBUG, "%s:       Restore file size / copy result: %d, statErr=%d\n", MYNAME, filesize, statErr);
    return filesize;
}

int casecmpFileTypeList(const char *fname) {
    char *ext = strrchr(fname, '.');
    int result = 1;
    for (fileType *tmp = fileTypeList; ext && tmp; tmp = tmp->next) {
        if (!(result = strcasecmp(ext, tmp->ext)))  break;
    }
    return result;
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
PI_ERR syncAlbum(const int sd, const unsigned volRef, FileRef dirRef, const char *rmRoot, DIR *lcDirP, const char *lcRoot, const char *name) {
    char rmTmp[name ? strlen(rmRoot) + strlen(name) + 2 : 0], *rmAlbumDir;
    char lcTmp[name ? strlen(lcRoot) + strlen(name) + 2 : 0], *lcAlbumDir;
    int dirItems_init = MIN_DIR_ITEMS;
    VFSDirInfo dirInfos[MAX_DIR_ITEMS];
    struct stat fstat;
    int statErr;
    PI_ERR result = 0, piOSErr;

    if (name) {
        rmAlbumDir = strcat(strcat(strcpy(rmTmp ,rmRoot), "/"), name);
        if (lcDirP) { // indicates, that we are in restore-only mode, so need to create a new remote album dir.
            jp_logf(L_DEBUG, "%s:    Try to create dir '%s' on volume %d\n", MYNAME, rmAlbumDir, volRef);
            if ((result = dlp_VFSDirCreate(sd, volRef, rmAlbumDir)) < 0) {
                if (result == PI_ERR_DLP_PALMOS && (piOSErr = pi_palmos_error(sd)) != 10758) { // File already exists.
                    jp_logf(L_FATAL, "%s:    ERROR %d: Could not create dir '%s' on volume %d\n", MYNAME, result, rmAlbumDir, volRef);
                    jp_logf(L_FATAL, "%s:    ERROR: PalmOS error: %d.\n", MYNAME, piOSErr);
                    return -2;
                }
            }
            importantWarning = 1;
            dirItems_init = 0; // prevent search on remote album
        }
        jp_logf(L_DEBUG, "%s:    Try to open dir '%s' on volume %d\n", MYNAME, rmAlbumDir, volRef);
        if (dlp_VFSFileOpen(sd, volRef, rmAlbumDir, vfsModeReadWrite, &dirRef) < 0) { // mode "Write" for setting date later
            jp_logf(L_FATAL, "%s:    ERROR: Could not open dir '%s' on volume %d\n", MYNAME, rmAlbumDir, volRef);
            return -2;
        }
        lcAlbumDir = strcpy(lcTmp, lcRoot);
        jp_logf(L_DEBUG, "%s:    Try to create dir '%s' in '%s'\n", MYNAME, name, lcRoot);
        if (createDir(lcAlbumDir, name)) {
            result = -2;
            goto Exit1;
        }
        if (lcDirP) { // indicates, that we are in restore-only mode, so need to create a new remote album dir.
            if ((statErr = stat(lcAlbumDir, &fstat))) {
                jp_logf(L_FATAL, "%s:    ERROR: %d; Could not read status of %s; No sync possible!\n", MYNAME, statErr, lcAlbumDir);
                result = -2;
                goto Exit1;
            }
            time_t date = fstat.st_mtime;
            // Set the date that the picture was created (not the file), aka modified time.
            if ((result = dlp_VFSFileSetDate(sd, dirRef, vfsFileDateModified, date)) < 0 ||
                    (result = dlp_VFSFileSetDate(sd, dirRef, vfsFileDateCreated, date)) < 0) {
                jp_logf(L_WARN, "%s:    WARNING: %d; Could not set date of remote file '%s' on volume %d\n", MYNAME, result, rmAlbumDir, volRef);
                if (result == PI_ERR_DLP_PALMOS)
                    jp_logf(L_FATAL, "%s:    ERROR: PalmOS error: %d.\n", MYNAME, pi_palmos_error(sd));
            }
        }
        jp_logf(L_DEBUG, "%s:    Try to open dir '%s' in '%s'\n", MYNAME, name, lcRoot);
        if (!(lcDirP = opendir(lcAlbumDir))) {
            jp_logf(L_FATAL, "%s:    ERROR: Could not open dir '%s' on '%s'\n", MYNAME, name, lcRoot);
            result = -2;
            goto Exit1;
        }
    } else {
        rmAlbumDir = (char *)rmRoot;
        lcAlbumDir = (char *)lcRoot;
    }
    jp_logf(L_GUI, "%s:    Sync album '%s' in '%s' on volume %d ...\n", MYNAME, name ? name : ".", rmRoot, volRef);
    // Iterate over all the files in the remote album dir.
    int dirItems;
    //~ enum dlpVFSFileIteratorConstants itr = vfsIteratorStart; // doesn't work because of type mismatch bug <https://github.com/juddmon/jpilot/issues/39>
    //~ while (itr != (unsigned long)vfsIteratorStop) { // doesn't work because of bug <https://github.com/juddmon/jpilot/issues/39>
    //~ while (itr != (unsigned)vfsIteratorStop) { // doesn't work because of bug <https://github.com/juddmon/jpilot/issues/41>
    unsigned long itr = (unsigned long)vfsIteratorStart;
    int loops = 16; // for debugging
    for (; (dirItems = dirItems_init) <= MAX_DIR_ITEMS && dirItems > 0; dirItems_init *= 2) { // WORKAROUND
        if (--loops < 0)  break; // for debugging
        itr = (unsigned long)vfsIteratorStart; // workaround, reset itr if it wrongly was -1 or 1888
        jp_logf(L_DEBUG, "%s:     Enumerate album '%s', dirRef=%8lx, itr=%4lx, dirItems=%d\n", MYNAME, rmAlbumDir, dirRef, itr, dirItems);
        if ((result = dlp_VFSDirEntryEnumerate(sd, dirRef, &itr, &dirItems, dirInfos)) < 0) {
            // Crashes on empty directory (see: <https://github.com/desrod/pilot-link/issues/11>):
            // Further research is neccessary (see: <https://github.com/juddmon/jpilot/issues/41>):
            // - Why in case of i.e. setting dirItems=4, itr != 0, even if there are more than 4 files?
            // - Why then on SDCard itr == 1888 in the first loop, so out of allowed range?
            jp_logf(L_FATAL, "%s:     Enumerate ERROR: result=%4d, dirRef=%8lx, itr=%4lx, dirItems=%d\n", MYNAME, result, dirRef, itr, dirItems);
            goto Exit2;
        }
        jp_logf(L_DEBUG, "%s:     Enumerate OK: result=%4d, dirRef=%8lx, itr=%4lx, dirItems=%d\n", MYNAME, result, dirRef, itr, dirItems);
        for (int i= dirItems_init==MIN_DIR_ITEMS ? 0 : dirItems_init/2; i<dirItems; i++) {
            jp_logf(L_DEBUG, "%s:      dirItem %3d: '%s' attributes %x\n", MYNAME, i, dirInfos[i].name, dirInfos[i].attr);
        }
        if (dirItems < dirItems_init) {
            break;
        }
    }
    jp_logf(L_DEBUG, "%s:     Now first search of local files, which to restore ...\n", MYNAME);
    // First iterate over all the local files in the album dir, to prevent from back-storing renamed files,
    // so only looking for remotely unknown files ... and then restore them.
    for (struct dirent *entry; doRestore && (entry = readdir(lcDirP));) {
        jp_logf(L_DEBUG, "%s:      Found local file: '%s' type=%d\n", MYNAME, entry->d_name, entry->d_type);
        // Intentionally remove the remote file for debugging purpose:
        //~ if (!strncasecmp(entry->d_name, "New_", 4)) { // Copy must exist locally.
        //~ if (!strncasecmp(entry->d_name, "Thumb_", 6)) { // Copy must exist locally.
            //~ PI_ERR piErr;
            //~ char toDelete[NAME_MAX];
            //~ strcat(strcat(strcpy(toDelete ,rmAlbumDir), "/"), entry->d_name);
            //~ if ((piErr = dlp_VFSFileDelete(sd, volRef, toDelete))) {
                //~ jp_logf(L_FATAL, "%s:       ERROR: %d; Not deleted remote file '%s'\n", MYNAME, piErr, toDelete);
                //~ if (piErr == PI_ERR_DLP_PALMOS)
                    //~ jp_logf(L_FATAL, "%s:       ERROR: PalmOS error: %d\n", MYNAME, pi_palmos_error(sd));
            //~ } else
                //~ jp_logf(L_WARN, "%s:       WARNING: Deleted remote file '%s' on volume %d\n", MYNAME, toDelete, volRef);
            //~ }
        char lcAlbumPath[NAME_MAX];
        if ((statErr = stat(strcat(strcat(strcpy(lcAlbumPath, lcAlbumDir), "/"), entry->d_name), &fstat))) {
            jp_logf(L_FATAL, "%s:      ERROR: %d; Could not read status of %s; No sync possible!\n", MYNAME, statErr, lcAlbumPath);
            result = MIN(result, -1);
            continue;
        }
        if (!S_ISREG(fstat.st_mode) // use fstat to follow symlinks; (entry->d_type != DT_REG) doesn't do this
                || strlen(entry->d_name) < 2
                || casecmpFileTypeList(entry->d_name)
                || !cmpRemote(dirInfos, dirItems, entry->d_name)) {
            continue;
        }
        //~ jp_logf(L_DEBUG, "%s:      Restore local file: '%s' to '%s'\n", MYNAME, entry->d_name, rmAlbumDir);
        int restoreResult = restoreFile(sd, volRef, lcAlbumDir, rmAlbumDir, entry->d_name);
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
                || casecmpFileTypeList(fname)) {
            continue;
        }
        //~ jp_logf(L_DEBUG, "%s:      Backup remote file: '%s' to '%s'\n", MYNAME, fname, lcAlbumDir);
        int backupResult = backupFileIfNeeded(sd, volRef, rmAlbumDir, lcAlbumDir, fname);
        result = MIN(result, backupResult);
    }
Exit2:
    if (name)  closedir(lcDirP);
    else  rewinddir(lcDirP);
Exit1:
    if (name)  dlp_VFSFileClose(sd, dirRef);
    jp_logf(L_DEBUG, "%s:    Album '%s' done -> result=%d\n", MYNAME,  rmAlbumDir, result);
    return result;
}

/*
 *  Backup all albums from volume volRef.
 */
PI_ERR syncVolume(const int sd, int volRef) {
    PI_ERR rootResult = -3, result = 0;

    jp_logf(L_DEBUG, "%s:  Searching roots on volume %d\n", MYNAME, volRef);
    for (int d = 0; d < sizeof(ROOTDIRS)/sizeof(*ROOTDIRS); d++) {
        VFSDirInfo dirInfos[MAX_DIR_ITEMS];

        // Open the remote root directory.
        FileRef dirRef;
        if (dlp_VFSFileOpen(sd, volRef, ROOTDIRS[d], vfsModeRead, &dirRef) < 0) {
            jp_logf(L_DEBUG, "%s:   Root '%s' does not exist on volume %d\n", MYNAME, ROOTDIRS[d], volRef);
            continue;
        }
        jp_logf(L_DEBUG, "%s:   Opened root '%s' on volume %d\n", MYNAME, ROOTDIRS[d], volRef);
        rootResult = 0;

        // Open the local root directory.
        char *lcRoot;
        if (!(lcRoot = localRoot(sd, volRef)))
            goto Continue1;
        DIR *lcRootP;
        if (!(lcRootP = opendir(lcRoot))) {
            jp_logf(L_DEBUG, "%s:   Root '%s' does not exist on '%s'\n", MYNAME, lcRoot + strlen(lcPath) + 1, lcPath);
            goto Continue2;
        }
        jp_logf(L_DEBUG, "%s:   Opened local root '%s' on '%s'\n", MYNAME, lcRoot + strlen(lcPath) + 1, lcPath);

        // Fetch the unfiled album, which is simply the root dir, and sync it.
        // Apparently the Treo 650 can store media in the root dir, as well as in album dirs.
        result = syncAlbum(sd, volRef, dirRef, ROOTDIRS[d], lcRootP, lcRoot, NULL);
        // ToDo: Print log if Error !

        // Iterate through the remote root directory, looking for things that might be albums.
        int dirItems = 0;
        //~ enum dlpVFSFileIteratorConstants itr = vfsIteratorStart;
        //~ while (itr != vfsIteratorStop) { // doesn't work because of type mismatch bug <https://github.com/juddmon/jpilot/issues/39>
        unsigned long itr = (unsigned long)vfsIteratorStart;
        for (int batch; (enum dlpVFSFileIteratorConstants)itr != vfsIteratorStop; dirItems += batch) {
            batch = MIN(512, MAX_DIR_ITEMS - dirItems);
            jp_logf(L_DEBUG, "%s:   Enumerate root '%s', dirRef=%8lx, itr=%4lx, batch=%d, dirItems=%d\n", MYNAME, ROOTDIRS[d], dirRef, itr, batch, dirItems);
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
            jp_logf(L_DEBUG, "%s:   Now search for remote albums on Volume %d in '%s' to sync ...\n", MYNAME, volRef, ROOTDIRS[d]);
            for (int i = dirItems; i < (dirItems + batch); i++) {
                jp_logf(L_DEBUG, "%s:    Found remote album candidate '%s' in '%s'; attributes=%x\n", MYNAME,  dirInfos[i].name, ROOTDIRS[d], dirInfos[i].attr);
                // Treo 650 has #Thumbnail dir that is not an album
                if (dirInfos[i].attr & vfsFileAttrDirectory && (syncThumbnailDir || strcmp(dirInfos[i].name, "#Thumbnail"))) {
                    jp_logf(L_DEBUG, "%s:    Found real remote album '%s' in '%s'\n", MYNAME, dirInfos[i].name, ROOTDIRS[d]);
                    PI_ERR albumResult = syncAlbum(sd, volRef, 0, ROOTDIRS[d], NULL, lcRoot, dirInfos[i].name);
                    result = MIN(result, albumResult);
                    // ToDo: Print log if Error !
                }
            }
        }

        // Now iterate over all the local files in the album dir. To prevent from back-storing renamed files,
        // only looking for remotely unknown albums ... and then restore them.
        jp_logf(L_DEBUG, "%s:   Now search for local albums in '%s' to restore ...\n", MYNAME, lcRoot);
        for (struct dirent *entry; (entry = readdir(lcRootP));) {
            jp_logf(L_DEBUG, "%s:    Found local album candidate '%s' in '%s'; type %d\n", MYNAME, entry->d_name, lcRoot + strlen(lcPath) + 1, entry->d_type);
            //~ // Intentionally remove a remote album for debugging purpose:
            //~ if (!strncasecmp(entry->d_name, "New_", 4)) { // Copy must exist locally.
                //~ PI_ERR piErr;
                //~ char toDelete[NAME_MAX];
                //~ strcat(strcat(strcpy(toDelete ,ROOTDIRS[d]), "/"), entry->d_name);
                //~ if ((piErr = dlp_VFSFileDelete(sd, volRef, toDelete))) {
                    //~ jp_logf(L_FATAL, "%s:     ERROR: %d; Not deleted remote album '%s'\n", MYNAME, piErr, toDelete);
                    //~ if (piErr == PI_ERR_DLP_PALMOS)
                        //~ jp_logf(L_FATAL, "%s:     ERROR: PalmOS error: %d\n", MYNAME, pi_palmos_error(sd));
                //~ } else
                    //~ jp_logf(L_WARN, "%s:     WARNING: Deleted remote album '%s' on volume %d\n", MYNAME, toDelete, volRef);
                //~ }
            struct stat fstat;
            int statErr;
            char lcAlbumDir[NAME_MAX];
            if ((statErr = stat(strcat(strcat(strcpy(lcAlbumDir, lcRoot), "/"), entry->d_name), &fstat))) {
                jp_logf(L_FATAL, "%s:    ERROR: %d; Could not read status of %s; No sync possible!\n", MYNAME, statErr, lcAlbumDir);
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
            PI_ERR albumResult = syncAlbum(sd, volRef, 0, ROOTDIRS[d], lcRootP, lcRoot, entry->d_name);
            result = MIN(result, albumResult);
            // ToDo: Print log if Error !
        }

        closedir(lcRootP);
Continue2:
        free(lcRoot);
Continue1:
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
    // On the Centro, Treo 650 and maybe more, it appears that the
    // first non-hidden volRef is 2, and the hidden volRef is 1.
    // Let's poke around to see, if there is really a volRef 1
    // that's hidden from the dlp_VFSVolumeEnumerate().
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
