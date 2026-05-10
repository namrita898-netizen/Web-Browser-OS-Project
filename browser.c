/*
 * ================================================================
 *  OS TEXT BROWSER  —  Multi-Process, Multi-Threaded Web Browser
 * ================================================================
 *
 *  OS CONCEPTS DEMONSTRATED
 *  ─────────────────────────────────────────────────────────────
 *  1. Process Management  : fork() creates a unique child process
 *                           for every tab. SIGCHLD + waitpid()
 *                           prevent zombie leaks.
 *  2. IPC – Pipes         : Anonymous pipe per tab carries a
 *                           one-byte status code ('O'=ok/'E'=err)
 *                           from child → parent.
 *  3. IPC – Shared Memory : POSIX shm_open/mmap transfers the
 *                           full HTTP response (up to 256 KB)
 *                           from child → parent without copying.
 *  4. Semaphores          : Named POSIX semaphore per tab signals
 *                           the parent when SHM data is ready.
 *  5. Reader-Writer Lock  : pthread_rwlock_t protects the cache
 *                           map. Multiple tabs can read the cache
 *                           concurrently; only one write at a time.
 *  6. Mutex Lock          : pthread_mutex_t serialises writes to
 *                           History.txt.
 *  7. pthreads            : History I/O runs in detached worker
 *                           threads so the GUI never blocks.
 *  8. Signal Handling     : SIGCHLD handler reaps dead children
 *                           asynchronously via waitpid(WNOHANG).
 *  9. Memory Management   : malloc / calloc / realloc / free used
 *                           throughout; mmap for shared memory.
 * 10. Networking          : gethostbyname() for DNS resolution,
 *                           TCP socket with HTTP/1.1 GET.
 *
 *  BUILD
 *  ─────
 *    gcc browser.c -o browser \
 *        $(pkg-config --cflags --libs gtk+-3.0) \
 *        -lpthread -lrt -Wall -Wextra \
 *        -Wno-deprecated-declarations
 *
 *  OR just:  make
 *
 *  RUN
 *  ───
 *    ./browser
 *
 *  TEST URLs  (HTTP port 80 only — no HTTPS)
 *  ──────────────────────────────────────────
 *    neverssl.com          httpforever.com       info.cern.ch
 *    http://neverssl.com   http://httpforever.com
 * ================================================================
 */

/* ─── Includes ─────────────────────────────────────────────────── */
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <semaphore.h>

/* ─── Constants ────────────────────────────────────────────────── */
#define MAX_TABS          20
#define MAX_CACHE_ENTRIES 50
#define MAX_HISTORY       200
#define SHM_SIZE          (4096 * 64)   /* 256 KB per tab          */
#define HTTP_PORT         80
#define CACHE_DIR         "Cache"
#define HISTORY_FILE      "History.txt"
#define CACHE_MAP_FILE    "CacheMap.txt"
#define POLL_INTERVAL_MS  300           /* GUI poll period (ms)    */

/* ─── Data Structures ──────────────────────────────────────────── */

typedef struct {
    char url[512];
    char filename[256];
} CacheEntry;

typedef struct {
    char url[512];
} HistoryEntry;

/*
 * TabInfo – one instance per open tab.
 * Bundles together all process, IPC, and GUI state for a tab.
 */
typedef struct {
    int        index;          /* slot index 0‥MAX_TABS-1         */
    bool       active;         /* slot is in use                   */
    bool       loading;        /* child fetch in progress          */
    char       pipe_status;    /* 0=pending 'O'=ok 'E'=error       */
    pid_t      child_pid;      /* PID of the fetch child process   */
    char       url[512];       /* last URL requested               */
    int        pipe_rd;        /* parent reads status byte here    */
    int        pipe_wr;        /* child writes status byte here    */
    char       shm_name[32];   /* e.g. "/WBD_3"                    */
    char       sem_name[32];   /* e.g. "/WBS_3"                    */
    /* GTK widgets (valid only while active == true) */
    GtkWidget *notebook_page;  /* VBox that is the tab page        */
    GtkWidget *url_entry;      /* URL entry bar                    */
    GtkWidget *status_label;   /* thin status line                 */
    GtkWidget *spinner;        /* activity indicator               */
    GtkWidget *content_view;   /* GtkTextView showing raw content  */
    GtkWidget *tab_label_box;  /* HBox used as notebook tab label  */
} TabInfo;

/* ─── Globals ──────────────────────────────────────────────────── */
static GtkWidget  *g_window;
static GtkWidget  *g_notebook;
static TabInfo     g_tabs[MAX_TABS];

/* Cache protected by a Reader-Writer lock */
static CacheEntry      *g_cache        = NULL;
static int              g_cache_count  = 0;
static pthread_rwlock_t g_cache_rwlock = PTHREAD_RWLOCK_INITIALIZER;

/* History protected by a Mutex */
static HistoryEntry    *g_history      = NULL;
static int              g_history_count = 0;
static pthread_mutex_t  g_history_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ─── Forward Declarations ─────────────────────────────────────── */
static TabInfo *create_new_tab(const char *initial_url);
static void     start_loading_url(TabInfo *tab, const char *url);
static void     on_go_clicked(GtkButton *btn, gpointer data);
static void     on_reload_clicked(GtkButton *btn, gpointer data);
static void     on_close_tab_clicked(GtkButton *btn, gpointer data);
static void     on_history_visit_clicked(GtkButton *btn, gpointer data);

/* ================================================================
 *  SECTION 1 — UTILITY FUNCTIONS
 * ================================================================ */

