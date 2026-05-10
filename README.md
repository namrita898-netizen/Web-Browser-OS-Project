# 🌐 OS Text Browser

> A multi-process, multi-threaded web browser simulation built in C — demonstrating core Operating Systems concepts through a real, working application.

---

## 📌 Table of Contents

- [About the Project](#about-the-project)
- [OS Concepts Implemented](#os-concepts-implemented)
- [Features](#features)
- [Project Structure](#project-structure)
- [Prerequisites](#prerequisites)
- [Installation & Setup](#installation--setup)
- [How to Run](#how-to-run)
- [How to Use](#how-to-use)
- [Test URLs](#test-urls)
- [Architecture Overview](#architecture-overview)
- [Limitations](#limitations)
- [Authors](#authors)

---

## About the Project

**OS Text Browser** is a terminal-launched, GUI-based web browser written entirely in **C**, built as a university Operating Systems project. It simulates how a real browser isolates tabs, fetches pages, and manages concurrency — using nothing but standard POSIX system calls and the GTK3 toolkit.

Each browser tab is an **independent child process** (created via `fork()`). The parent process runs the GTK3 graphical interface, while child processes handle all networking. Data flows between them through **Shared Memory**, **Pipes**, and **Semaphores**. History and caching are managed with **Mutex** and **Reader-Writer** locks.

This is **not** a full browser — it displays raw HTTP responses as text. No CSS rendering, no JavaScript, no HTTPS. The focus is entirely on demonstrating OS internals.

---

## OS Concepts Implemented

| # | Concept | Mechanism Used |
|---|---------|---------------|
| 1 | **Process Management** | `fork()` per tab · `SIGCHLD` + `waitpid()` for cleanup |
| 2 | **IPC — Pipes** | Anonymous pipe per tab (status byte: `'O'` ok / `'E'` error) |
| 3 | **IPC — Shared Memory** | `shm_open` + `mmap` transfers the full HTTP response (up to 256 KB) |
| 4 | **Semaphores** | Named POSIX semaphore per tab signals the parent when data is ready |
| 5 | **Reader-Writer Lock** | `pthread_rwlock_t` protects the cache — multiple concurrent readers, exclusive writer |
| 6 | **Mutex Lock** | `pthread_mutex_t` serialises all writes to `History.txt` |
| 7 | **pthreads** | Detached worker threads handle history I/O without blocking the GUI |
| 8 | **Signal Handling** | `sigaction(SIGCHLD)` with `SA_RESTART` reaps zombie child processes |
| 9 | **Memory Management** | `malloc` / `calloc` / `free` · `mmap` / `munmap` throughout |
| 10 | **TCP Networking** | `gethostbyname()` for DNS · TCP socket · HTTP/1.1 GET with 10s timeout |

---

## Features

### 🖥️ Interface
- Multi-tab window using **GtkNotebook** (up to 20 tabs)
- Per-tab URL entry bar — type and press **Enter** or click **Go**
- **↺ Reload** button to re-fetch bypassing cache
- **＋ New Tab** button in the toolbar
- **× Close** button on every tab label
- Activity spinner while loading
- Live status line: `Resolving...` → `Fetching...` → `Loaded` / `Error`

### 📋 History
- Every successfully loaded URL is automatically saved to `History.txt`
- Newest-first ordering
- Click **📜 History** to open a scrollable dialog of all visited URLs
- **Open →** button on each entry loads it in a new tab
- Writes happen in background `pthreads` — GUI never blocks

### ⚡ Cache
- HTTP responses cached to disk in the `Cache/` directory
- Cache map stored in `CacheMap.txt` (URL ↔ filename pairs)
- **LRU eviction** — holds up to 50 entries
- Cache hits load instantly from disk — no network call made
- Status bar shows `Loaded (cache hit ✓)` vs `Loaded`
- Cache persists between runs of the program

### 🔒 Safety
- Closing a loading tab sends `SIGTERM` to the child and cleans up all IPC resources
- SIGCHLD handler prevents zombie process accumulation
- No GTK calls inside child processes or signal handlers

---

## Project Structure

```
os-text-browser/
│
├── browser.c          # Full source — single-file implementation (~700 lines)
├── Makefile           # Build, run, and clean targets
├── README.md          # This file
│
├── Cache/             # Created at runtime — stores cached HTML files
├── CacheMap.txt       # Created at runtime — URL-to-filename index
└── History.txt        # Created at runtime — browsing history (newest first)
```

> `Cache/`, `CacheMap.txt`, and `History.txt` are generated automatically on first run. Do not commit them to your repository — add them to `.gitignore`.

---

## Prerequisites

You need the following packages installed on Ubuntu/Debian:

| Package | Purpose |
|---------|---------|
| `gcc` | C compiler |
| `make` | Build system |
| `libgtk-3-dev` | GTK3 development headers and libraries |
| `libglib2.0-dev` | GLib (bundled with GTK3) |

Install all at once:

```bash
sudo apt update
sudo apt install gcc make libgtk-3-dev libglib2.0-dev -y
```

---

## Installation & Setup

**1. Clone the repository**

```bash
git clone https://github.com/your-username/os-text-browser.git
cd os-text-browser
```

**2. Build the project**

```bash
make
```

Expected output:
```
gcc -Wall -Wextra -Wno-deprecated-declarations -g browser.c -o browser ...
  Build successful!  Run with:  ./browser
```

**3. (Optional) Clean build artifacts**

```bash
make clean
```

This removes the binary and all generated runtime files (`Cache/`, `CacheMap.txt`, `History.txt`).

---

## How to Run

```bash
./browser
```

Or use the Makefile shortcut:

```bash
make run
```

The GTK3 window will open with one blank tab ready to use. A terminal window will remain open showing debug output (child PIDs, DNS results, byte counts).

---

## How to Use

| Action | How |
|--------|-----|
| Load a URL | Type in the URL bar and press **Enter**, or click **Go** |
| Open a new tab | Click **＋ New Tab** in the top toolbar |
| Close a tab | Click **×** on the tab label |
| Reload a page | Click **↺ Reload** inside the tab |
| View history | Click **📜 History** in the top toolbar |
| Revisit a history URL | Click **Open →** next to any entry in the History dialog |
| Exit | Close the window |

---

## Test URLs

This browser supports **HTTP (port 80) only**. Most modern websites force HTTPS and will return an error. Use these URLs for testing:

```
neverssl.com
httpforever.com
http://info.cern.ch
http://example.com
```

> `neverssl.com` is specifically designed to always serve over plain HTTP — perfect for testing.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                  PARENT PROCESS  (GTK3 GUI)                 │
│                                                             │
│  GtkNotebook  ──►  Tab 0  │  Tab 1  │  Tab 2  │  ...       │
│                     │          │         │                  │
│  300ms GLib timer   │  sem_trywait() on each loading tab    │
│  check_loading_tabs()                                       │
│                     │                                       │
│  pthread_rwlock_t ──► CacheMap.txt  (LRU, 50 entries)      │
│  pthread_mutex_t  ──► History.txt   (detached pthreads)    │
│                                                             │
└────────────┬────────────────────────────────────────────────┘
             │  fork() per tab load
             ▼
┌─────────────────────────────────────┐
│   CHILD PROCESS  (per tab/URL)      │
│                                     │
│  1. gethostbyname()  ── DNS         │
│  2. socket() + connect()  ── TCP    │
│  3. write('O'/'E') ──► Pipe        │
│  4. send() HTTP GET                 │
│  5. recv() response                 │
│  6. shm_open + mmap ──► SHM write  │
│  7. fwrite() ──► Cache file        │
│  8. sem_post() ──► Semaphore       │
│  9. exit()                          │
└─────────────────────────────────────┘

IPC Channels:
  Pipe        ─── 1 byte status ('O' ok, 'E' error)    [child → parent]
  Shared Mem  ─── Full HTTP response (up to 256 KB)     [child → parent]
  Semaphore   ─── "Data is ready" signal                [child → parent]
```

---

## Limitations

- **HTTP only** — HTTPS (TLS/SSL) is not supported. Sites that redirect to `https://` will fail.
- **Raw HTML display** — The text view shows the raw HTTP response including all HTML markup. CSS and JavaScript are not rendered.
- **Port 80 only** — No support for custom ports.
- **No redirects** — HTTP 301/302 redirects are not followed automatically.
- **No image loading** — Only text content is displayed.
- **Linux only** — Uses Linux-specific POSIX APIs (`shm_open`, `sem_open`, GTK3).

---

## Authors

| Name | Role |
|------|------|
| Nimrita Baby 23K-2070 | System Architecture, IPC & Synchronization, Threading |
| Wishmah Akthar 23K-0595| Testing, Debugging, Cache & History, GUI (GTK3) |

---

> **Course:** Operating Systems  
> **Platform:** Ubuntu 24.04 LTS  
> **Language:** C (C11) · GTK3 · POSIX Threads · librt
