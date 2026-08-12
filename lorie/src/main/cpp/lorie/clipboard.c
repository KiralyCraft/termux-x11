/* Copyright 2016-2019 Pierre Ossman for Cendio AB
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#pragma clang diagnostic ignored "-Wunknown-pragmas"

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <android/log.h>
#include <X11/Xatom.h>
#include <windowstr.h>
#include <selection.h>
#include <propertyst.h>
#include <xacestr.h>

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>

#include "lorie.h"

/* utility functions for text conversion */

static inline void lorieConvertLF(const char* src, char *dst, size_t bytes) {
    size_t i = 0, j = 0;
    for (; i < bytes; i++)
        if (src[i] != '\r')
            dst[j++] = src[i];
}

static inline void lorieLatin1ToUTF8(unsigned char* out, const unsigned char* in) {
    while (*in)
        if (*in < 128)
            *out++ = *in++;
        else
            *out++ = 0xc2 + (*in > 0xbf), *out++ = (*in++ & 0x3f) + 0x80;
}

static inline int lorieCheckUTF8(const unsigned char *utf, size_t size) {
    int ix;
    unsigned char c;

    for (ix = 0; (c = utf[ix]) && ix < size;) {
        if (c & 0x80) {
            if ((utf[ix + 1] & 0xc0) != 0x80)
                return 0;
            if ((c & 0xe0) == 0xe0) {
                if ((utf[ix + 2] & 0xc0) != 0x80)
                    return 0;
                if ((c & 0xf0) == 0xf0) {
                    if ((c & 0xf8) != 0xf0 || (utf[ix + 3] & 0xc0) != 0x80)
                        return 0;
                    ix += 4;
                    /* 4-byte code */
                } else
                    /* 3-byte code */
                    ix += 3;
            } else
                /* 2-byte code */
                ix += 2;
        } else
            /* 1-byte code */
            ix++;
    }
    return 1;
}

static size_t lorieUtf8ToUCS4(const char* src, size_t max, unsigned* dst) {
    size_t count, consumed;

    *dst = 0xfffd;

    if (max == 0)
        return 0;

    consumed = 1;

    if ((*src & 0x80) == 0) {
        *dst = *src;
        count = 0;
    } else if ((*src & 0xe0) == 0xc0) {
        *dst = *src & 0x1f;
        count = 1;
    } else if ((*src & 0xf0) == 0xe0) {
        *dst = *src & 0x0f;
        count = 2;
    } else if ((*src & 0xf8) == 0xf0) {
        *dst = *src & 0x07;
        count = 3;
    } else {
        // Invalid sequence, consume all continuation characters
        src++;
        max--;
        while ((max-- > 0) && ((*src++ & 0xc0) == 0x80))
            consumed++;
        return consumed;
    }

    src++;
    max--;

    while (count--) {
        consumed++;

        // Invalid or truncated sequence?
        if ((max == 0) || ((*src & 0xc0) != 0x80)) {
            *dst = 0xfffd;
            return consumed;
        }

        *dst <<= 6;
        *dst |= *src & 0x3f;

        src++;
        max--;
    }

    // UTF-16 surrogate code point?
    if ((*dst >= 0xd800) && (*dst < 0xe000))
        *dst = 0xfffd;

    return consumed;
}

static const char *lorieUtf8ToLatin1(const char *src) {
    size_t sz;

    const char* in;
    size_t in_len;

    // Compute output size
    sz = 0;
    in = src;
    in_len = -1;
    while ((in_len > 0) && (*in != '\0')) {
        size_t len;
        unsigned ucs;

        len = lorieUtf8ToUCS4(in, in_len, &ucs);
        in += len;
        in_len -= len;
        sz++;
    }

    // Reserve space
    unsigned char out[sz + 1];
    memset(out, 0, sz + 1);
    size_t position = 0;

    // And convert
    in = src;
    in_len = 4.294967295E9;
    while ((in_len > 0) && (*in != '\0')) {
        size_t len;
        unsigned ucs;

        len = lorieUtf8ToUCS4(in, in_len, &ucs);
        in += len;
        in_len -= len;

        if (ucs > 0xff)
            out[position++] = '?';
        else
            out[position++] = (unsigned char)ucs;
    }

    return strdup((const char*) out);
}

/* end utility functions */

#define log(prio, ...) __android_log_print(ANDROID_LOG_ ## prio, "LorieNative", __VA_ARGS__)
extern ScreenPtr pScreenPtr;

static Atom xaTIMESTAMP = 0, xaTEXT = 0, xaCLIPBOARD = 0, xaTARGETS = 0, xaSTRING = 0, xaUTF8_STRING = 0;
static Atom xaINCR = 0, xaMULTIPLE = 0;
static Atom xaTEXT_URI_LIST = 0, xaGNOME_COPIED_FILES = 0;
static Bool clipboardEnabled = FALSE;

// The target most recently asked for via lorieSelectionRequest(). lorieHandleSelection() only
// acts on a PropertyNotify for this exact target, so whatever mime the real owner answers with
// (text/html, application/octet-stream, ...) flows through -- not just the handful of targets we
// happen to special-case by name.
static Atom pendingRequestTarget = 0;
static LorieClipboardData* cachedClip = NULL;

// Multi-item (file list) clip, materialized under a per-generation staging directory on disk
// (see lorieMaterializeClipFile) so real X11 clients can open() the files by path. cachedClipList
// and the staging directory named by clipGeneration always share the same lifetime.
static LorieClipboardList* cachedClipList = NULL;
static int clipGeneration = -1;

