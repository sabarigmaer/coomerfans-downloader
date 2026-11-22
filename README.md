# CoomerFans Downloader

A CLI tool for collecting post lists and downloading media (images & videos) from [coomerfans.com](https://coomerfans.com).

---

## Table of Contents
- [Features](#features)
- [Prerequisites](#prerequisites)
- [Installation](#installation)
- [How to Use](#how-to-use)
  - [1. Interactive Mode](#1-interactive-mode-recommended)
  - [2. Download File List (Option 1)](#2-download-file-list-option-1)
  - [3. Download Content (Option 2)](#3-download-content-option-2)
- [CLI Options & Flags](#cli-options--flags)
- [Output Directory Structure](#output-directory-structure)
- [Key Mechanisms](#key-mechanisms)
  - [Title Sanitization & Character Encoding](#title-sanitization--character-encoding)
  - [Automatic 4-Step Retry & Fallback](#automatic-4-step-retry--fallback)
  - [Interrupt & Resume Support](#interrupt--resume-support)
- [License](#license)

---

## Features

- **Interactive CLI & Direct Command Flags**: Run interactively with user prompts or pass flags directly for scripting/automation.
- **Option 1 (Download File List)**:
  - Traverses through all creator pages by following the "Next" button.
  - Extracts the title, post ID, post URL, and date for each post.
  - Saves the structured data page-wise in `file_list.json` (no media files downloaded).
- **Option 2 (Download Content)**:
  - Iterates through each page, opens each post, and extracts media files inside `.post-body`.
  - Downloads all media directly into the creator's directory (`downloads/<creator>/`).
  - No messy subfolders per post and no `.txt` files.
  - Formats filenames as `[<post_id>] <safe_title>.<ext>`.
- **Awkward Character Encoding**:
  - Encodes filesystem-reserved characters (`:`, `?`, `"`, `<`, `>`, `\`, `/`, `|`) into safe visual Unicode counterparts (`：`, `？`, `”`, `《`, `》`, `＼`, `⁄`, `¦`).
  - Keeps emojis (`✝️`, `🔥`), punctuation, and natural readability 100% intact without OS path errors.
- **Robust 4-Step Retry & Failover**:
  - Automatically retries 4 times with a 500ms delay.
  - Rotates through CDN subdomains (`img1` to `img10.coomerfans.com`) if the initial host fails.
  - Automatically refetches the post page to get fresh signed media tokens/hashes for expired links.
  - Records failed files in `file_list.json` and `progress.json` so they can be retried on subsequent runs.
- **Resumable State**:
  - Continuously updates `progress.json` and `file_list.json` after every single post.
  - Safely handles `Ctrl+C` / exit signals.
  - Re-running the command automatically skips already downloaded files and resumes from the exact point of failure.

---

## Prerequisites

1. **[Bun](https://bun.sh)** (v1.1+):
   ```bash
   # Install Bun (Linux/macOS)
   curl -fsSL https://bun.sh/install | bash
   ```
2. **`curl`**: Pre-installed on Linux and macOS; available by default on Windows 10/11.

---

## Installation

Clone the repository and install dependencies:

```bash
git clone https://github.com/sabarigmaer/coomerfans-downloader.git
cd coomerfans-downloader
bun install
```

---

## How to Use

### 1. Interactive Mode (Recommended)

Run the tool without arguments:

```bash
bun start
```
*(or `bun run index.ts`)*

The interactive prompt will guide you:
1. **Enter the creator URL**: Paste the creator profile URL open in your browser (e.g. `https://coomerfans.com/u/onlyfans/313401/rbbca`).
2. **Choose an option**:
   - `[1] Download file list`: Scrapes all post titles page-wise and writes `file_list.json`.
   - `[2] Download content`: Downloads all media files inside `.post-body` with resume support.

---

### 2. Download File List (Option 1)

Collects all post titles and links page-by-page across the entire profile without downloading any media files:

```bash
bun run list https://coomerfans.com/u/onlyfans/313401/rbbca
```
*(or `bun run index.ts <url> --list`)*

**Output**:
- Creates `downloads/<creator>/file_list.json` containing the complete page-by-page index of all posts.

---

### 3. Download Content (Option 2)

Downloads all images and videos found in each post's `.post-body`:

```bash
bun run download https://coomerfans.com/u/onlyfans/313401/rbbca
```
*(or `bun run index.ts <url> --download`)*

**Output**:
- Downloads media files directly to `downloads/<creator>/`.
- Maintains and updates `downloads/<creator>/progress.json` and `file_list.json`.

---

## CLI Options & Flags

You can customize the downloader using flags:

```bash
bun run index.ts [url] [options]
```

| Flag | Shorthand | Description | Default |
|---|---|---|---|
| `--list` | `-l` | Scrape and save post list page-wise to `file_list.json` | - |
| `--download` | `-d` | Download all media content with resume capability | - |
| `--output <dir>` | `-o` | Specify custom destination folder | `./downloads` |
| `--delay <ms>` | - | Polite delay between requests in milliseconds | `500` |
| `--help` | `-h` | Display help and usage examples | - |

### Examples:

- **Custom output directory**:
  ```bash
  bun run index.ts https://coomerfans.com/u/onlyfans/313401/rbbca -d -o /path/to/my_folder
  ```

- **Set a custom delay between requests (e.g. 1000ms)**:
  ```bash
  bun run index.ts https://coomerfans.com/u/onlyfans/313401/rbbca -d --delay 1000
  ```

---

## Output Directory Structure

All files for a creator are stored cleanly in `downloads/<creator>/`:

```text
downloads/
└── rbbca/
    ├── file_list.json    # Complete page-wise post list
    ├── progress.json     # State tracking for resuming
    └── files/            # All downloaded media files
        ├── [91536913] taking my time to corrupt this good christian boy ✝️.jpg
        ├── [91536918] edge me for the whole day if u want ur entire room soaked wi.jpg
        ├── [91536919] my response to all the fucked up twisted shit yall say to me.mp4
        └── [91536920] after convincing me to skip class：.mp4
```

> **Note**: All media files are downloaded directly into the `files/` subfolder. No post subfolders or `.txt` files are created.

---

## Key Mechanisms

### Title Sanitization & Character Encoding
Post titles often contain emojis (`✝️`, `❤️`) and characters forbidden by filesystems (`:`, `?`, `"`, `<`, `>`, `\`, `/`, `|`).
- Standard sanitizers strip these characters, destroying readability.
- This tool encodes illegal characters into safe visual Unicode equivalents (`：`, `？`, `”`, `《`, `》`, `＼`, `⁄`, `¦`).
- Emojis and international characters are preserved 100% intact.

### Automatic 4-Step Retry & Fallback
When downloading media files:
1. **Initial URL Retries**: Retries up to 4 times with a 500ms delay between attempts.
2. **Subdomain Rotation**: If `img<N>.coomerfans.com` fails, the tool automatically tries alternative subdomains (`img1` through `img10.coomerfans.com`).
3. **Post Page Refetch**: If subdomains fail (e.g., expired timestamp or hash), the tool refetches the post page HTML to retrieve a fresh signed URL and retries.
4. **Failure Tracking**: If all retries fail, the file is marked with `"status": "failed"` in `file_list.json` and `progress.json`.

### Interrupt & Resume Support
- If your connection drops or you press `Ctrl+C`, current progress is saved immediately.
- Re-running the command automatically loads `progress.json`:
  - Posts with completed files already on disk are instantly skipped.
  - Unfinished posts and failed files are automatically resumed.

---

## License
MIT
