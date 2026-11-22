import { JSDOM } from "jsdom";
import { mkdir, rename, stat, unlink } from "node:fs/promises";
import path from "node:path";
import { spawn } from "node:child_process";

export interface RequestOptions {
  headers?: Record<string, string>;
  maxRetries?: number;
  retryDelayMs?: number;
  timeoutMs?: number;
}

export class HttpClient {
  private cookies: Map<string, string> = new Map();
  private userAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36";
  private defaultReferer = "https://coomerfans.com/";

  constructor(customUserAgent?: string) {
    if (customUserAgent) {
      this.userAgent = customUserAgent;
    }
  }

  private getCookieHeader(): string {
    const pairs: string[] = [];
    for (const [key, val] of this.cookies.entries()) {
      pairs.push(`${key}=${val}`);
    }
    return pairs.join("; ");
  }

  private saveCookies(response: Response): void {
    const rawSetCookie = response.headers.get("set-cookie");
    if (!rawSetCookie) return;

    const cookieStrings = rawSetCookie.split(/,(?=[^;]+=[^;]+)/);
    for (const cookieStr of cookieStrings) {
      const parts = cookieStr.trim().split(";")[0];
      const eqIdx = parts.indexOf("=");
      if (eqIdx > 0) {
        const key = parts.slice(0, eqIdx).trim();
        const val = parts.slice(eqIdx + 1).trim();
        if (key) {
          this.cookies.set(key, val);
        }
      }
    }
  }

  public async request(url: string, options: RequestOptions = {}): Promise<Response> {
    const maxRetries = options.maxRetries ?? 3;
    const baseRetryDelay = options.retryDelayMs ?? 1500;
    const timeoutMs = options.timeoutMs ?? 30000;

    let lastError: any = null;

    for (let attempt = 0; attempt <= maxRetries; attempt++) {
      try {
        const headers: Record<string, string> = {
          "User-Agent": this.userAgent,
          "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,video/*,*/*;q=0.8",
          "Accept-Language": "en-US,en;q=0.9",
          "Referer": this.defaultReferer,
          ...options.headers,
        };

        const cookieHeader = this.getCookieHeader();
        if (cookieHeader) {
          headers["Cookie"] = cookieHeader;
        }

        const controller = new AbortController();
        const timeout = setTimeout(() => controller.abort(), timeoutMs);

        const res = await fetch(url, {
          headers,
          signal: controller.signal,
        });

        clearTimeout(timeout);
        this.saveCookies(res);

        if ((res.status === 429 || res.status === 503) && attempt < maxRetries) {
          const delay = baseRetryDelay * Math.pow(2, attempt);
          console.warn(`[HTTP ${res.status}] Retrying in ${Math.round(delay / 1000)}s for ${url}...`);
          await new Promise((resolve) => setTimeout(resolve, delay));
          continue;
        }

        if (!res.ok) {
          throw new Error(`HTTP ${res.status} ${res.statusText}`);
        }

        return res;
      } catch (err: any) {
        lastError = err;
        if (attempt < maxRetries) {
          const delay = baseRetryDelay * Math.pow(2, attempt);
          await new Promise((resolve) => setTimeout(resolve, delay));
        }
      }
    }

    throw new Error(`Failed to fetch ${url} after ${maxRetries + 1} attempts: ${lastError?.message || lastError}`);
  }

  public async fetchText(url: string, options?: RequestOptions): Promise<string> {
    const res = await this.request(url, options);
    return await res.text();
  }

  public async fetchDocument(url: string, options?: RequestOptions): Promise<Document> {
    const html = await this.fetchText(url, options);
    const dom = new JSDOM(html, { url });
    return dom.window.document;
  }

  /**
   * Downloads a file to disk using curl to bypass CDN TLS fingerprint restrictions.
   */
  public async downloadFile(
    url: string,
    destDir: string,
    customFilename?: string,
    options?: RequestOptions
  ): Promise<{ localPath: string; size: number }> {
    await mkdir(destDir, { recursive: true });

    let filename = customFilename;
    if (!filename) {
      try {
        const u = new URL(url);
        filename = decodeURIComponent(u.pathname.split("/").filter(Boolean).pop() || "");
      } catch {}
      if (!filename) {
        filename = "download";
      }
    }

    // Default to .jpg if no extension found
    if (!filename.includes(".")) {
      filename += ".jpg";
    }

    const finalPath = path.join(destDir, filename);
    const partPath = `${finalPath}.part`;

    // Check if already downloaded and has valid size
    try {
      const existingStat = await stat(finalPath);
      if (existingStat.isFile() && existingStat.size > 0) {
        return { localPath: finalPath, size: existingStat.size };
      }
    } catch {}

    const maxRetries = options?.maxRetries ?? 4;
    const retryDelay = options?.retryDelayMs ?? 500;
    let lastError: any = null;

    for (let attempt = 0; attempt <= maxRetries; attempt++) {
      try {
        // Clean up partial file if exists before retrying
        try {
          await unlink(partPath);
        } catch {}

        const httpCode = await new Promise<number>((resolve, reject) => {
          const args = [
            "-s",
            "-L",
            "--connect-timeout", "20",
            "--max-time", "180",
            "-A", this.userAgent,
            "-e", this.defaultReferer,
            "-w", "%{http_code}",
            "-o", partPath,
            url,
          ];

          const child = spawn("curl", args);
          let stdout = "";
          let stderr = "";

          child.stdout?.on("data", (chunk) => {
            stdout += chunk.toString();
          });
          child.stderr?.on("data", (chunk) => {
            stderr += chunk.toString();
          });

          child.on("error", (err) => reject(err));
          child.on("close", (code) => {
            if (code !== 0) {
              reject(new Error(`curl exited with code ${code}: ${stderr}`));
            } else {
              const statusCode = parseInt(stdout.trim(), 10) || 0;
              resolve(statusCode);
            }
          });
        });

        if (httpCode < 200 || httpCode >= 300) {
          const err = new Error(`HTTP ${httpCode}`);
          // 404 indicates resource is not on this host; don't burn retries on same URL
          if (httpCode === 404) {
            throw err;
          }
          lastError = err;
          if (attempt < maxRetries) {
            await new Promise((r) => setTimeout(r, retryDelay));
            continue;
          }
          throw err;
        }

        // Rename part to final
        await rename(partPath, finalPath);
        const finalStat = await stat(finalPath);
        return { localPath: finalPath, size: finalStat.size };
      } catch (err: any) {
        lastError = err;
        if (err.message === "HTTP 404") {
          break; // Don't retry same URL on 404
        }
        if (attempt < maxRetries) {
          await new Promise((r) => setTimeout(r, retryDelay));
        }
      }
    }

    throw new Error(`Failed to download ${url}: ${lastError?.message || lastError}`);
  }
}

export const defaultClient = new HttpClient();