// Real, absolute paths of the files most recently offered to Android via lorieHandleUriList (see
// lorieReopenSentClipItem) -- Android can't safely reopen these itself a second time (dup()'d fds
// share file position; /proc/self/fd reopen fails, wrong uid), so it asks the X server, which has
// genuine filesystem access, to open() the real path fresh on each read. Guarded by sentClipLock
// since it's written from the X server's main thread (lorieHandleUriList) and read from the
// activity-socket input thread (lorieReopenSentClipItem).
static pthread_mutex_t sentClipLock = PTHREAD_MUTEX_INITIALIZER;
static int32_t sentClipGeneration = -1;
static char** sentClipPaths = NULL;
static size_t sentClipCount = 0;

static const char* lorieClipStagingDirFor(int generation);
static void lorieRemoveStagingDir(int generation);
static Bool lorieMaterializeClipFile(int generation, const char* name, int srcFd, char* outName, size_t outNameSize);
static char* lorieAppendUriEncoded(char* out, const char* s);
static void lorieFreeSentClipPathsLocked(void);

// State of an in-progress ICCCM INCR transfer (used when a property is too big to fit a
// single ChangeProperty/GetProperty round trip, e.g. most clipboard images).
static Bool incrActive = FALSE;
static Atom incrTarget = None;
static void* incrBuf = NULL;
static size_t incrLen = 0;

static void lorieFreeCachedClip(void) {
    free(cachedClip);
    cachedClip = NULL;

    if (cachedClipList) {
        lorieRemoveStagingDir(clipGeneration);
        free(cachedClipList);
        cachedClipList = NULL;
    }
}

struct LorieDataTarget {
    ClientPtr client;
    Atom selection;
    Atom target;
    Atom property;
    Window requestor;
    CARD32 time;
    struct LorieDataTarget* next;
} *lorieDataTargetHead;

void lorieEnableClipboardSync(Bool enable) {
    clipboardEnabled = enable;
}

/* functions related to clipboard receiving */

static void lorieSelectionRequest(Atom selection, Atom target) {
    Selection *pSel;

    if (clipboardEnabled && dixLookupSelection(&pSel, selection, serverClient, DixGetAttrAccess) == Success) {
        pendingRequestTarget = target;
        xEvent event = {0};
        event.u.u.type = SelectionRequest;
        event.u.selectionRequest.owner = pSel->window;
        event.u.selectionRequest.time = currentTime.milliseconds;
        event.u.selectionRequest.requestor = pScreenPtr->root->drawable.id;
        event.u.selectionRequest.selection = selection;
        event.u.selectionRequest.target = target;
        event.u.selectionRequest.property = target;
        WriteEventsToClient(pSel->client, 1, &event);
    }
}

static Bool lorieHasAtom(Atom atom, const Atom list[], size_t size) {
    for (size_t i = 0; i < size; i++)
        if (list[i] == atom)
            return TRUE;

    return FALSE;
}

// Decodes one text/uri-list line (RFC 2483) into a plain filesystem path if it is a "file://" URI,
// percent-decoding as it goes. Returns NULL for anything else (other schemes, comment lines).
// Caller frees the result.
static char* lorieDecodeFileUri(const char* line, size_t len) {
    static const char prefix[] = "file://";
    const size_t prefixLen = sizeof(prefix) - 1;
    const char *p, *end;
    char *decoded, *out;

    if (len < prefixLen || strncmp(line, prefix, prefixLen) != 0)
        return NULL;

    p = line + prefixLen;
    end = line + len;
    // "file://host/path" -> keep only the path, starting at the next '/'; "file:///path" (no
    // host) already starts with '/' right after the prefix.
    if (p < end && *p != '/') {
        const char* slash = memchr(p, '/', end - p);
        if (!slash)
            return NULL;
        p = slash;
    }

    decoded = malloc((size_t) (end - p) + 1);
    if (!decoded)
        return NULL;

    for (out = decoded; p < end;) {
        if (*p == '%' && p + 2 < end && isxdigit((unsigned char) p[1]) && isxdigit((unsigned char) p[2])) {
            char hex[3] = { p[1], p[2], 0 };
            *out++ = (char) strtol(hex, NULL, 16);
            p += 3;
        } else {
            *out++ = *p++;
        }
    }
    *out = '\0';
    return decoded;
}

// The eventual Android-side ClipData carries one Uri (plus a mime string) per item through a
// single binder transaction (~1MB total, regardless of the files' actual byte sizes -- file
// contents themselves never cross binder). Past a few thousand items that marshaling overhead
// alone risks a TransactionTooLargeException, so a huge selection is rejected outright here,
// before opening a single file or sending anything to the activity process, rather than letting
// it fail downstream after the round trip.
#define LORIE_MAX_CLIP_ITEMS 1024

