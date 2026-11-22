import { mkdir, readFile, rename, writeFile } from "node:fs/promises";
import path from "node:path";
import type { CreatorInfo, FileListOutput, PageData, ProgressState } from "./types";

export class StorageManager {
  private baseDir: string;
  private creator: string;
  private creatorDir: string;
  private progressPath: string;
  private listJsonPath: string;

  constructor(baseOutputDir: string, creator: string) {
    this.baseDir = baseOutputDir;
    this.creator = creator;
    this.creatorDir = path.join(this.baseDir, this.creator);
    this.progressPath = path.join(this.creatorDir, "progress.json");
    this.listJsonPath = path.join(this.creatorDir, "file_list.json");
  }

  public getCreatorDir(): string {
    return this.creatorDir;
  }

  public getProgressFilePath(): string {
    return this.progressPath;
  }

  public getFileListJsonPath(): string {
    return this.listJsonPath;
  }

  public async initDirs(): Promise<void> {
    await mkdir(this.creatorDir, { recursive: true });
  }

  /**
   * Loads existing progress state if it exists.
   */
  public async loadProgress(): Promise<ProgressState | null> {
    try {
      const content = await readFile(this.progressPath, "utf-8");
      return JSON.parse(content) as ProgressState;
    } catch {
      return null;
    }
  }

  /**
   * Loads existing file_list.json if it exists.
   */
  public async loadExistingFileList(): Promise<FileListOutput | null> {
    try {
      const content = await readFile(this.listJsonPath, "utf-8");
      return JSON.parse(content) as FileListOutput;
    } catch {
      return null;
    }
  }

  /**
   * Atomically saves progress state to disk.
   */
  public async saveProgress(state: ProgressState): Promise<void> {
    await this.initDirs();
    state.lastUpdated = new Date().toISOString();
    const tempPath = `${this.progressPath}.tmp`;
    const jsonStr = JSON.stringify(state, null, 2);
    await writeFile(tempPath, jsonStr, "utf-8");
    await rename(tempPath, this.progressPath);
  }

  /**
   * Initializes a fresh progress state if none exists.
   */
  public createInitialProgress(info: CreatorInfo, startUrl: string): ProgressState {
    return {
      creator: info.username,
      service: info.service,
      sourceUrl: info.normalizedUrl,
      outputDir: this.creatorDir,
      currentPageNumber: 1,
      currentPageUrl: startUrl,
      totalPagesScanned: 0,
      totalPostsCount: 0,
      totalFilesDownloaded: 0,
      totalBytesDownloaded: 0,
      isFinished: false,
      lastUpdated: new Date().toISOString(),
      pages: {},
    };
  }

  /**
   * Generates and writes file_list.json in page-wise format.
   */
  public async writePageWiseJson(
    info: CreatorInfo,
    pages: PageData[]
  ): Promise<string> {
    await this.initDirs();

    const totalPosts = pages.reduce((sum, p) => sum + p.posts.length, 0);

    const jsonOutput: FileListOutput = {
      creator: info.username,
      service: info.service,
      sourceUrl: info.normalizedUrl,
      totalPages: pages.length,
      totalPosts,
      generatedAt: new Date().toISOString(),
      pages: pages.map((p) => ({
        pageNumber: p.pageNumber,
        pageUrl: p.pageUrl,
        postCount: p.posts.length,
        posts: p.posts.map((post) => ({
          id: post.id,
          title: post.title,
          safeTitle: post.safeTitle,
          url: post.url,
          date: post.date,
          status: post.status,
          files: post.files,
        })),
      })),
    };

    const tempJson = `${this.listJsonPath}.tmp`;
    await writeFile(tempJson, JSON.stringify(jsonOutput, null, 2), "utf-8");
    await rename(tempJson, this.listJsonPath);

    return this.listJsonPath;
  }
}
