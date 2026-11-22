import { sanitizeTitle, decodeHtmlEntities } from "./sanitizer";
import type { CreatorInfo, PostItem } from "./types";

/**
 * Parses creator and service information from a CoomerFans URL.
 * Example: https://coomerfans.com/u/onlyfans/313401/rbbca?page=2
 */
export function parseCreatorInfo(rawUrl: string): CreatorInfo {
  let parsed: URL;
  try {
    parsed = new URL(rawUrl);
  } catch {
    throw new Error(`Invalid URL: ${rawUrl}`);
  }

  // Pathname: /u/onlyfans/313401/rbbca
  const segments = parsed.pathname.split("/").filter(Boolean);

  let service = "unknown";
  let userId = "";
  let username = "";

  if (segments[0] === "u") {
    if (segments.length >= 4) {
      service = segments[1];
      userId = segments[2];
      username = segments[3];
    } else if (segments.length === 3) {
      service = segments[1];
      username = segments[2];
      userId = segments[2];
    } else if (segments.length === 2) {
      username = segments[1];
      userId = segments[1];
    }
  }

  if (!username) {
    username = segments[segments.length - 1] || "creator";
  }

  // Build clean normalized URL without query params
  const normalizedUrl = `${parsed.origin}${parsed.pathname}`;

  return {
    service,
    userId,
    username,
    normalizedUrl,
  };
}

/**
 * Extracts post items from a profile listing page.
 */
export function extractPostsFromPage(doc: Document, pageUrl: string): PostItem[] {
  const posts: PostItem[] = [];
  const postCards = doc.querySelectorAll(".post, article.post, .post-card");

  for (const card of postCards) {
    // 1. Post link & title
    const linkEl = card.querySelector("h3 a") || card.querySelector("a.view-post") || card.querySelector("a[href*='/p/']");
    if (!linkEl) continue;

    const href = linkEl.getAttribute("href");
    if (!href) continue;

    const fullUrl = new URL(href, pageUrl).toString();

    // Extract ID: e.g. /p/91536913/313401/onlyfans -> 91536913
    const idMatch = href.match(/\/p\/([^\/]+)/);
    const id = idMatch ? idMatch[1] : href.replace(/[^a-zA-Z0-9_-]/g, "_");

    // Title from h3 a or h3
    const rawTitle = (card.querySelector("h3")?.textContent || linkEl.textContent || "").trim();
    const cleanRawTitle = decodeHtmlEntities(rawTitle) || `Post ${id}`;
    const safeTitle = sanitizeTitle(cleanRawTitle);

    // Date
    const date = card.querySelector(".post-date")?.textContent?.trim() || "";

    // Text preview
    const description = card.querySelector("p")?.textContent?.trim() || "";

    posts.push({
      id,
      title: cleanRawTitle,
      safeTitle,
      url: fullUrl,
      date,
      description,
      status: "pending",
    });
  }

  return posts;
}

/**
 * Finds the "Next" page URL from pagination.
 */
export function getNextPageUrl(doc: Document, currentUrl: string): string | null {
  // Look for a.next or within .pagination
  const nextEl =
    doc.querySelector(".pagination a.next, a.next, a[rel='next'], .pagination-bottom a.next") ||
    Array.from(doc.querySelectorAll(".pagination a, nav a")).find((el) => {
      const text = el.textContent?.trim().toLowerCase() || "";
      return text === "next" || text.startsWith("next ");
    });

  if (!nextEl) return null;

  const href = nextEl.getAttribute("href");
  if (!href) return null;

  try {
    const nextUrl = new URL(href, currentUrl).toString();
    // Verify it's not the exact same page
    if (nextUrl === currentUrl) return null;
    return nextUrl;
  } catch {
    return null;
  }
}

/**
 * Extracts downloadable media files from inside the `.post-body` of a post page.
 */
export function extractPostMedia(
  doc: Document,
  postUrl: string
): { files: string[]; fullTitle?: string; description?: string } {
  const postBody = doc.querySelector(".post-body") || doc.querySelector(".post-wrap") || doc.body;
  if (!postBody) {
    return { files: [] };
  }

  // Full title from h1 if present
  const h1Text = doc.querySelector(".post-wrap h1, h1")?.textContent?.trim();
  const fullTitle = h1Text ? decodeHtmlEntities(h1Text) : undefined;

  // Description/caption
  const descText = doc.querySelector(".post-wrap p, .post-body p")?.textContent?.trim();

  const fileUrls = new Set<string>();

  // 1. Images
  const images = postBody.querySelectorAll("img");
  for (const img of images) {
    const src = img.getAttribute("src") || img.getAttribute("data-src") || img.getAttribute("data-url");
    if (src && !src.startsWith("data:") && !src.includes("creative.tscprts.com")) {
      try {
        fileUrls.add(new URL(src, postUrl).toString());
      } catch {}
    }
  }

  // 2. Videos and video sources
  const videos = postBody.querySelectorAll("video");
  for (const video of videos) {
    const vSrc = video.getAttribute("src") || video.getAttribute("data-src");
    if (vSrc && !vSrc.startsWith("data:")) {
      try {
        fileUrls.add(new URL(vSrc, postUrl).toString());
      } catch {}
    }

    const sources = video.querySelectorAll("source");
    for (const source of sources) {
      const sSrc = source.getAttribute("src") || source.getAttribute("data-src");
      if (sSrc && !sSrc.startsWith("data:")) {
        try {
          fileUrls.add(new URL(sSrc, postUrl).toString());
        } catch {}
      }
    }
  }

  // 3. Audio and audio sources
  const audios = postBody.querySelectorAll("audio");
  for (const audio of audios) {
    const aSrc = audio.getAttribute("src") || audio.getAttribute("data-src");
    if (aSrc && !aSrc.startsWith("data:")) {
      try {
        fileUrls.add(new URL(aSrc, postUrl).toString());
      } catch {}
    }

    const sources = audio.querySelectorAll("source");
    for (const source of sources) {
      const sSrc = source.getAttribute("src");
      if (sSrc && !sSrc.startsWith("data:")) {
        try {
          fileUrls.add(new URL(sSrc, postUrl).toString());
        } catch {}
      }
    }
  }

  // 4. Download links or storage links in post body
  const links = postBody.querySelectorAll("a");
  const mediaExtRegex = /\.(jpe?g|png|gif|webp|avif|mp4|webm|m4v|mov|mkv|zip|rar|7z|tar|gz|pdf|mp3|flac|wav)(\?.*)?$/i;

  for (const link of links) {
    const href = link.getAttribute("href");
    if (!href) continue;

    const isStorage = href.includes("/storage/");
    const hasMediaExt = mediaExtRegex.test(href);
    const hasDownloadAttr = link.hasAttribute("download");

    if (isStorage || hasMediaExt || hasDownloadAttr) {
      try {
        fileUrls.add(new URL(href, postUrl).toString());
      } catch {}
    }
  }

  return {
    files: Array.from(fileUrls),
    fullTitle,
    description: descText,
  };
}