/*
 * parse_url()
 * Splits a URL into hostname and path.
 * Handles:  http://host/path   https://host/path   host/path   host
 */
static void parse_url(const char *url,
                      char *hostname, size_t hlen,
                      char *path,     size_t plen)
{
    const char *p = url;
    if      (strncmp(p, "http://",  7) == 0)  p += 7;
    else if (strncmp(p, "https://", 8) == 0)  p += 8;

    const char *slash = strchr(p, '/');
    if (slash) {
        size_t n = (size_t)(slash - p);
        if (n >= hlen) n = hlen - 1;
        strncpy(hostname, p, n);
        hostname[n] = '\0';
        strncpy(path, slash, plen - 1);
        path[plen - 1] = '\0';
    } else {
        strncpy(hostname, p, hlen - 1);
        hostname[hlen - 1] = '\0';
        strncpy(path, "/", plen - 1);
    }
}

/*
 * url_to_filename()
 * Converts a URL into a safe filename string (strips scheme,
 * replaces special chars with '_', appends ".html").
 */
static void url_to_filename(const char *url, char *out, size_t max)
{
    size_t i = 0, j = 0;
    if      (strncmp(url, "http://",  7) == 0) i = 7;
    else if (strncmp(url, "https://", 8) == 0) i = 8;

    for (; url[i] && j < max - 6; i++, j++) {
        char c = url[i];
        out[j] = (isalnum((unsigned char)c) || c == '-' || c == '.') ? c : '_';
    }
    strncpy(out + j, ".html", max - j - 1);
    out[max - 1] = '\0';
}

/* Strip trailing whitespace / newline in-place */
static void trim_right(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' ||
                     s[n-1] == ' '  || s[n-1] == '\t'))
        s[--n] = '\0';
}

/* ================================================================
 *  SECTION 2 — SIGNAL HANDLER  (OS Concept: Signal Handling)
 * ================================================================ */

/*
 * sigchld_handler()
 * Reaps all finished child processes (tab fetchers) to prevent
 * zombie accumulation.  Uses WNOHANG so it never blocks.
 */
