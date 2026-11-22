# CoomerFans Downloader

A CLI tool for downloading content and collecting page-wise file lists from [coomerfans.com](https://coomerfans.com).

## Features

- **Interactive or Flag-driven CLI**: Prompts for target URL or accepts it as an argument.
- **Option 1: Download File List**:
  - Traverses through pages by following the "Next" button.
  - Extracts the title, ID, URL, and date of each post.
  - Stores all post data page-wise in `file_list.json` (no files downloaded, no `.txt` files).
- **Option 2: Download Content**:
  - Crawls each page, enters each post, and downloads all media files found inside `.post-body` directly into the creator directory (`downloads/<creator>/`).
  - No subfolders per post and no `.txt` files.
  - File names are formatted as `[<id>] <title>.<ext>`, safely encoding awkward or filesystem-reserved characters while keeping the title 100% readable.
  - Robust retry logic:
    - 4 retries per attempt with a 500ms delay.
    - Automatic fallback rotating through `img1..img10.coomerfans.com` subdomains.
    - Refetches post page for fresh media links when links expire.
    - Records failed files in `file_list.json` and `progress.json` for future retries.
  - Keeps `file_list.json` and `progress.json` continuously updated in real time to guarantee downloads can resume from the exact page and post where it left off in case of any interruption or failure.

## Quick Start

### 1. Install Dependencies
```bash
bun install
```

### 2. Available Scripts

| Command | Description |
|---|---|
| `bun start` | Launch the interactive CLI menu |
| `bun run list <url>` | Download file list (page-wise `file_list.json`) |
| `bun run download <url>` | Download all media content (resumable) |
| `bun run typecheck` | Run TypeScript type check |

### Examples

**Interactive mode:**
```bash
bun start
```

**Download File List directly:**
```bash
bun run list https://coomerfans.com/u/onlyfans/313401/rbbca
```

**Download Content directly:**
```bash
bun run download https://coomerfans.com/u/onlyfans/313401/rbbca
```

**Custom options:**
```bash
bun run index.ts https://coomerfans.com/u/onlyfans/313401/rbbca --download --output ./my_downloads --delay 1000
```