// Parses an RFC 2483 text/uri-list property into a LorieClipboardList of already-open fds: the
// X server process has full filesystem access to whatever local path a real X11 client's file
// manager offers, so files are handed to Android as real opened fds instead of being copied
// through the ICCCM property mechanism.
static void lorieHandleUriList(const char* data, size_t size) {
    LorieClipboardItem* items;
    LorieClipboardList* list;
    char** paths;
    size_t i, start, itemCount = 0, capacity = 8;
    uint32_t generation;
    Bool tooManyItems = FALSE;

    items = calloc(capacity, sizeof(LorieClipboardItem));
    paths = calloc(capacity, sizeof(char*));
    if (!items || !paths) {
        log(ERROR, "Out of memory parsing uri-list\n");
        free(items);
        free(paths);
        return;
    }

    for (start = 0, i = 0; i <= size; i++) {
        if (i != size && data[i] != '\n' && data[i] != '\r')
            continue;

        size_t lineLen = i - start;
        if (lineLen > 0 && data[start] != '#') { // '#'-prefixed lines are comments per RFC 2483
            char* path = lorieDecodeFileUri(data + start, lineLen);
            if (path && itemCount >= LORIE_MAX_CLIP_ITEMS) {
                free(path);
                tooManyItems = TRUE;
                break;
            } else if (path) {
                struct stat st;
                int fd = open(path, O_RDONLY);
                if (fd >= 0 && fstat(fd, &st) == 0) {
                    const char* name;

                    if (itemCount == capacity) {
                        size_t newCapacity = capacity * 2;
                        // realloc() invalidates the old pointer on success even if we end up
                        // bailing out right after, so items/paths must be updated unconditionally.
                        LorieClipboardItem* grownItems = realloc(items, newCapacity * sizeof(LorieClipboardItem));
                        if (grownItems)
                            items = grownItems;
                        char** grownPaths = grownItems ? realloc(paths, newCapacity * sizeof(char*)) : NULL;
                        if (grownPaths)
                            paths = grownPaths;

                        if (!grownItems || !grownPaths) {
                            close(fd);
                            free(path);
                            break;
                        }
                        capacity = newCapacity;
                    }

                    name = strrchr(path, '/');
                    name = name ? name + 1 : path;

                    memset(&items[itemCount], 0, sizeof(items[itemCount]));
                    snprintf(items[itemCount].name, sizeof(items[itemCount].name), "%s", name);
                    snprintf(items[itemCount].mime, sizeof(items[itemCount].mime), "application/octet-stream");
                    items[itemCount].kind = LORIE_CLIP_FILE;
                    items[itemCount].fd = fd;
                    items[itemCount].length = (uint64_t) st.st_size;
                    paths[itemCount] = path; // ownership moved into paths[]; not freed below
                    itemCount++;
                } else {
                    if (fd >= 0)
                        close(fd);
                    log(ERROR, "Failed to open uri-list entry %s: %s\n", path, strerror(errno));
                    free(path);
                }
            }
        }
        start = i + 1;
    }

    if (tooManyItems) {
        log(ERROR, "uri-list has more than %d files, refusing to send it to the activity "
                    "(the resulting ClipData would risk a binder TransactionTooLargeException)\n",
            LORIE_MAX_CLIP_ITEMS);
        for (i = 0; i < itemCount; i++) {
            close(items[i].fd);
            free(paths[i]);
        }
        free(items);
        free(paths);
        return;
    }

    if (itemCount == 0) {
        log(DEBUG, "uri-list contained no openable files\n");
        free(items);
        free(paths);
        return;
    }

    list = malloc(sizeof(LorieClipboardList) + itemCount * sizeof(LorieClipboardItem));
    if (!list) {
        log(ERROR, "Out of memory building clipboard file list\n");
        for (i = 0; i < itemCount; i++) {
            close(items[i].fd);
            free(paths[i]);
        }
        free(items);
        free(paths);
        return;
    }
    list->count = itemCount;
    memcpy(list->items, items, itemCount * sizeof(LorieClipboardItem));
    free(items);

    pthread_mutex_lock(&sentClipLock);
    lorieFreeSentClipPathsLocked();
    sentClipGeneration++;
    sentClipPaths = paths;
    sentClipCount = itemCount;
    generation = (uint32_t) sentClipGeneration;
    pthread_mutex_unlock(&sentClipLock);

    log(DEBUG, "Sending clipboard file list to clients (%zu files)\n", itemCount);
    lorieSendClipboardFileList(list, generation);
}

// Caller must hold sentClipLock.
static void lorieFreeSentClipPathsLocked(void) {
    for (size_t i = 0; i < sentClipCount; i++)
        free(sentClipPaths[i]);
    free(sentClipPaths);
    sentClipPaths = NULL;
    sentClipCount = 0;
}

int lorieReopenSentClipItem(uint32_t generation, uint32_t index) {
    int fd = -1;
    pthread_mutex_lock(&sentClipLock);
    if (generation == (uint32_t) sentClipGeneration && index < sentClipCount)
        fd = open(sentClipPaths[index], O_RDONLY);
    pthread_mutex_unlock(&sentClipLock);
    return fd;
}

// Converts fully-assembled property data (whether it arrived directly or was reassembled from an
// INCR transfer) into the wire format the activity side expects, and hands it off.
static void lorieFinishSelectionData(Atom target, const void* data, size_t size) {
    if (target == xaSTRING) {
        char* filtered = calloc(1, size + 1);
        char* utf8 = calloc(1, (size + 1) * 2);
        if (!filtered || !utf8) {
            log(ERROR, "Out of memory converting clipboard text\n");
        } else {
            lorieConvertLF(data, filtered, size);
            lorieLatin1ToUTF8((unsigned char*) utf8, (unsigned char*) filtered);
            log(DEBUG, "Sending clipboard to clients (%zu bytes)\n", strlen(utf8));
            lorieSendClipboardText(utf8, strlen(utf8));
        }
        free(filtered);
        free(utf8);
    } else if (target == xaUTF8_STRING) {
        char* filtered = calloc(1, size + 1);
        if (!filtered) {
            log(ERROR, "Out of memory converting clipboard text\n");
        } else if (!lorieCheckUTF8(data, size)) {
            dprintf(2, "Invalid UTF-8 sequence in clipboard\n");
        } else {
            lorieConvertLF(data, filtered, size);
            log(DEBUG, "Sending clipboard to clients (%zu bytes)\n", strlen(filtered));
            lorieSendClipboardText(filtered, strlen(filtered));
        }
        free(filtered);
    } else if (target == xaTEXT_URI_LIST) {
        lorieHandleUriList(data, size);
    } else {
        // Any other mime (image/*, text/html, application/octet-stream, ...) with no dedicated
        // handling of its own is forwarded as an opaque blob, named after its own target atom.
        const char* mime = NameForAtom(target);
        log(DEBUG, "Sending clipboard blob (%s) to clients (%zu bytes)\n", mime, size);
        lorieSendClipboardBlob(mime, data, size);
    }
}

