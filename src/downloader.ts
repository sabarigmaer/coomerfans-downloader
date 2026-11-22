import path from "node:path";
import { existsSync } from "node:fs";
import { HttpClient, defaultClient } from "./client";
import { extractPostMedia, extractPostsFromPage, getNextPageUrl, parseCreatorInfo } from "./scraper";
import { StorageManager } from "./storage";
import type { CreatorInfo, PageData, PostFile, PostItem, ProgressState } from "./types";

export interface DownloaderOptions {
  outputDir?: string;
  delayMs?: number;
  client?: HttpClient;
  onProgress?: (message: string) => void;
}

function formatBytes(bytes?: number): string {
  if (!bytes || bytes <= 0) return "0 B";
  const units = ["B", "KB", "MB", "GB", "TB"];
  const i = Math.floor(Math.log(bytes) / Math.log(1024));
  return `${(bytes / Math.pow(1024, i)).toFixed(2)} ${units[i]}`;
}

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function getFileExtension(fileUrl: string, defaultExt: string = ".jpg"): string {
  try {
    const pathname = new URL(fileUrl).pathname;
    const ext = path.extname(pathname);
    if (ext && ext.length >= 2 && ext.length <= 5) {
      return ext.toLowerCase();
    }
  } catch {}
  return defaultExt;
}

/**
 * Generates alternative URLs by rotating img<N>.coomerfans.com from 1 to 10.
 */
function getSubdomainVariants(fileUrl: string): string[] {
  const variants: string[] = [];
  try {
    const parsed = new URL(fileUrl);
    const hostMatch = parsed.hostname.match(/^img(\d+)\.coomerfans\.com$/i);
    if (hostMatch) {
      const originalNum = parseInt(hostMatch[1], 10);
      for (let n = 1; n <= 10; n++) {
        if (n !== originalNum) {
          const u = new URL(fileUrl);
          u.hostname = `img${n}.coomerfans.com`;
          variants.push(u.toString());
        }
      }
    }
  } catch {}
  return variants;
}

export class CoomerDownloader {
  private client: HttpClient;
  private delayMs: number;
  private outputDir: string;

  constructor(options: DownloaderOptions = {}) {
    this.client = options.client || defaultClient;
    this.delayMs = options.delayMs ?? 500;
    this.outputDir = options.outputDir || "./downloads";
  }

  /**
   * Robust download with:
   * 1. Initial URL download with 4 retries after 500ms delay.
   * 2. If failing, rotates subdomain numbers (img1..img10) with 4 retries (500ms delay).
   * 3. If failing, refetches the post page to get a fresh link and retries.
   * 4. If everything fails, throws an error so file is recorded as failed in file_list.json.
   */
  private async downloadFileWithRobustRetries(
    fileUrl: string,
    postUrl: string,
    fIdx: number,
    destDir: string,
    filename: string,
    log: (msg: string) => void
  ): Promise<{ localPath: string; size: number }> {
    // Step 1: Initial URL download with 4 retries and 500ms delay
    try {
      return await this.client.downloadFile(fileUrl, destDir, filename, {
        maxRetries: 4,
        retryDelayMs: 500,
      });
    } catch (initialErr: any) {
      log(`   -> [Retry] Initial URL failed: ${initialErr.message}`);
    }

    // Step 2: Try alternative subdomains img1..img10
    const subdomainVariants = getSubdomainVariants(fileUrl);
    if (subdomainVariants.length > 0) {
      log(`   -> [Retry] Trying alternative subdomains (img1..img10)...`);
      for (const variantUrl of subdomainVariants) {
        try {
          const variantHost = new URL(variantUrl).hostname;
          log(`   -> Trying subdomain host ${variantHost} ...`);
          return await this.client.downloadFile(variantUrl, destDir, filename, {
            maxRetries: 4,
            retryDelayMs: 500,
          });
        } catch {
          // Continue to next subdomain
        }
      }
    }

    // Step 3: Refetch the post page and try the new link again with retry
    log(`   -> [Retry] Subdomains failed. Refetching post page (${postUrl}) for fresh link...`);
    await sleep(500);

    try {
      const freshDoc = await this.client.fetchDocument(postUrl);
      const freshMedia = extractPostMedia(freshDoc, postUrl);
      const freshUrl = freshMedia.files[fIdx] || freshMedia.files[0];

      if (freshUrl) {
        log(`   -> Got fresh link from post page. Retrying download with 4 retries (500ms delay)...`);
        try {
          return await this.client.downloadFile(freshUrl, destDir, filename, {
            maxRetries: 4,
            retryDelayMs: 500,
          });
        } catch (freshErr: any) {
          log(`   -> [Retry] Fresh link initial attempt failed: ${freshErr.message}`);
        }

        // Try fresh link subdomain variants
        const freshVariants = getSubdomainVariants(freshUrl);
        for (const fVar of freshVariants) {
          try {
            const varHost = new URL(fVar).hostname;
            log(`   -> Trying fresh link subdomain ${varHost} ...`);
            return await this.client.downloadFile(fVar, destDir, filename, {
              maxRetries: 4,
              retryDelayMs: 500,
            });
          } catch {}
        }
      }
    } catch (refetchErr: any) {
      log(`   -> [Retry] Error refetching post page: ${refetchErr.message}`);
    }

    // Step 4: If everything fails
    throw new Error(
      `All download attempts, img1-10 subdomains, and post page refetch failed for ${filename}`
    );
  }