static void sigchld_handler(int sig)
{
    (void)sig;
    /* waitpid is async-signal-safe */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

/* ================================================================
 *  SECTION 3 — SETUP
 * ================================================================ */

static void setup_directories(void)
{
    struct stat st = {0};
    if (stat(CACHE_DIR, &st) == -1)
        mkdir(CACHE_DIR, 0755);

    /* Ensure flat files exist */
    FILE *f;
    f = fopen(HISTORY_FILE,  "a"); if (f) fclose(f);
    f = fopen(CACHE_MAP_FILE,"a"); if (f) fclose(f);

    /* Clean up leftover SHM objects / semaphores from any prior crash */
    char name[32];
    for (int i = 0; i < MAX_TABS; i++) {
        snprintf(name, sizeof(name), "/WBD_%d", i); shm_unlink(name);
        snprintf(name, sizeof(name), "/WBS_%d", i); sem_unlink(name);
    }
}

/* ================================================================
 *  SECTION 4 — CACHE  (OS Concept: Reader-Writer Lock)
 *
 *  Multiple tabs may check the cache simultaneously (read-lock).
 *  Only one tab at a time may update the cache map (write-lock).
 * ================================================================ */

/* load_cache_map() — must be called once at startup */
static void load_cache_map(void)
{
    pthread_rwlock_wrlock(&g_cache_rwlock);

    free(g_cache);
    g_cache = (CacheEntry *)calloc(MAX_CACHE_ENTRIES, sizeof(CacheEntry));
    g_cache_count = 0;

    if (g_cache) {
        FILE *fp = fopen(CACHE_MAP_FILE, "r");
        if (fp) {
            while (g_cache_count < MAX_CACHE_ENTRIES &&
                   fscanf(fp, "%511s %255s\n",
                          g_cache[g_cache_count].url,
                          g_cache[g_cache_count].filename) == 2)
                g_cache_count++;
            fclose(fp);
        }
    }

    pthread_rwlock_unlock(&g_cache_rwlock);
}

/* save_cache_map_locked() — caller MUST hold the write-lock */
static void save_cache_map_locked(void)
{
    FILE *fp = fopen(CACHE_MAP_FILE, "w");
    if (!fp) return;
    for (int i = 0; i < g_cache_count; i++)
        fprintf(fp, "%s %s\n", g_cache[i].url, g_cache[i].filename);
    fclose(fp);
}

/*
 * cache_lookup()
 * Returns 0 (hit) and fills buf with file contents, or -1 (miss).
 * Acquires read-lock for the map scan; upgrades to write-lock to
 * perform LRU promotion.
 */
static int cache_lookup(const char *url, char *buf, size_t max)
{
    /* --- Phase 1: read-lock scan --- */
    pthread_rwlock_rdlock(&g_cache_rwlock);

    int found = -1;
    char filepath[512];
    for (int i = 0; i < g_cache_count; i++) {
        if (strcmp(g_cache[i].url, url) == 0) {
            snprintf(filepath, sizeof(filepath), "%s/%s",
                     CACHE_DIR, g_cache[i].filename);
            found = i;
            break;
        }
    }

    pthread_rwlock_unlock(&g_cache_rwlock);

    if (found == -1) return -1;     /* cache miss */

    /* --- Phase 2: read the cached file (no lock needed) --- */
    FILE *f = fopen(filepath, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, max - 1, f);
    buf[n] = '\0';
    fclose(f);

    /* --- Phase 3: write-lock LRU promotion --- */
    pthread_rwlock_wrlock(&g_cache_rwlock);
    CacheEntry hit = g_cache[found];
    memmove(&g_cache[1], &g_cache[0], (size_t)found * sizeof(CacheEntry));
    g_cache[0] = hit;
    save_cache_map_locked();
    pthread_rwlock_unlock(&g_cache_rwlock);

    return 0;   /* cache hit */
}

/*
 * cache_add_entry()
 * Inserts a new URL→filename mapping at the front (MRU position),
 * evicting the oldest entry if the cache is full.
 */
static void cache_add_entry(const char *url, const char *filename)
{
    pthread_rwlock_wrlock(&g_cache_rwlock);

    if (g_cache_count >= MAX_CACHE_ENTRIES)
        g_cache_count = MAX_CACHE_ENTRIES - 1;

    memmove(&g_cache[1], &g_cache[0],
            (size_t)g_cache_count * sizeof(CacheEntry));

    strncpy(g_cache[0].url,      url,      511); g_cache[0].url[511]      = '\0';
    strncpy(g_cache[0].filename, filename, 255); g_cache[0].filename[255] = '\0';
    g_cache_count++;

    save_cache_map_locked();
    pthread_rwlock_unlock(&g_cache_rwlock);
}

/* ================================================================
 *  SECTION 5 — HISTORY  (OS Concepts: Mutex, pthreads)
 *
 *  History writes run in detached pthreads so the GUI never stalls.
 *  The mutex ensures that concurrent writes don't corrupt the file.
 * ================================================================ */

/*
 * history_writer_thread()
 * Prepends a URL to History.txt.  Protected by g_history_mutex.
 * The arg is a heap-allocated strdup of the URL; this function
 * frees it.
 */
static void *history_writer_thread(void *arg)
{
    char *url = (char *)arg;

    pthread_mutex_lock(&g_history_mutex);

    /* Read existing content */
    FILE *fp = fopen(HISTORY_FILE, "r");
    char *old = NULL;
    long  fsz = 0;
    if (fp) {
        fseek(fp, 0, SEEK_END);
        fsz = ftell(fp);
        rewind(fp);
        old = (char *)malloc((size_t)fsz + 2);
        if (old) { fread(old, 1, (size_t)fsz, fp); old[fsz] = '\0'; }
        fclose(fp);
    }

    /* Overwrite: new URL first, then old content */
    fp = fopen(HISTORY_FILE, "w");
    if (fp) {
        fprintf(fp, "%s\n", url);
        if (old) fprintf(fp, "%s", old);
        fclose(fp);
    }
    free(old);

    pthread_mutex_unlock(&g_history_mutex);
    free(url);
    return NULL;
}

/*
 * write_to_history()
 * Spawns a detached pthread to record the URL.  Returns immediately;
 * the GUI is never blocked.
 */
static void write_to_history(const char *url)
{
    char *copy = strdup(url);
    if (!copy) return;

    pthread_t tid;
    if (pthread_create(&tid, NULL, history_writer_thread, copy) == 0)
        pthread_detach(tid);
    else
        free(copy);
}

/*
 * load_history()
 * Reads History.txt into the g_history array under the mutex.
 * Called from the GUI thread when the History dialog is opened.
 */
static void load_history(void)
{
    pthread_mutex_lock(&g_history_mutex);

    free(g_history);
    g_history = (HistoryEntry *)calloc(MAX_HISTORY, sizeof(HistoryEntry));
    g_history_count = 0;

    if (g_history) {
        FILE *fp = fopen(HISTORY_FILE, "r");
        if (fp) {
            char line[512];
            while (g_history_count < MAX_HISTORY &&
                   fgets(line, sizeof(line), fp)) {
                line[strcspn(line, "\n")] = '\0';
                if (line[0]) {
                    strncpy(g_history[g_history_count].url, line, 511);
                    g_history_count++;
                }
            }
            fclose(fp);
        }
    }

    pthread_mutex_unlock(&g_history_mutex);
}

/* ================================================================
 *  SECTION 6 — NETWORK FETCH  (Runs in child process only!)
 *
 *  OS Concepts: fork(), Pipes, Shared Memory, Semaphores,
 *               TCP Sockets, gethostbyname()
 *
 *  This function is called after fork().  It MUST NOT call any
 *  GTK functions and MUST exit() — never return.
 * ================================================================ */

/*
 * child_fetch_url()
 * The complete fetch logic running in the child process:
 *   1. DNS resolution via gethostbyname()
 *   2. TCP connect to port 80
 *   3. HTTP/1.1 GET request
 *   4. Write response into POSIX Shared Memory (shm_open + mmap)
 *   5. Write response to disk cache file
 *   6. Post the per-tab named semaphore to wake the parent
 *
 * The anonymous pipe is used early:
 *   'O'  → DNS + connect succeeded (parent can start its spinner)
 *   'E'  → fatal error before any data
 */
static void child_fetch_url(const char *url,
                             const char *shm_name,
                             const char *sem_name,
                             int         pipe_wr,
                             const char *cache_filename)
{
    char hostname[256] = {0};
    char path[512]     = {0};
    parse_url(url, hostname, sizeof(hostname), path, sizeof(path));

    /* ── DNS Resolution ─────────────────────────────────────── */
    struct hostent *he = gethostbyname(hostname);
    if (!he) {
        fprintf(stderr, "[child %d] DNS failed for '%s': %s\n",
                getpid(), hostname, hstrerror(h_errno));
        write(pipe_wr, "E", 1);
        close(pipe_wr);
        exit(EXIT_FAILURE);
    }

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, he->h_addr_list[0], ip, sizeof(ip));
    printf("[child %d] %s → %s\n", getpid(), hostname, ip);

    /* ── TCP Socket ─────────────────────────────────────────── */
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        write(pipe_wr, "E", 1); close(pipe_wr); exit(EXIT_FAILURE);
    }

    /* 10-second receive timeout so we don't hang forever */
    struct timeval tv = {10, 0};
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port   = htons(HTTP_PORT);
    inet_pton(AF_INET, ip, &srv.sin_addr);

    if (connect(sockfd, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        fprintf(stderr, "[child %d] connect failed: %s\n",
                getpid(), strerror(errno));
        close(sockfd);
        write(pipe_wr, "E", 1); close(pipe_wr); exit(EXIT_FAILURE);
    }

    /* Signal parent: DNS + connect succeeded */
    write(pipe_wr, "O", 1);
    printf("[child %d] Connected. Sending HTTP request.\n", getpid());

    /* ── HTTP/1.1 GET Request ───────────────────────────────── */
    char request[1024];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Connection: close\r\n"
             "User-Agent: OSBrowser/1.0\r\n"
             "Accept: text/html,text/plain,*/*\r\n\r\n",
             path, hostname);
    send(sockfd, request, strlen(request), 0);

    /* ── Open Shared Memory ─────────────────────────────────── */
    int shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1 || ftruncate(shm_fd, SHM_SIZE) == -1) {
        perror("[child] shm_open/ftruncate");
        close(sockfd); close(pipe_wr); exit(EXIT_FAILURE);
    }
    char *shm_ptr = (char *)mmap(NULL, SHM_SIZE,
                                 PROT_READ | PROT_WRITE,
                                 MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        perror("[child] mmap"); close(shm_fd);
        close(sockfd); close(pipe_wr); exit(EXIT_FAILURE);
    }
    memset(shm_ptr, 0, SHM_SIZE);

    /* ── Receive Response + write to SHM + cache file ──────── */
    char cache_path[600];
    snprintf(cache_path, sizeof(cache_path), "%s/%s",
             CACHE_DIR, cache_filename);
    FILE *cf = fopen(cache_path, "w");

    char   recv_buf[8192];
    ssize_t n;
    size_t  total = 0;

    while ((n = recv(sockfd, recv_buf, sizeof(recv_buf) - 1, 0)) > 0) {
        recv_buf[n] = '\0';
        if (cf) fwrite(recv_buf, 1, (size_t)n, cf);
        if (total + (size_t)n < (size_t)(SHM_SIZE - 1)) {
            memcpy(shm_ptr + total, recv_buf, (size_t)n);
            total += (size_t)n;
        }
    }
    shm_ptr[total] = '\0';
    printf("[child %d] Received %zu bytes.\n", getpid(), total);

    if (cf) fclose(cf);
    close(sockfd);

    /* Unmap (data persists in the named SHM object) */
    munmap(shm_ptr, SHM_SIZE);
    close(shm_fd);

    /* ── Post Semaphore — wake the parent ───────────────────── */
    sem_t *sem = sem_open(sem_name, O_CREAT, 0666, 0);
    if (sem == SEM_FAILED) {
        perror("[child] sem_open");
        close(pipe_wr); exit(EXIT_FAILURE);
    }
    sem_post(sem);
    sem_close(sem);

    close(pipe_wr);
    exit(EXIT_SUCCESS);
}