static void lorieHandleSelection(Atom target) {
    PropertyPtr prop;

    if (dixLookupProperty(&prop, pScreenPtr->root, target, serverClient, DixReadAccess) != Success)
        return;

    log(DEBUG, "Selection notification for CLIPBOARD (target %s, type %s)\n", NameForAtom(target), NameForAtom(prop->type));
    // Only ever act on a reply to what we ourselves last asked for -- this is what lets any mime
    // (image/*, text/html, application/octet-stream, ...) flow through generically below, instead
    // of needing every target we might ever receive enumerated by name up front.
    if (target != pendingRequestTarget)
        return;

    if (target == xaTARGETS && prop->type == XA_ATOM && prop->format == 32) {
        const Atom* atoms = (const Atom*) prop->data;
        size_t count = prop->size;

        // Prefer a real file list over raw bytes whenever the owner offers one (e.g. a file
        // manager selection), so files round-trip as actual named/sized files, not opaque blobs.
        if (lorieHasAtom(xaTEXT_URI_LIST, atoms, count))
            lorieSelectionRequest(xaCLIPBOARD, xaTEXT_URI_LIST);
        else if (lorieHasAtom(xaUTF8_STRING, atoms, count))
            lorieSelectionRequest(xaCLIPBOARD, xaUTF8_STRING);
        else if (lorieHasAtom(xaSTRING, atoms, count))
            lorieSelectionRequest(xaCLIPBOARD, xaSTRING);
        else {
            // Take whatever else the owner offers (image/*, text/html, application/octet-stream,
            // ...) as an opaque blob, skipping ICCCM meta-targets that aren't real content types.
            size_t i;
            for (i = 0; i < count; i++)
                if (atoms[i] != xaTARGETS && atoms[i] != xaTIMESTAMP && atoms[i] != xaMULTIPLE)
                    break;

            if (i < count) {
                log(DEBUG, "Client offers %s, requesting it\n", NameForAtom(atoms[i]));
                lorieSelectionRequest(xaCLIPBOARD, atoms[i]);
            } else
                log(DEBUG, "Client offers no target we understand\n");
        }
    } else if (prop->type == xaINCR) {
        // Property too big for a single GetProperty/ChangeProperty round trip; the owner will
        // stream it in chunks via PropertyNotify once we delete this placeholder (ICCCM 2.7.2).
        log(DEBUG, "Client is starting an INCR transfer for target %s\n", NameForAtom(target));
        free(incrBuf);
        incrBuf = NULL;
        incrLen = 0;
        incrActive = TRUE;
        incrTarget = target;
        DeleteProperty(serverClient, pScreenPtr->root, target);
    } else if (target == xaSTRING && prop->type == xaSTRING && prop->format == 8) {
        lorieFinishSelectionData(target, prop->data, prop->size);
    } else if (target == xaUTF8_STRING && prop->type == xaUTF8_STRING && prop->format == 8) {
        lorieFinishSelectionData(target, prop->data, prop->size);
    } else if (target == xaTEXT_URI_LIST && prop->format == 8) {
        // Owners vary in what they set the property type to for this target (some use the
        // target atom itself, some plain STRING) -- format 8 is the only thing that matters.
        lorieFinishSelectionData(target, prop->data, prop->size);
    } else if (prop->format == 8) {
        // Any other target we explicitly asked for above (image/*, text/html,
        // application/octet-stream, ...) is handled as an opaque blob keyed by its mime name.
        lorieFinishSelectionData(target, prop->data, prop->size);
    } else {
        log(ERROR, "Got property for target %s but type %s/format %d did not match what we expected\n",
            NameForAtom(target), NameForAtom(prop->type), prop->format);
    }
}

static void loriePropertyStateCallback(__unused CallbackListPtr *callbacks, __unused void *data, void *args) {
    PropertyStateRec *rec = (PropertyStateRec *) args;
    PropertyPtr prop;

    if (!incrActive || rec->state != PropertyNewValue || rec->win != pScreenPtr->root || rec->prop->propertyName != incrTarget)
        return;

    if (dixLookupProperty(&prop, pScreenPtr->root, incrTarget, serverClient, DixReadAccess) != Success)
        return;

    if (prop->size == 0) {
        // A zero-length chunk is the ICCCM-defined terminator for the transfer.
        log(DEBUG, "INCR transfer for target %s complete (%zu bytes)\n", NameForAtom(incrTarget), incrLen);
        incrActive = FALSE;
        DeleteProperty(serverClient, pScreenPtr->root, incrTarget);
        lorieFinishSelectionData(incrTarget, incrBuf, incrLen);
        free(incrBuf);
        incrBuf = NULL;
        incrLen = 0;
        return;
    }

    void* newBuf = realloc(incrBuf, incrLen + prop->size);
    if (!newBuf) {
        log(ERROR, "Out of memory reassembling INCR transfer for target %s\n", NameForAtom(incrTarget));
        free(incrBuf);
        incrBuf = NULL;
        incrLen = 0;
        incrActive = FALSE;
        return;
    }
    incrBuf = newBuf;
    memcpy((char*) incrBuf + incrLen, prop->data, prop->size);
    incrLen += prop->size;
    log(DEBUG, "INCR chunk for target %s: %u bytes (%zu bytes so far)\n", NameForAtom(incrTarget), prop->size, incrLen);

    DeleteProperty(serverClient, pScreenPtr->root, incrTarget);
}

