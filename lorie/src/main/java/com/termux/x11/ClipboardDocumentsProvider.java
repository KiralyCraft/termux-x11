package com.termux.x11;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Build;
import android.os.CancellationSignal;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.SharedMemory;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.util.Log;
import android.webkit.MimeTypeMap;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import java.io.FileNotFoundException;
import java.io.IOException;
import java.nio.ByteBuffer;

/**
 * Exposes the current X11-clipboard item(s) as real DocumentsContract documents.
 *
 * A plain ContentProvider isn't enough for DocumentsUI-family file managers: their
 * paste-from-clipboard code (RuntimeDocumentClipper/CopyJob) doesn't do a generic
 * openInputStream() -- it assumes the pasted Uri is itself a /document/<id> Uri, reconstructs a
 * DocumentInfo from it, and calls DocumentsContract.createDocument()/queryDocument() (reading
 * Document.COLUMN_MIME_TYPE) on it. Against a non-SAF provider that all fails silently or with a
 * SecurityException, well before our code ever runs. A DocumentsContract Uri works equally well
 * for ordinary (non-DocumentsUI) apps calling plain openInputStream()/openFileDescriptor(): SAF
 * document Uris are regular content:// Uris, and Document.COLUMN_DISPLAY_NAME/COLUMN_SIZE share
 * their string values with OpenableColumns.DISPLAY_NAME/SIZE.
 *
 * Not registered as a browsable root (queryRoots() is empty): these documents only exist as
 * clipboard paste targets, not as a persistent storage location to navigate into.
 *
 * Two kinds of item:
 * - FILE: backed by a real file the X server has open. openDocument() doesn't serve reads from
 *   the fd it was originally handed -- that fd can't be dup()'d a second time without racing on
 *   its shared file position, and can't be reopened via /proc/self/fd either (our uid has no path
 *   access to a file the X server process opened under Termux's own uid -- only the fd itself was
 *   shared, over a socket). Instead every openDocument() call asks the X server for a fresh,
 *   independent fd by (generation, index) -- see LorieView.reopenClipboardItem. The originally
 *   received fd is kept only for metadata (COLUMN_SIZE) and closed on the next publish().
 * - RAW_BYTES: a non-file blob (e.g. an image rendered directly, no backing path) held in a
 *   shared-memory region. There's no X-server-side path to reopen, so every openDocument() call
 *   instead makes its own private ashmem copy of the source bytes (see copyToAshmem()).
 */
public class ClipboardDocumentsProvider extends DocumentsProvider {
    private static final String AUTHORITY = BuildConfig.APPLICATION_ID + ".clipboarddocuments";

    private static final String[] DEFAULT_ROOT_PROJECTION = {
            Root.COLUMN_ROOT_ID, Root.COLUMN_FLAGS, Root.COLUMN_TITLE, Root.COLUMN_DOCUMENT_ID,
    };
    private static final String[] DEFAULT_DOCUMENT_PROJECTION = {
            Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE,
            Document.COLUMN_SIZE, Document.COLUMN_FLAGS,
    };

    private enum Kind { FILE, RAW_BYTES }

    private static final class Item {
        final String name;
        final String mime;
        final Kind kind;
        final ParcelFileDescriptor pfd;
        Item(String name, String mime, Kind kind, ParcelFileDescriptor pfd) {
            this.name = name;
            this.mime = mime;
            this.kind = kind;
            this.pfd = pfd;
        }
    }

    // Not a locally-assigned counter: this is the same generation number the X server embedded in
    // EVENT_CLIPBOARD_LIST_BEGIN, so a later reopen request unambiguously names the right clip.
    // Only meaningful for the current clip when it's FILE-kind.
    private static int wireGeneration = -1;
    // Uri-facing generation: bumped on every publish, FILE or RAW_BYTES alike.
    private static int currentGeneration = -1;
    private static long currentNativeContext;
    private static Item[] currentItems;

    static synchronized Uri[] publishFiles(String[] names, String[] mimes, ParcelFileDescriptor[] pfds, int generation, long nativeContext) {
        closeCurrentLocked();

        currentGeneration++;
        wireGeneration = generation;
        currentNativeContext = nativeContext;
        currentItems = new Item[pfds.length];
        Uri[] uris = new Uri[pfds.length];
        for (int i = 0; i < pfds.length; i++) {
            currentItems[i] = new Item(names[i], mimes[i], Kind.FILE, pfds[i]);
            uris[i] = buildUri(i);
        }
        return uris;
    }

    static synchronized Uri publishRawBytes(String mime, ParcelFileDescriptor pfd) {
        closeCurrentLocked();

        currentGeneration++;
        currentItems = new Item[]{new Item("", mime, Kind.RAW_BYTES, pfd)};
        return buildUri(0);
    }

    private static Uri buildUri(int index) {
        return DocumentsContract.buildDocumentUri(AUTHORITY, currentGeneration + "_" + index);
    }

    private static void closeCurrentLocked() {
        if (currentItems == null)
            return;
        for (Item item : currentItems) {
            try {
                item.pfd.close();
            } catch (IOException ignored) {
            }
        }
        currentItems = null;
    }