/* ================================================================
 *  SECTION 7 — DISPLAY HELPERS
 * ================================================================ */

static void set_tab_status(TabInfo *tab, const char *msg)
{
    if (tab && tab->active && tab->status_label)
        gtk_label_set_text(GTK_LABEL(tab->status_label), msg);
}

static void display_content(TabInfo *tab, const char *text)
{
    if (!tab || !tab->active || !tab->content_view) return;
    GtkTextBuffer *buf =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(tab->content_view));
    gtk_text_buffer_set_text(buf, text ? text : "", -1);
    if (tab->spinner) gtk_spinner_stop(GTK_SPINNER(tab->spinner));
    tab->loading = false;
}

static void update_tab_label_hostname(TabInfo *tab)
{
    if (!tab->tab_label_box) return;
    GList *ch = gtk_container_get_children(
                    GTK_CONTAINER(tab->tab_label_box));
    if (!ch) return;
    char hostname[64] = {0}, path[512] = {0};
    parse_url(tab->url, hostname, sizeof(hostname), path, sizeof(path));
    char title[20];
    snprintf(title, sizeof(title), "%.18s",
             hostname[0] ? hostname : tab->url);
    gtk_label_set_text(GTK_LABEL(GTK_WIDGET(ch->data)), title);
    g_list_free(ch);
}

/* ================================================================
 *  SECTION 8 — PERIODIC POLL  (GLib timer, GTK main loop)
 *
 *  Runs every POLL_INTERVAL_MS ms in the GUI thread.
 *  For each loading tab it:
 *    1. Tries a non-blocking pipe read for an early error byte.
 *    2. Tries sem_trywait (non-blocking) for the content semaphore.
 *    3. If the semaphore fires, reads the SHM and displays content.
 * ================================================================ */