// Invoked by dix/events.c's ProcSendEvent for every validated X_SendEvent request. A real
// client's SelectionNotify reply to our own lorieSelectionRequest() lands here: it's addressed to
// pScreenPtr->root, which serverClient can't otherwise learn about (WriteEventsToClient() never
// delivers to serverClient, and there is no event mask for SelectionNotify to select on).
static void lorieSendEventCallback(__unused CallbackListPtr *callbacks, __unused void *data, void *args) {
    SendEventInfoRec *info = (SendEventInfoRec *) args;
    xEvent *e = info->event;
    __typeof__(e->u.selectionNotify)* sn = &e->u.selectionNotify;

    if (clipboardEnabled && info->window == pScreenPtr->root &&
        (e->u.u.type & 0x7f) == SelectionNotify && sn->selection == xaCLIPBOARD && sn->target == sn->property)
        lorieHandleSelection(sn->target);
}

static void lorieSelectionCallback(__unused CallbackListPtr *callbacks, __unused void * data, void * args) {
    SelectionInfoRec *info = (SelectionInfoRec *) args;

    if (clipboardEnabled && info->selection->selection == xaCLIPBOARD && info->kind == SelectionSetOwner && info->selection->client != serverClient)
        lorieSelectionRequest(xaCLIPBOARD, xaTARGETS);
}

/* end functions related to clipboard receiving */

/* functions related to clipboard announcing and sending */