  /**
   * Option 1: Download File List
   * Crawls through all pages using the "Next" button, gets the title of each post,
   * and stores them page-wise in file_list.json (no files, no text files, no subfolders).
   */
  public async downloadFileList(
    startUrl: string,
    onProgress?: (msg: string) => void
  ): Promise<{ jsonPath: string; totalPosts: number; totalPages: number }> {
    const log = onProgress || console.log;
    const info = parseCreatorInfo(startUrl);
    const storage = new StorageManager(this.outputDir, info.username);
    await storage.initDirs();

    log(`\n==================================================`);
    log(`  MODE: Download File List`);
    log(`  Creator: ${info.username} (${info.service})`);
    log(`  Starting URL: ${startUrl}`);
    log(`  Output Directory: ${storage.getCreatorDir()}`);
    log(`==================================================\n`);

    const pages: PageData[] = [];
    const visitedUrls = new Set<string>();

    let currentUrl: string | null = startUrl;
    let pageNumber = 1;

    while (currentUrl) {
      if (visitedUrls.has(currentUrl)) {
        log(`[Page ${pageNumber}] URL already visited (${currentUrl}), ending pagination.`);
        break;
      }
      visitedUrls.add(currentUrl);

      log(`[Page ${pageNumber}] Fetching: ${currentUrl} ...`);

      let doc: Document;
      try {
        doc = await this.client.fetchDocument(currentUrl);
      } catch (err: any) {
        log(`[Page ${pageNumber}] Error fetching page: ${err.message}`);
        break;
      }

      const posts = extractPostsFromPage(doc, currentUrl);
      log(`[Page ${pageNumber}] Found ${posts.length} posts.`);

      const pageData: PageData = {
        pageNumber,
        pageUrl: currentUrl,
        postCount: posts.length,
        status: "completed",
        posts,
      };

      pages.push(pageData);

      // Keep file_list.json updated after each page
      await storage.writePageWiseJson(info, pages);

      // Check next page
      const nextUrl = getNextPageUrl(doc, currentUrl);
      if (nextUrl && !visitedUrls.has(nextUrl)) {
        log(`[Page ${pageNumber}] Next page found -> ${nextUrl}`);
        currentUrl = nextUrl;
        pageNumber++;
        if (this.delayMs > 0) await sleep(this.delayMs);
      } else {
        log(`[Page ${pageNumber}] No further "Next" page link found. End of profile.`);
        currentUrl = null;
      }
    }

    const jsonPath = await storage.writePageWiseJson(info, pages);
    const totalPosts = pages.reduce((sum, p) => sum + p.posts.length, 0);

    log(`\n==================================================`);
    log(`  File list generation completed!`);
    log(`  Total Pages Scanned: ${pages.length}`);
    log(`  Total Posts Found:   ${totalPosts}`);
    log(`  File List (JSON):    ${jsonPath}`);
    log(`==================================================\n`);

    return { jsonPath, totalPosts, totalPages: pages.length };
  }