static gboolean check_loading_tabs(gpointer data)
{
    (void)data;

    for (int i = 0; i < MAX_TABS; i++) {
        TabInfo *tab = &g_tabs[i];
        if (!tab->active || !tab->loading) continue;

        /* ── Step 1: check pipe for early error ──────────────── */
        if (tab->pipe_status == 0 && tab->pipe_rd >= 0) {
            /* Temporarily set non-blocking */
            int flags = fcntl(tab->pipe_rd, F_GETFL, 0);
            fcntl(tab->pipe_rd, F_SETFL, flags | O_NONBLOCK);
            char byte = 0;
            ssize_t r = read(tab->pipe_rd, &byte, 1);
            fcntl(tab->pipe_rd, F_SETFL, flags);  /* restore */

            if (r == 1) {
                tab->pipe_status = byte;
                printf("[parent] tab %d pipe status: '%c'\n", i, byte);
                if (byte == 'E') {
                    /* Child signalled a network error */
                    display_content(tab,
                        "[Network Error — Page Could Not Be Loaded]\n\n"
                        "Possible causes:\n"
                        "  • Hostname could not be resolved\n"
                        "    (check spelling, or no DNS/network)\n"
                        "  • Server refused connection on port 80\n"
                        "  • The site uses HTTPS only — this browser\n"
                        "    supports plain HTTP (port 80) only.\n"
                        "  • Connection timed out (10-second limit)\n\n"
                        "Try: neverssl.com  httpforever.com  info.cern.ch");
                    set_tab_status(tab, "Error – could not connect");
                    close(tab->pipe_rd);
                    tab->pipe_rd = -1;
                    sem_unlink(tab->sem_name);
                    shm_unlink(tab->shm_name);
                    tab->loading = false;
                    continue;
                }
                /* byte == 'O' means DNS+connect OK, still fetching */
                set_tab_status(tab, "Fetching data...");
            }
        }

        /* ── Step 2: try semaphore (non-blocking) ─────────────── */
        sem_t *sem = sem_open(tab->sem_name, O_CREAT, 0666, 0);
        if (sem == SEM_FAILED) continue;

        if (sem_trywait(sem) == 0) {
            /* Semaphore posted — content is ready in SHM */
            sem_close(sem);
            sem_unlink(tab->sem_name);

            /* Read from shared memory */
            int shm_fd = shm_open(tab->shm_name, O_RDONLY, 0666);
            if (shm_fd != -1) {
                char *ptr = (char *)mmap(NULL, SHM_SIZE, PROT_READ,
                                         MAP_SHARED, shm_fd, 0);
                if (ptr != MAP_FAILED) {
                    display_content(tab, ptr);
                    munmap(ptr, SHM_SIZE);
                }
                close(shm_fd);
                shm_unlink(tab->shm_name);
            }

            /* Update cache map (write-lock inside cache_add_entry) */
            char fname[256];
            url_to_filename(tab->url, fname, sizeof(fname));
            cache_add_entry(tab->url, fname);

            /* Write history in a background thread (non-blocking) */
            write_to_history(tab->url);

            set_tab_status(tab, "Loaded");
            update_tab_label_hostname(tab);

            if (tab->pipe_rd >= 0) {
                close(tab->pipe_rd);
                tab->pipe_rd = -1;
            }
        } else {
            sem_close(sem);   /* still waiting — come back next poll */
        }
    }

    return G_SOURCE_CONTINUE;   /* keep timer alive */
}

/* ================================================================
 *  SECTION 9 — TAB MANAGEMENT  (OS Concept: fork per tab)
 * ================================================================ */

/*
 * start_loading_url()
 * 1. Checks the cache (Reader-Writer lock).
 * 2. On miss: fork()s a child process to fetch the page.
 *    The child writes its status to a pipe and its content
 *    to shared memory, then posts a named semaphore.
 */
static void start_loading_url(TabInfo *tab, const char *url)
{
    if (!url || !url[0] || !tab || !tab->active) return;

    /* Clean the URL */
    char clean[512];
    strncpy(clean, url, sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';
    trim_right(clean);

    strncpy(tab->url, clean, sizeof(tab->url) - 1);
    gtk_entry_set_text(GTK_ENTRY(tab->url_entry), clean);
    tab->pipe_status = 0;

    /* Update tab label immediately */
    GList *ch = gtk_container_get_children(
                    GTK_CONTAINER(tab->tab_label_box));
    if (ch) {
        char hostname[64] = {0}, path[512] = {0};
        parse_url(clean, hostname, sizeof(hostname), path, sizeof(path));
        char title[22];
        snprintf(title, sizeof(title), "%.20s",
                 hostname[0] ? hostname : clean);
        gtk_label_set_text(GTK_LABEL(GTK_WIDGET(ch->data)), title);
        g_list_free(ch);
    }

    /* ── Cache lookup (acquires read-lock inside) ─────────────── */
    char *cbuf = (char *)malloc(SHM_SIZE);
    if (cbuf) {
        if (cache_lookup(clean, cbuf, SHM_SIZE) == 0) {
            display_content(tab, cbuf);
            set_tab_status(tab, "Loaded  (cache hit ✓)");
            write_to_history(clean);
            free(cbuf);
            return;
        }
        free(cbuf);
    }

    /* ── Cache miss: fork a child process ─────────────────────── */
    set_tab_status(tab, "Resolving hostname...");
    if (tab->spinner) gtk_spinner_start(GTK_SPINNER(tab->spinner));

    /* Close any previous pipe fds */
    if (tab->pipe_rd >= 0) { close(tab->pipe_rd); tab->pipe_rd = -1; }
    if (tab->pipe_wr >= 0) { close(tab->pipe_wr); tab->pipe_wr = -1; }

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        set_tab_status(tab, "Error: pipe() failed");
        if (tab->spinner) gtk_spinner_stop(GTK_SPINNER(tab->spinner));
        return;
    }
    tab->pipe_rd = pipefd[0];   /* parent reads  */
    tab->pipe_wr = pipefd[1];   /* child writes  */

    char cache_fname[256];
    url_to_filename(clean, cache_fname, sizeof(cache_fname));

    /* ── fork() ── */
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        set_tab_status(tab, "Error: fork() failed");
        if (tab->spinner) gtk_spinner_stop(GTK_SPINNER(tab->spinner));
        close(tab->pipe_rd); tab->pipe_rd = -1;
        close(tab->pipe_wr); tab->pipe_wr = -1;
        return;
    }

    if (pid == 0) {
        /* ── CHILD PROCESS ─────────────────────────────────────
         * Close the read end — child only writes to the pipe.
         * Then fetch the URL (this function calls exit()). */
        close(tab->pipe_rd);
        child_fetch_url(clean,
                        tab->shm_name,
                        tab->sem_name,
                        tab->pipe_wr,
                        cache_fname);
        /* never reaches here */
    }

    /* ── PARENT continues ─────────────────────────────────────── */
    close(tab->pipe_wr);    /* parent does not write */
    tab->pipe_wr   = -1;
    tab->child_pid = pid;
    tab->loading   = true;

    printf("[parent] Forked child PID %d for tab %d (%s)\n",
           pid, tab->index, clean);
}