static int lorieConvertSelection(ClientPtr client, Atom selection, Atom target, Atom property, Window requestor, CARD32 time, const LorieClipboardData* data, const LorieClipboardList* list) {
    Selection *pSel;
    WindowPtr pWin;
    int rc;

    Atom realProperty;

    xEvent event;

    if (data == NULL && list == NULL) {
        log(DEBUG, "Selection request for %s (type %s)",
            NameForAtom(selection), NameForAtom(target));
    } else {
        log(DEBUG, "Sending data for selection request for %s (type %s)",
            NameForAtom(selection), NameForAtom(target));
    }

    rc = dixLookupSelection(&pSel, selection, client, DixGetAttrAccess);
    if (rc != Success)
        return rc;

    /* We do not validate the time argument because neither does
     * dix/selection.c and some clients (e.g. Qt) relies on this */

    rc = dixLookupWindow(&pWin, requestor, client, DixSetAttrAccess);
    if (rc != Success)
        return rc;

    if (property != None)
        realProperty = property;
    else
        realProperty = target;

    /* FIXME: MULTIPLE target */

    if (target == xaTIMESTAMP) {
        // Always answerable immediately: it doesn't depend on the actual clip content.
        rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                     XA_INTEGER, 32, PropModeReplace, 1,
                                     &pSel->lastTimeChanged.milliseconds,
                                     TRUE);
        if (rc != Success)
            return rc;
    } else if (data == NULL && list == NULL) {
        // Including TARGETS here: what we can honestly advertise depends on the real data (which
        // mime it actually is), so every target waits for it instead of being pre-judged here --
        // an unmatched target still gets BadMatch below, once the real data is in hand.
        struct LorieDataTarget* ldt;

        ldt = calloc(1, sizeof(struct LorieDataTarget));
        if (ldt == NULL)
            return BadAlloc;

        ldt->client = client;
        ldt->selection = selection;
        ldt->target = target;
        ldt->property = property;
        ldt->requestor = requestor;
        ldt->time = time;

        ldt->next = lorieDataTargetHead;
        lorieDataTargetHead = ldt;

        log(DEBUG, "Requesting clipboard data from client");
        lorieRequestClipboard();

        return Success;
    } else if (list != NULL) {
        // A single-item clip (e.g. a plain "copy this photo/file") also gets its own mime
        // advertised/served directly, not just as a file reference, for clients that paste the
        // bytes themselves rather than a file (e.g. "paste as new image" instead of "paste into
        // this folder").
        Bool singleBlob = list->count == 1 && list->items[0].mime[0] && list->items[0].name[0];

        if (target == xaTARGETS) {
            Atom targets[5] = { xaTARGETS, xaTIMESTAMP, xaTEXT_URI_LIST, xaGNOME_COPIED_FILES };
            int count = 4;
            if (singleBlob)
                targets[count++] = MakeAtom(list->items[0].mime, strlen(list->items[0].mime), TRUE);

            rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                         XA_ATOM, 32, PropModeReplace,
                                         count, targets, TRUE);
            if (rc != Success)
                return rc;
        } else if (singleBlob && !strcmp(NameForAtom(target), list->items[0].mime)) {
            const char* dir = lorieClipStagingDirFor(clipGeneration);
            char full[PATH_MAX];
            struct stat st;
            void* buf;
            int fd;

            snprintf(full, sizeof(full), "%s/%s", dir, list->items[0].name);
            fd = open(full, O_RDONLY);
            if (fd < 0 || fstat(fd, &st) != 0) {
                if (fd >= 0)
                    close(fd);
                return BadMatch;
            }

            buf = malloc((size_t) st.st_size);
            if (!buf) {
                close(fd);
                return BadAlloc;
            }
            if (read(fd, buf, (size_t) st.st_size) != st.st_size) {
                close(fd);
                free(buf);
                return BadMatch;
            }
            close(fd);

            rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                         target, 8, PropModeReplace,
                                         (unsigned long) st.st_size, buf, TRUE);
            free(buf);
            if (rc != Success)
                return rc;
        } else if (target == xaTEXT_URI_LIST || target == xaGNOME_COPIED_FILES) {
            const char* dir = lorieClipStagingDirFor(clipGeneration);
            Bool gnomeFormat = target == xaGNOME_COPIED_FILES;
            size_t maxLen = gnomeFormat ? 5 : 0, i; // "copy\n" prefix for the gnome format
            Bool first = TRUE;
            char* body;
            char* out;

            // Upper bound: "file://" + percent-encoded("<dir>/<name>") (every byte can expand to
            // 3 chars) + separator, per item.
            for (i = 0; i < list->count; i++) {
                if (!list->items[i].name[0])
                    continue;
                maxLen += 7 + (strlen(dir) + 1 + strlen(list->items[i].name)) * 3 + 2;
            }

            if (maxLen == (gnomeFormat ? 5 : 0))
                return BadMatch; // nothing in this clip staged successfully

            body = malloc(maxLen);
            if (!body)
                return BadAlloc;

            out = body;
            if (gnomeFormat) {
                memcpy(out, "copy\n", 5);
                out += 5;
            }
            for (i = 0; i < list->count; i++) {
                char full[PATH_MAX];
                if (!list->items[i].name[0])
                    continue;

                // Nautilus/Thunar's gnome-copied-files reader splits items on a bare "\n" and
                // doesn't expect one after the last entry; text/uri-list (RFC 2483) wants a line
                // terminator after every entry including the last.
                if (gnomeFormat) {
                    if (!first) {
                        memcpy(out, "\n", 1);
                        out += 1;
                    }
                    first = FALSE;
                }

                snprintf(full, sizeof(full), "%s/%s", dir, list->items[i].name);
                memcpy(out, "file://", 7);
                out += 7;
                out = lorieAppendUriEncoded(out, full);

                if (!gnomeFormat) {
                    memcpy(out, "\r\n", 2);
                    out += 2;
                }
            }

            rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                         target, 8, PropModeReplace,
                                         (unsigned long) (out - body), body, TRUE);
            free(body);
            if (rc != Success)
                return rc;
        } else {
            return BadMatch;
        }
    } else if (target == xaTARGETS) {
        Atom targets[6] = { xaTARGETS, xaTIMESTAMP, xaSTRING, xaTEXT, xaUTF8_STRING };
        int count = 5;

        if (strcmp(data->mime, "text/plain") != 0)
            targets[count++] = MakeAtom(data->mime, strlen(data->mime), TRUE);

        rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                     XA_ATOM, 32, PropModeReplace,
                                     count, targets, TRUE);
        if (rc != Success)
            return rc;
    } else {
        Bool isText = !strcmp(data->mime, "text/plain");

        if ((target == xaSTRING) || (target == xaTEXT)) {
            const char* latin1;

            if (!isText) {
                log(DEBUG, "Client asked for %s but the clipboard currently holds %s\n", NameForAtom(target), data->mime);
                return BadMatch;
            }

            latin1 = lorieUtf8ToLatin1((const char*) data->data);
            if (latin1 == NULL)
                return BadAlloc;

            rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                         XA_STRING, 8, PropModeReplace,
                                         strlen(latin1), latin1, TRUE);

            free((void*) latin1);

            if (rc != Success)
                return rc;
        } else if (target == xaUTF8_STRING) {
            if (!isText) {
                log(DEBUG, "Client asked for UTF8_STRING but the clipboard currently holds %s\n", data->mime);
                return BadMatch;
            }

            rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                         xaUTF8_STRING, 8, PropModeReplace,
                                         data->length, data->data, TRUE);
            if (rc != Success)
                return rc;
        } else if (!strcmp(NameForAtom(target), data->mime)) {
            log(DEBUG, "Handing %zu bytes of %s to client\n", data->length, data->mime);
            rc = dixChangeWindowProperty(serverClient, pWin, realProperty,
                                         target, 8, PropModeReplace,
                                         data->length, data->data, TRUE);
            if (rc != Success)
                return rc;
        } else {
            log(DEBUG, "Client asked for %s but the clipboard currently holds %s\n", NameForAtom(target), data->mime);
            return BadMatch;
        }
    }

    event.u.u.type = SelectionNotify;
    event.u.selectionNotify.time = time;
    event.u.selectionNotify.requestor = requestor;
    event.u.selectionNotify.selection = selection;
    event.u.selectionNotify.target = target;
    event.u.selectionNotify.property = property;
    WriteEventsToClient(client, 1, &event);
    return Success;
}