    @Nullable
    private static synchronized Item findItem(String documentId) {
        int sep = documentId.indexOf('_');
        if (sep < 0)
            return null;

        try {
            int generation = Integer.parseInt(documentId.substring(0, sep));
            int index = Integer.parseInt(documentId.substring(sep + 1));
            if (generation != currentGeneration || currentItems == null || index < 0 || index >= currentItems.length)
                return null;
            return currentItems[index];
        } catch (NumberFormatException e) {
            return null;
        }
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public Cursor queryRoots(String[] projection) {
        return new MatrixCursor(projection != null ? projection : DEFAULT_ROOT_PROJECTION);
    }

    @Override
    public Cursor queryDocument(String documentId, @Nullable String[] projection) throws FileNotFoundException {
        Item item = findItem(documentId);
        if (item == null)
            throw new FileNotFoundException("No clipboard item for " + documentId);

        String[] columns = projection != null ? projection : DEFAULT_DOCUMENT_PROJECTION;
        MatrixCursor cursor = new MatrixCursor(columns);
        Object[] row = new Object[columns.length];
        for (int i = 0; i < columns.length; i++) {
            switch (columns[i]) {
                case Document.COLUMN_DOCUMENT_ID: row[i] = documentId; break;
                case Document.COLUMN_DISPLAY_NAME:
                    row[i] = !item.name.isEmpty() ? item.name : ("clipboard." + extensionFor(item.mime));
                    break;
                case Document.COLUMN_MIME_TYPE: row[i] = item.mime; break;
                case Document.COLUMN_SIZE:
                    try {
                        row[i] = statSize(item.pfd);
                    } catch (IOException e) {
                        row[i] = -1L;
                    }
                    break;
                case Document.COLUMN_FLAGS: row[i] = 0; break;
            }
        }
        cursor.addRow(row);
        return cursor;
    }

    private static String extensionFor(String mime) {
        String ext = MimeTypeMap.getSingleton().getExtensionFromMimeType(mime);
        return ext != null ? ext : "bin";
    }

    @Override
    public Cursor queryChildDocuments(String parentDocumentId, @Nullable String[] projection, @Nullable String sortOrder) {
        // Clipboard documents are always leaf files, never containers.
        return new MatrixCursor(projection != null ? projection : DEFAULT_DOCUMENT_PROJECTION);
    }

    @Override
    public ParcelFileDescriptor openDocument(String documentId, @NonNull String mode, @Nullable CancellationSignal signal) throws FileNotFoundException {
        Item item = findItem(documentId);
        if (item == null) {
            Log.e("CLIP", "openDocument(" + documentId + ") called but no matching clipboard item is published");
            throw new FileNotFoundException("No clipboard item available");
        }

        if (item.kind == Kind.FILE) {
            int index = Integer.parseInt(documentId.substring(documentId.indexOf('_') + 1));
            int fd = LorieView.reopenClipboardItem(currentNativeContext, wireGeneration, index);
            if (fd < 0) {
                Log.e("CLIP", "openDocument(" + documentId + ") -> X server reopen failed or timed out");
                throw new FileNotFoundException("Failed to reopen clipboard item");
            }

            Log.d("CLIP", "openDocument(" + documentId + ") -> reopened via X server, fd=" + fd);
            return ParcelFileDescriptor.adoptFd(fd);
        }

        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O_MR1) {
            throw new FileNotFoundException("Clipboard blobs need Android 8.1 or newer");
        }

        try {
            ParcelFileDescriptor copy = copyToAshmem(item.pfd);
            Log.d("CLIP", "openDocument(" + documentId + ") -> ashmem copy, fd=" + copy.getFd());
            return copy;
        } catch (ErrnoException | IOException e) {
            Log.e("CLIP", "Failed to copy clipboard item into ashmem for " + documentId, e);
            throw new FileNotFoundException(e.getMessage());
        }
    }

    /**
     * Fills a fresh ashmem region with a private copy of src's contents via pread() (src's own
     * position is untouched), then hands it out read-only. There's no X-server-side path to
     * reopen for a raw-bytes item, so every open gets its own independent copy instead.
     */
    private static ParcelFileDescriptor copyToAshmem(ParcelFileDescriptor src) throws ErrnoException, IOException {
        long size = statSize(src);
        SharedMemory shm = SharedMemory.create("clipboard-item", (int) size);
        try {
            ByteBuffer buf = shm.mapReadWrite();
            try (ParcelFileDescriptor dup = src.dup()) {
                byte[] chunk = new byte[64 * 1024];
                long offset = 0;
                while (offset < size) {
                    int toRead = (int) Math.min(chunk.length, size - offset);
                    int n = Os.pread(dup.getFileDescriptor(), chunk, 0, toRead, offset);
                    if (n <= 0)
                        break;
                    buf.position((int) offset);
                    buf.put(chunk, 0, n);
                    offset += n;
                }
            } finally {
                SharedMemory.unmap(buf);
            }
            shm.setProtect(OsConstants.PROT_READ);
            return extractFd(shm);
        } finally {
            shm.close();
        }
    }

    /**
     * SharedMemory doesn't expose its fd through any public getter; parceling it through its own
     * Parcelable implementation and reading the fd back out is the only public way to get an
     * independent, dup()'d ParcelFileDescriptor from it.
     */
    private static ParcelFileDescriptor extractFd(SharedMemory shm) {
        Parcel parcel = Parcel.obtain();
        try {
            shm.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            return parcel.readFileDescriptor();
        } finally {
            parcel.recycle();
        }
    }

    /** stat() doesn't report a size for ashmem-backed fds (not a regular file); fall back to the ashmem ioctl. */
    private static long statSize(ParcelFileDescriptor pfd) throws IOException {
        long size = pfd.getStatSize();
        if (size >= 0)
            return size;
        try (SharedMemory shm = SharedMemory.fromFileDescriptor(pfd.dup())) {
            return shm.getSize();
        }
    }
}