/*
 * create_new_tab()
 * Allocates a tab slot, builds the GTK widgets, adds the page to
 * the GtkNotebook, and optionally starts loading an initial URL.
 */
static TabInfo *create_new_tab(const char *initial_url)
{
    /* Find a free slot */
    int idx = -1;
    for (int i = 0; i < MAX_TABS; i++) {
        if (!g_tabs[i].active) { idx = i; break; }
    }
    if (idx == -1) {
        fprintf(stderr, "Maximum tabs (%d) reached.\n", MAX_TABS);
        return NULL;
    }

    TabInfo *tab = &g_tabs[idx];
    memset(tab, 0, sizeof(TabInfo));
    tab->index    = idx;
    tab->active   = true;
    tab->pipe_rd  = -1;
    tab->pipe_wr  = -1;
    snprintf(tab->shm_name, sizeof(tab->shm_name), "/WBD_%d", idx);
    snprintf(tab->sem_name, sizeof(tab->sem_name), "/WBS_%d", idx);

    /* ──────────────────────────────────────────────────────────
     * Build the tab page:
     *
     *  VBox (notebook_page)
     *    HBox  [url_entry] [Go] [Reload]
     *    HSeparator
     *    HBox  [spinner] [status_label]
     *    ScrolledWindow
     *      TextView (content_view)
     * ────────────────────────────────────────────────────────── */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    /* URL bar */
    GtkWidget *url_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start (url_bar, 6);
    gtk_widget_set_margin_end   (url_bar, 6);
    gtk_widget_set_margin_top   (url_bar, 4);
    gtk_widget_set_margin_bottom(url_bar, 4);

    tab->url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(tab->url_entry),
        "Enter URL — e.g.  neverssl.com  or  http://info.cern.ch");
    gtk_widget_set_hexpand(tab->url_entry, TRUE);
    if (initial_url && initial_url[0])
        gtk_entry_set_text(GTK_ENTRY(tab->url_entry), initial_url);

    GtkWidget *go_btn     = gtk_button_new_with_label("  Go  ");
    GtkWidget *reload_btn = gtk_button_new_with_label("↺");
    gtk_widget_set_tooltip_text(reload_btn, "Reload this page");

    gtk_box_pack_start(GTK_BOX(url_bar), tab->url_entry, TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(url_bar), go_btn,         FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(url_bar), reload_btn,     FALSE, FALSE, 0);

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);

    /* Status bar */
    GtkWidget *status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(status_bar, 6);
    gtk_widget_set_margin_top  (status_bar, 2);

    tab->spinner = gtk_spinner_new();
    tab->status_label = gtk_label_new(
        "Ready — type a URL above and press Enter or click Go");
    gtk_label_set_xalign(GTK_LABEL(tab->status_label), 0.0f);

    gtk_box_pack_start(GTK_BOX(status_bar), tab->spinner,      FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(status_bar), tab->status_label, TRUE,  TRUE,  0);

    /* Content view */
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);

    tab->content_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tab->content_view),
                                GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tab->content_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tab->content_view), FALSE);
    gtk_text_view_set_left_margin (GTK_TEXT_VIEW(tab->content_view), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(tab->content_view), 8);
    gtk_text_view_set_top_margin  (GTK_TEXT_VIEW(tab->content_view), 6);

    /* Use monospace font via CSS to avoid deprecation warning */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "textview { font-family: Monospace; font-size: 10pt; }",
        -1, NULL);
    gtk_style_context_add_provider(
        gtk_widget_get_style_context(tab->content_view),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    gtk_container_add(GTK_CONTAINER(scroll), tab->content_view);

    gtk_box_pack_start(GTK_BOX(vbox), url_bar,    FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), sep,        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), status_bar, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(vbox), scroll,     TRUE,  TRUE,  0);

    tab->notebook_page = vbox;

    /* ── Tab label widget (shown in the notebook tab strip) ──── */
    tab->tab_label_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

    GtkWidget *tab_text = gtk_label_new("New Tab");
    gtk_label_set_width_chars(GTK_LABEL(tab_text), 12);
    gtk_label_set_ellipsize  (GTK_LABEL(tab_text), PANGO_ELLIPSIZE_END);

    GtkWidget *close_btn = gtk_button_new_with_label("×");
    gtk_button_set_relief(GTK_BUTTON(close_btn), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(close_btn, FALSE);

    /* Compact close button via CSS */
    GtkCssProvider *btn_css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(btn_css,
        "button { padding: 0 3px; min-width: 16px; min-height: 16px; }",
        -1, NULL);
    gtk_style_context_add_provider(
        gtk_widget_get_style_context(close_btn),
        GTK_STYLE_PROVIDER(btn_css),
        GTK_STYLE_PROVIDER_PRIORITY_USER);
    g_object_unref(btn_css);

    gtk_box_pack_start(GTK_BOX(tab->tab_label_box), tab_text,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tab->tab_label_box), close_btn, FALSE, FALSE, 0);
    gtk_widget_show_all(tab->tab_label_box);

    /* ── Append to GtkNotebook and switch to it ─────────────── */
    gtk_notebook_append_page(GTK_NOTEBOOK(g_notebook),
                             vbox,
                             tab->tab_label_box);
    gtk_widget_show_all(vbox);

    int pg = gtk_notebook_page_num(GTK_NOTEBOOK(g_notebook), vbox);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), pg);

    /* ── Connect GTK Signals ─────────────────────────────────── */
    g_signal_connect(go_btn,         "clicked",  G_CALLBACK(on_go_clicked),        tab);
    g_signal_connect(reload_btn,     "clicked",  G_CALLBACK(on_reload_clicked),    tab);
    g_signal_connect(close_btn,      "clicked",  G_CALLBACK(on_close_tab_clicked), tab);
    /* The "activate" signal fires when the user presses Enter */
    g_signal_connect(tab->url_entry, "activate", G_CALLBACK(on_go_clicked),        tab);

    /* Auto-load if an initial URL was supplied */
    if (initial_url && initial_url[0])
        start_loading_url(tab, initial_url);

    printf("[parent] Created tab slot %d\n", idx);
    return tab;
}