// Invoked by dix/selection.c's ProcConvertSelection whenever a request targets a selection we
// own as serverClient (see lorieOwnSelection), instead of us monkey-patching X_ConvertSelection
// in ProcVector ourselves.
static void lorieSelectionBridgeCallback(__unused CallbackListPtr *callbacks, __unused void *data, void *args) {
    SelectionBridgeInfoRec *info = (SelectionBridgeInfoRec *) args;
    int rc;

    if (info->selection != xaCLIPBOARD)
        return;

    /* cachedClip will be NULL for the first request, but can then be
     * reused once we've gotten the data once from the client */
    rc = lorieConvertSelection(info->client, info->selection, info->target,
                               info->property, info->requestor, info->time,
                               cachedClip, cachedClipList);
    if (rc != Success) {
        xEvent event;

        memset(&event, 0, sizeof(xEvent));
        event.u.u.type = SelectionNotify;
        event.u.selectionNotify.time = info->time;
        event.u.selectionNotify.requestor = info->requestor;
        event.u.selectionNotify.selection = info->selection;
        event.u.selectionNotify.target = info->target;
        event.u.selectionNotify.property = None;
        WriteEventsToClient(info->client, 1, &event);
    }
}

static int lorieOwnSelection(Atom selection) {
    Selection *pSel;
    int rc;

    SelectionInfoRec info;

    rc = dixLookupSelection(&pSel, selection, serverClient, DixSetAttrAccess);
    if (rc == Success) {
        if (pSel->client && (pSel->client != serverClient)) {
            xEvent event = {
                    .u.selectionClear.time = currentTime.milliseconds,
                    .u.selectionClear.window = pSel->window,
                    .u.selectionClear.atom = pSel->selection
            };
            event.u.u.type = SelectionClear;
            WriteEventsToClient(pSel->client, 1, &event);
        }
    } else if (rc == BadMatch) {
        pSel = dixAllocateObjectWithPrivates(Selection, PRIVATE_SELECTION);
        if (!pSel)
            return BadAlloc;

        pSel->selection = selection;

        rc = XaceHookSelectionAccess(serverClient, &pSel, DixCreateAccess | DixSetAttrAccess);
        if (rc != Success) {
            free(pSel);
            return rc;
        }

        pSel->next = CurrentSelections;
        CurrentSelections = pSel;
    }
    else
        return rc;

    pSel->lastTimeChanged = currentTime;
    pSel->window = pScreenPtr->root->drawable.id;
    pSel->pWin = pScreenPtr->root;
    pSel->client = serverClient;

    log(DEBUG, "Grabbed %s selection", NameForAtom(selection));

    info.selection = pSel;
    info.client = serverClient;
    info.kind = SelectionSetOwner;
    CallCallbacks(&SelectionCallback, &info);

    return Success;
}

void lorieHandleClipboardAnnounce(void) {
    // The data has changed in some way, so whatever is in our cache is now stale
    lorieFreeCachedClip();

    int rc;

    log(DEBUG, "Remote clipboard announced, grabbing local ownership");

    rc = lorieOwnSelection(xaCLIPBOARD);
    if (rc != Success)
        log(ERROR, "Could not set CLIPBOARD selection");
}

void lorieHandleClipboardData(LorieClipboardData* data) {
    struct LorieDataTarget* next;

    log(DEBUG, "Got remote clipboard data (%s, %zu bytes), sending to X11 clients", data->mime, data->length);

    lorieFreeCachedClip();
    cachedClip = data;

    while (lorieDataTargetHead != NULL) {
        int rc;
        xEvent event;

        rc = lorieConvertSelection(lorieDataTargetHead->client,
                                   lorieDataTargetHead->selection,
                                   lorieDataTargetHead->target,
                                   lorieDataTargetHead->property,
                                   lorieDataTargetHead->requestor,
                                   lorieDataTargetHead->time,
                                 cachedClip, NULL);
        if (rc != Success) {
            event.u.u.type = SelectionNotify;
            event.u.selectionNotify.time = lorieDataTargetHead->time;
            event.u.selectionNotify.requestor = lorieDataTargetHead->requestor;
            event.u.selectionNotify.selection = lorieDataTargetHead->selection;
            event.u.selectionNotify.target = lorieDataTargetHead->target;
            event.u.selectionNotify.property = None;
            WriteEventsToClient(lorieDataTargetHead->client, 1, &event);
        }

        next = lorieDataTargetHead->next;
        free(lorieDataTargetHead);
        lorieDataTargetHead = next;
    }
}

void lorieHandleClipboardDataList(LorieClipboardList* list) {
    struct LorieDataTarget* next;
    size_t i;

    if (!list)
        return;

    log(DEBUG, "Got remote clipboard file list (%zu items), materializing", list->count);

    lorieFreeCachedClip(); // drops whatever was cached before, including any previous staged generation
    clipGeneration++;

    for (i = 0; i < list->count; i++) {
        LorieClipboardItem* item = &list->items[i];
        char staged[sizeof(item->name)];

        if (item->fd < 0) {
            item->name[0] = '\0';
            continue;
        }

        if (lorieMaterializeClipFile(clipGeneration, item->name, item->fd, staged, sizeof(staged)))
            snprintf(item->name, sizeof(item->name), "%s", staged);
        else
            item->name[0] = '\0'; // marks this item unavailable; lorieConvertSelection skips it
        item->fd = -1; // materialized (or failed) -- either way, the fd is spent
    }

    cachedClipList = list;

    while (lorieDataTargetHead != NULL) {
        int rc;
        xEvent event;

        rc = lorieConvertSelection(lorieDataTargetHead->client,
                                   lorieDataTargetHead->selection,
                                   lorieDataTargetHead->target,
                                   lorieDataTargetHead->property,
                                   lorieDataTargetHead->requestor,
                                   lorieDataTargetHead->time,
                                 NULL, cachedClipList);
        if (rc != Success) {
            event.u.u.type = SelectionNotify;
            event.u.selectionNotify.time = lorieDataTargetHead->time;
            event.u.selectionNotify.requestor = lorieDataTargetHead->requestor;
            event.u.selectionNotify.selection = lorieDataTargetHead->selection;
            event.u.selectionNotify.target = lorieDataTargetHead->target;
            event.u.selectionNotify.property = None;
            WriteEventsToClient(lorieDataTargetHead->client, 1, &event);
        }

        next = lorieDataTargetHead->next;
        free(lorieDataTargetHead);
        lorieDataTargetHead = next;
    }
}