  /**
   * Option 2: Download Content
   * Crawls through each page, goes inside each post, extracts files inside `.post-body`,
   * downloads them directly into the creator directory (no post subfolders, no txt files),
   * naming files using the sanitized post title and ID, and keeps progress.json and
   * file_list.json updated to resume from the exact page and post on any failure.
   */
  public async downloadContent(
    startUrl: string,
    onProgress?: (msg: string) => void
  ): Promise<ProgressState> {
    const log = onProgress || console.log;
    const info = parseCreatorInfo(startUrl);
    const storage = new StorageManager(this.outputDir, info.username);
    await storage.initDirs();

    // Check for existing progress
    let state = await storage.loadProgress();
    if (state && !state.isFinished && Object.keys(state.pages).length > 0) {
      log(`\n[Resume] Found existing progress for "${info.username}". Resuming download...`);
      log(`[Resume] Last updated: ${state.lastUpdated}`);
    } else {
      state = storage.createInitialProgress(info, startUrl);

      // If file_list.json already exists, pre-populate state with discovered posts
      const existingList = await storage.loadExistingFileList();
      if (existingList && existingList.pages.length > 0) {
        log(`\n[Init] Loaded existing file_list.json with ${existingList.totalPages} pages and ${existingList.totalPosts} posts.`);
        for (const p of existingList.pages) {
          state.pages[String(p.pageNumber)] = {
            pageNumber: p.pageNumber,
            pageUrl: p.pageUrl,
            postCount: p.posts.length,
            status: "pending",
            posts: p.posts.map((post) => ({
              id: post.id,
              title: post.title,
              safeTitle: post.safeTitle,
              url: post.url,
              date: post.date,
              status: post.status || "pending",
              files: post.files,
            })),
          };
        }
        state.totalPagesScanned = existingList.totalPages;
        state.totalPostsCount = existingList.totalPosts;
      }

      await storage.saveProgress(state);
    }

    log(`\n==================================================`);
    log(`  MODE: Download Content (Resumable)`);
    log(`  Creator: ${info.username} (${info.service})`);
    log(`  Starting URL: ${startUrl}`);
    log(`  Destination: ${storage.getCreatorDir()}`);
    log(`==================================================\n`);

    // Setup graceful exit handler to save state on Ctrl+C
    let isTerminating = false;
    const sigintHandler = async () => {
      if (isTerminating) return;
      isTerminating = true;
      log(`\n\n[Interrupt] Caught exit signal. Saving current progress...`);
      if (state) {
        state.lastUpdated = new Date().toISOString();
        await storage.saveProgress(state);
        const pagesArray = Object.values(state.pages).sort((a, b) => a.pageNumber - b.pageNumber);
        await storage.writePageWiseJson(info, pagesArray);
        log(`[Interrupt] Progress saved to progress.json. You can resume anytime by re-running the command.`);
      }
      process.exit(0);
    };
    process.on("SIGINT", sigintHandler);
    process.on("SIGTERM", sigintHandler);

    try {
      const visitedUrls = new Set<string>();
      let currentUrl: string | null = state.currentPageUrl || startUrl;
      let pageNumber = state.currentPageNumber || 1;

      while (currentUrl && !isTerminating) {
        if (visitedUrls.has(currentUrl)) {
          log(`[Page ${pageNumber}] Already visited (${currentUrl}), stopping pagination.`);
          break;
        }
        visitedUrls.add(currentUrl);

        state.currentPageNumber = pageNumber;
        state.currentPageUrl = currentUrl;

        const pageKey = String(pageNumber);
        let pageData = state.pages[pageKey];

        let doc: Document | null = null;
        let nextUrl: string | null = null;

        if (!pageData || pageData.status !== "completed") {
          log(`\n--------------------------------------------------`);
          log(`[Page ${pageNumber}] Fetching listing: ${currentUrl}`);
          log(`--------------------------------------------------`);

          doc = await this.client.fetchDocument(currentUrl);
          const scrapedPosts = extractPostsFromPage(doc, currentUrl);

          nextUrl = getNextPageUrl(doc, currentUrl);

          // Preserve completed post records
          const existingPostsMap = new Map<string, PostItem>();
          if (pageData && pageData.posts) {
            for (const p of pageData.posts) {
              existingPostsMap.set(p.id, p);
            }
          }

          const mergedPosts: PostItem[] = scrapedPosts.map((p) => {
            const existing = existingPostsMap.get(p.id);
            if (existing && existing.status === "completed") {
              return existing;
            }
            return existing || p;
          });

          pageData = {
            pageNumber,
            pageUrl: currentUrl,
            postCount: mergedPosts.length,
            status: "in_progress",
            posts: mergedPosts,
          };

          state.pages[pageKey] = pageData;
          state.totalPagesScanned = Object.keys(state.pages).length;
          await storage.saveProgress(state);

          // Update file_list.json
          const pagesArray = Object.values(state.pages).sort((a, b) => a.pageNumber - b.pageNumber);
          await storage.writePageWiseJson(info, pagesArray);
        } else {
          log(`[Page ${pageNumber}] Cached in progress. Checking posts...`);
        }

        const totalPostsOnPage = pageData.posts.length;

        for (let i = 0; i < totalPostsOnPage; i++) {
          if (isTerminating) break;

          const post = pageData.posts[i];
          const postIndex = i + 1;

          // Check if post already completed and its files exist on disk
          if (post.status === "completed" && post.files && post.files.length > 0) {
            const allFilesExist = post.files.every(
              (f) => f.status === "completed" && f.localPath && existsSync(f.localPath)
            );
            if (allFilesExist) {
              log(`[Page ${pageNumber} | Post ${postIndex}/${totalPostsOnPage}] [Skipped] Already downloaded: "${post.title}" (ID: ${post.id})`);
              continue;
            }
          }

          log(`\n[Page ${pageNumber} | Post ${postIndex}/${totalPostsOnPage}] Processing: "${post.title}" (ID: ${post.id})`);
          log(` -> URL: ${post.url}`);

          post.status = "in_progress";
          await storage.saveProgress(state);

          try {
            // Fetch post page
            const postDoc = await this.client.fetchDocument(post.url);
            const media = extractPostMedia(postDoc, post.url);

            if (media.fullTitle) {
              post.title = media.fullTitle;
            }
            if (media.description) {
              post.description = media.description;
            }

            log(` -> Found ${media.files.length} file(s) in .post-body`);

            const destDir = storage.getCreatorDir();
            const postFiles: PostFile[] = [];

            // Download each media file directly into creator dir
            for (let fIdx = 0; fIdx < media.files.length; fIdx++) {
              if (isTerminating) break;

              const fileUrl = media.files[fIdx];
              const ext = getFileExtension(fileUrl, ".jpg");

              // File name formatting: [<id>] <safeTitle>.<ext> or [<id>] <safeTitle>_<num>.<ext>
              let filename =
                media.files.length === 1
                  ? `[${post.id}] ${post.safeTitle}${ext}`
                  : `[${post.id}] ${post.safeTitle}_${fIdx + 1}${ext}`;

              // Check if file is already completed and exists on disk
              const existingRecord = post.files?.find((pf) => pf.filename === filename);
              if (
                existingRecord &&
                existingRecord.status === "completed" &&
                existingRecord.localPath &&
                existsSync(existingRecord.localPath)
              ) {
                log(`   [File ${fIdx + 1}/${media.files.length}] [Skipped] Already exists on disk: ${filename}`);
                postFiles.push(existingRecord);
                continue;
              }

              log(`   [File ${fIdx + 1}/${media.files.length}] Downloading: ${filename} ...`);

              try {
                // Execute robust retry logic (Subdomain rotation img1..10, 4 retries, page refetch)
                const downloadResult = await this.downloadFileWithRobustRetries(
                  fileUrl,
                  post.url,
                  fIdx,
                  destDir,
                  filename,
                  log
                );
                log(`   -> Downloaded (${formatBytes(downloadResult.size)})`);

                postFiles.push({
                  url: fileUrl,
                  filename,
                  size: downloadResult.size,
                  status: "completed",
                  localPath: downloadResult.localPath,
                });

                state.totalFilesDownloaded++;
                state.totalBytesDownloaded += downloadResult.size;
              } catch (fileErr: any) {
                log(`   -> [FAILED] ${fileErr.message}`);
                // Record failed file in state and file_list.json so it can be retried in future
                postFiles.push({
                  url: fileUrl,
                  filename,
                  status: "failed",
                  error: fileErr.message,
                });
              }

              // Update state and save progress and file_list.json after each file
              post.files = postFiles;
              await storage.saveProgress(state);
              const pagesArray = Object.values(state.pages).sort((a, b) => a.pageNumber - b.pageNumber);
              await storage.writePageWiseJson(info, pagesArray);
            }

            const hasFailures = postFiles.some((f) => f.status === "failed");
            post.status = hasFailures ? "failed" : "completed";
          } catch (postErr: any) {
            log(` -> [Error processing post ${post.id}]: ${postErr.message}`);
            post.status = "failed";
            post.error = postErr.message;
          }

          // Save state after each post
          await storage.saveProgress(state);

          // Keep file_list.json updated after each post
          const pagesArray = Object.values(state.pages).sort((a, b) => a.pageNumber - b.pageNumber);
          await storage.writePageWiseJson(info, pagesArray);

          if (this.delayMs > 0) await sleep(this.delayMs);
        }

        // Mark page completed if all posts completed
        const allPostsOnPageCompleted = pageData.posts.every((p) => p.status === "completed");
        pageData.status = allPostsOnPageCompleted ? "completed" : "in_progress";
        await storage.saveProgress(state);

        // Determine next page URL
        if (!nextUrl && doc) {
          nextUrl = getNextPageUrl(doc, currentUrl);
        } else if (!nextUrl) {
          try {
            const pageDoc = await this.client.fetchDocument(currentUrl);
            nextUrl = getNextPageUrl(pageDoc, currentUrl);
          } catch {}
        }

        if (nextUrl && !visitedUrls.has(nextUrl)) {
          log(`\n[Page ${pageNumber}] Finished page. Moving to Next -> ${nextUrl}`);
          currentUrl = nextUrl;
          pageNumber++;
          if (this.delayMs > 0) await sleep(this.delayMs);
        } else {
          log(`\n[Page ${pageNumber}] No next page found. Profile crawl finished.`);
          currentUrl = null;
        }
      }

      state.isFinished = true;
      state.lastUpdated = new Date().toISOString();
      await storage.saveProgress(state);

      // Final update to file_list.json
      const finalPagesArray = Object.values(state.pages).sort((a, b) => a.pageNumber - b.pageNumber);
      await storage.writePageWiseJson(info, finalPagesArray);

      log(`\n==================================================`);
      log(`  Download Content Completed!`);
      log(`  Creator:           ${info.username}`);
      log(`  Pages Scanned:     ${Object.keys(state.pages).length}`);
      log(`  Files Downloaded:  ${state.totalFilesDownloaded}`);
      log(`  Total Downloaded:  ${formatBytes(state.totalBytesDownloaded)}`);
      log(`  Destination:       ${storage.getCreatorDir()}`);
      log(`  Progress File:     ${storage.getProgressFilePath()}`);
      log(`  File List (JSON):  ${storage.getFileListJsonPath()}`);
      log(`==================================================\n`);

      return state;
    } finally {
      process.off("SIGINT", sigintHandler);
      process.off("SIGTERM", sigintHandler);
    }
  }
}