/* ================================================================
 *  SECTION 10 — GTK CALLBACKS
 * ================================================================ */

static void on_go_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    TabInfo *tab = (TabInfo *)data;
    const char *url = gtk_entry_get_text(GTK_ENTRY(tab->url_entry));
    if (url && url[0])
        start_loading_url(tab, url);
}

static void on_reload_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    TabInfo *tab = (TabInfo *)data;
    if (tab->url[0])
        start_loading_url(tab, tab->url);
}

static void on_close_tab_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    TabInfo *tab = (TabInfo *)data;

    /* Stop any in-progress fetch */
    tab->loading = false;
    if (tab->child_pid > 0) {
        kill(tab->child_pid, SIGTERM);
        waitpid(tab->child_pid, NULL, WNOHANG);
        tab->child_pid = 0;
    }

    /* Clean up IPC resources */
    shm_unlink(tab->shm_name);
    sem_unlink(tab->sem_name);
    if (tab->pipe_rd >= 0) { close(tab->pipe_rd); tab->pipe_rd = -1; }
    if (tab->pipe_wr >= 0) { close(tab->pipe_wr); tab->pipe_wr = -1; }

    /* Remove the page from the notebook before marking slot free */
    GtkWidget *page = tab->notebook_page;
    tab->active         = false;
    tab->notebook_page  = NULL;
    tab->content_view   = NULL;
    tab->url_entry      = NULL;
    tab->status_label   = NULL;
    tab->tab_label_box  = NULL;
    tab->spinner        = NULL;

    int pg = gtk_notebook_page_num(GTK_NOTEBOOK(g_notebook), page);
    if (pg >= 0)
        gtk_notebook_remove_page(GTK_NOTEBOOK(g_notebook), pg);

    printf("[parent] Closed tab slot %d\n", tab->index);

    /* Always keep at least one tab open */
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook)) == 0)
        create_new_tab(NULL);
}

static void on_new_tab_clicked(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;
    create_new_tab(NULL);
}

/*
 * on_history_visit_clicked()
 * Called when the user clicks "Open" next to a history entry.
 * Opens the URL in a new tab and closes the dialog.
 */
static void on_history_visit_clicked(GtkButton *btn, gpointer data)
{
    GtkWidget  *dialog = GTK_WIDGET(data);
    const char *url    = (const char *)g_object_get_data(G_OBJECT(btn), "url");
    if (url) create_new_tab(url);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE);
}

/*
 * on_history_clicked()
 * Loads history under the mutex, then shows a modal dialog with
 * a scrollable list.  Each row has an "Open" button that opens
 * the URL in a new tab.
 */