// Directory a given generation's files are staged under: "$TMPDIR/.termux-x11-clipboard/<generation>".
// Returns a pointer to a static buffer, valid until the next call.
static const char* lorieClipStagingDirFor(int generation) {
    static char path[PATH_MAX];
    const char* tmpdir = getenv("TMPDIR");
    snprintf(path, sizeof(path), "%s/.termux-x11-clipboard/%d", tmpdir ? tmpdir : "/tmp", generation);
    return path;
}

static int lorieMkdirRecursive(const char* path) {
    char tmp[PATH_MAX];
    char* p;

    snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    return (mkdir(tmp, 0700) == 0 || errno == EEXIST) ? 0 : -1;
}

static void lorieRemoveStagingDir(int generation) {
    const char* dir = lorieClipStagingDirFor(generation);
    DIR* d = opendir(dir);
    struct dirent* ent;
    char path[PATH_MAX];

    if (!d)
        return;

    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

// Copies srcFd's content into a real file under the given generation's staging directory
// (creating it if needed), closing srcFd either way. Only ever uses the basename of `name`, and
// only ever writes into a directory this process itself just created for this generation -- that,
// together, is the full "belongs to the current clip" validation: nothing user/client-controlled
// can make this touch a path outside the staging directory. Writes the actual on-disk name (which
// may differ from `name` if it collided with an earlier item in the same list) to outName.
static Bool lorieMaterializeClipFile(int generation, const char* name, int srcFd, char* outName, size_t outNameSize) {
    const char* dir = lorieClipStagingDirFor(generation);
    char destPath[PATH_MAX];
    char safeName[256];
    const char* base;
    int dstFd = -1, attempt;
    ssize_t n;
    char buf[65536];

    if (lorieMkdirRecursive(dir) != 0) {
        log(ERROR, "Failed to create clipboard staging dir %s: %s\n", dir, strerror(errno));
        close(srcFd);
        return FALSE;
    }

    base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (!*base)
        base = "file";

    for (attempt = 0; attempt < 100; attempt++) {
        if (attempt == 0)
            snprintf(safeName, sizeof(safeName), "%s", base);
        else
            snprintf(safeName, sizeof(safeName), "%d-%s", attempt, base);

        snprintf(destPath, sizeof(destPath), "%s/%s", dir, safeName);
        dstFd = open(destPath, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (dstFd >= 0 || errno != EEXIST)
            break;
    }

    if (dstFd < 0) {
        log(ERROR, "Failed to create staged clipboard file %s: %s\n", destPath, strerror(errno));
        close(srcFd);
        return FALSE;
    }

    while ((n = read(srcFd, buf, sizeof(buf))) > 0)
        if (write(dstFd, buf, n) != n) {
            log(ERROR, "Short write staging clipboard file %s: %s\n", destPath, strerror(errno));
            break;
        }

    close(srcFd);
    close(dstFd);
    snprintf(outName, outNameSize, "%s", safeName);
    return TRUE;
}

static char* lorieAppendUriEncoded(char* out, const char* s) {
    static const char safe[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~/";
    for (; *s; s++) {
        if (strchr(safe, *s))
            *out++ = *s;
        else
            out += sprintf(out, "%%%02X", (unsigned char) *s);
    }
    return out;
}

/* end functions related to clipboard announcing and sending */

void lorieInitClipboard(void) {
#define ATOM(name) xa##name = MakeAtom(#name, strlen(#name), TRUE)
    ATOM(TIMESTAMP); ATOM(TEXT); ATOM(CLIPBOARD); ATOM(TARGETS); ATOM(STRING); ATOM(UTF8_STRING); ATOM(INCR); ATOM(MULTIPLE);
    xaTEXT_URI_LIST = MakeAtom("text/uri-list", strlen("text/uri-list"), TRUE);
    // GTK/Nautilus/Thunar request this in preference to text/uri-list for file pastes, unprompted
    // (even when it isn't offered in our TARGETS answer), and treat a missing/refused answer as
    // "nothing to paste" rather than falling back. Format: "copy\n" or "cut\n" + uris joined by \n.
    xaGNOME_COPIED_FILES = MakeAtom("x-special/gnome-copied-files", strlen("x-special/gnome-copied-files"), TRUE);

    if (!AddCallback(&SelectionCallback, lorieSelectionCallback, NULL))
        FatalError("Adding SelectionCallback failed\n");

    if (!AddCallback(&SelectionBridgeCallback, lorieSelectionBridgeCallback, NULL))
        FatalError("Adding SelectionBridgeCallback failed\n");

    if (!AddCallback(&SendEventCallback, lorieSendEventCallback, NULL))
        FatalError("Adding SendEventCallback failed\n");

    if (!AddCallback(&PropertyStateCallback, loriePropertyStateCallback, NULL))
        FatalError("Adding PropertyStateCallback failed\n");
}