static void on_history_clicked(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;

    load_history();   /* acquires mutex internally */

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Browsing History",
        GTK_WINDOW(g_window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Close", GTK_RESPONSE_CLOSE,
        NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 580, 440);

    GtkWidget *ca  = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *scr = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scr),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scr, TRUE);
    gtk_widget_set_hexpand(scr, TRUE);
    gtk_widget_set_size_request(scr, 560, 380);
    gtk_box_pack_start(GTK_BOX(ca), scr, TRUE, TRUE, 0);

    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_container_add(GTK_CONTAINER(scr), list);

    if (g_history_count == 0) {
        GtkWidget *lbl = gtk_label_new("No browsing history yet.");
        gtk_widget_set_margin_top(lbl, 24);
        gtk_container_add(GTK_CONTAINER(list), lbl);
    } else {
        for (int i = 0; i < g_history_count; i++) {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_widget_set_margin_start (row, 10);
            gtk_widget_set_margin_end   (row, 10);
            gtk_widget_set_margin_top   (row,  4);
            gtk_widget_set_margin_bottom(row,  4);

            char num[8];
            snprintf(num, sizeof(num), "%d.", i + 1);
            GtkWidget *num_lbl = gtk_label_new(num);
            gtk_widget_set_size_request(num_lbl, 32, -1);

            GtkWidget *url_lbl = gtk_label_new(g_history[i].url);
            gtk_label_set_xalign   (GTK_LABEL(url_lbl), 0.0f);
            gtk_label_set_ellipsize(GTK_LABEL(url_lbl), PANGO_ELLIPSIZE_END);
            gtk_widget_set_hexpand (url_lbl, TRUE);

            GtkWidget *open_btn = gtk_button_new_with_label("Open →");
            g_object_set_data_full(G_OBJECT(open_btn), "url",
                                   g_strdup(g_history[i].url), g_free);
            g_signal_connect(open_btn, "clicked",
                             G_CALLBACK(on_history_visit_clicked), dialog);

            gtk_box_pack_start(GTK_BOX(row), num_lbl,  FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(row), url_lbl,  TRUE,  TRUE,  0);
            gtk_box_pack_start(GTK_BOX(row), open_btn, FALSE, FALSE, 0);
            gtk_container_add(GTK_CONTAINER(list), row);
        }
    }

    gtk_widget_show_all(ca);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

/*
 * on_window_delete()
 * Called when the user clicks the window's × button.
 * Terminates all child processes and exits cleanly.
 */
static gboolean on_window_delete(GtkWidget *widget,
                                  GdkEvent  *event,
                                  gpointer   data)
{
    (void)widget; (void)event; (void)data;

    printf("[parent] Shutting down — killing child processes...\n");
    for (int i = 0; i < MAX_TABS; i++) {
        if (g_tabs[i].active && g_tabs[i].child_pid > 0) {
            kill(g_tabs[i].child_pid, SIGTERM);
            waitpid(g_tabs[i].child_pid, NULL, 0);
        }
    }
    gtk_main_quit();
    return FALSE;
}

/* ================================================================
 *  SECTION 11 — MAIN
 * ================================================================ */

int main(int argc, char *argv[])
{
    printf("=== OS Text Browser starting ===\n");

    /* ── OS Concept: Signal Handling ─────────────────────────── */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction"); return EXIT_FAILURE;
    }
    printf("[parent] SIGCHLD handler installed.\n");

    /* ── Filesystem setup ─────────────────────────────────────── */
    setup_directories();
    printf("[parent] Directories ready. Loading cache map...\n");

    /* ── Load cache under write-lock ─────────────────────────── */
    load_cache_map();
    printf("[parent] Cache: %d entries loaded.\n", g_cache_count);

    /* ── Initialise tab array ─────────────────────────────────── */
    memset(g_tabs, 0, sizeof(g_tabs));

    /* ── GTK Initialisation ───────────────────────────────────── */
    gtk_init(&argc, &argv);

    /* Main window */
    g_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_window),
                         "OS Text Browser  —  HTTP/1.1  (no HTTPS)");
    gtk_window_set_default_size(GTK_WINDOW(g_window), 1000, 680);
    g_signal_connect(g_window, "delete-event",
                     G_CALLBACK(on_window_delete), NULL);

    /* Root vertical box */
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(g_window), root);

    /* Top toolbar */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start (toolbar, 8);
    gtk_widget_set_margin_end   (toolbar, 8);
    gtk_widget_set_margin_top   (toolbar, 6);
    gtk_widget_set_margin_bottom(toolbar, 6);

    GtkWidget *new_tab_btn = gtk_button_new_with_label("＋ New Tab");
    GtkWidget *history_btn = gtk_button_new_with_label("📜 History");

    gtk_box_pack_start(GTK_BOX(toolbar), new_tab_btn, FALSE, FALSE, 0);
    gtk_box_pack_end  (GTK_BOX(toolbar), history_btn, FALSE, FALSE, 0);

    GtkWidget *toolbar_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);

    gtk_box_pack_start(GTK_BOX(root), toolbar,     FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), toolbar_sep, FALSE, FALSE, 0);

    g_signal_connect(new_tab_btn, "clicked", G_CALLBACK(on_new_tab_clicked), NULL);
    g_signal_connect(history_btn, "clicked", G_CALLBACK(on_history_clicked), NULL);

    /* GtkNotebook — one page per tab */
    g_notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(g_notebook), TRUE);
    gtk_widget_set_vexpand(g_notebook, TRUE);
    gtk_box_pack_start(GTK_BOX(root), g_notebook, TRUE, TRUE, 0);

    /* Open the initial blank tab */
    create_new_tab(NULL);

    /* Start the periodic polling timer */
    g_timeout_add(POLL_INTERVAL_MS, check_loading_tabs, NULL);

    gtk_widget_show_all(g_window);
    printf("[parent] GUI ready. Entering GTK main loop.\n");
    gtk_main();

    /* ── Cleanup ─────────────────────────────────────────────── */
    pthread_rwlock_destroy(&g_cache_rwlock);
    pthread_mutex_destroy (&g_history_mutex);
    free(g_cache);
    free(g_history);

    printf("[parent] Exited cleanly.\n");
    return EXIT_SUCCESS;
}
