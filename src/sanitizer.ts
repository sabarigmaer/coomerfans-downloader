/**
 * Character replacements that map awkward / filesystem-reserved characters
 * to visually identical or clean Unicode equivalents that are safe across
 * Linux, macOS, and Windows filesystems, keeping the title fully intact and readable.
 */
const CHAR_MAP: Record<string, string> = {
  "/": "\u2044", // ⁄ (Fraction slash)
  "\\": "\uFF3C", // ＼ (Fullwidth reverse solidus)
  ":": "\uFF1A", // ： (Fullwidth colon)
  "*": "\u273E", // ✱ (Heavy asterisk)
  "?": "\uFF1F", // ？ (Fullwidth question mark)
  "\"": "\u201D", // ” (Right double quotation mark)
  "<": "\u300A", // 《 (Left double angle bracket)
  ">": "\u300B", // 》 (Right double angle bracket)
  "|": "\u00A6", // ¦ (Broken bar)
};

/**
 * Decode common HTML entities.
 */
export function decodeHtmlEntities(str: string): string {
  if (!str) return "";
  return str
    .replace(/&amp;/g, "&")
    .replace(/&lt;/g, "<")
    .replace(/&gt;/g, ">")
    .replace(/&quot;/g, "\"")
    .replace(/&#039;|&apos;|&#39;/g, "'")
    .replace(/&hellip;/g, "...")
    .replace(/&ndash;/g, "-")
    .replace(/&mdash;/g, "--")
    .replace(/&#(\d+);/g, (_, code) => {
      try {
        return String.fromCharCode(parseInt(code, 10));
      } catch {
        return _;
      }
    });
}

/**
 * Encodes / sanitizes awkward characters in a post title so it can be safely used
 * in folder names and file paths while keeping the title visually intact to read.
 *
 * - Preserves emojis, accents, and readable Unicode text.
 * - Safely maps illegal filesystem characters (/, \, :, *, ?, ", <, >, |) to safe visual equivalents.
 * - Strips control characters and collapses whitespace.
 * - Trims leading/trailing spaces and dots.
 * - Safely truncates length to avoid OS 255-byte filesystem limits.
 */
export function sanitizeTitle(rawTitle: string, maxLength: number = 100): string {
  if (!rawTitle || !rawTitle.trim()) {
    return "untitled";
  }

  // 1. Decode any HTML entities first
  let text = decodeHtmlEntities(rawTitle);

  // 2. Normalize whitespace (tabs, newlines, carriage returns -> single space)
  text = text.replace(/[\r\n\t]+/g, " ");

  // 3. Remove control characters (0x00 - 0x1F, 0x7F - 0x9F)
  text = text.replace(/[\x00-\x1f\x7f-\x9f]/g, "");

  // 4. Map illegal filesystem characters to safe visual Unicode equivalents
  text = text.replace(/[/\\:*?"<>|]/g, (char) => CHAR_MAP[char] || "-");

  // 5. Collapse multiple spaces
  text = text.replace(/\s+/g, " ").trim();

  // 6. Remove leading and trailing dots or spaces which cause OS issues
  text = text.replace(/^[\.\s]+|[\.\s]+$/g, "");

  if (!text) {
    return "untitled";
  }

  // 7. Safely limit length without cutting surrogate pairs (e.g. emojis)
  if (text.length > maxLength) {
    // Array.from splits correctly on code points / surrogate pairs
    const chars = Array.from(text);
    if (chars.length > maxLength) {
      text = chars.slice(0, maxLength).join("").trim();
      text = text.replace(/^[\.\s]+|[\.\s]+$/g, "");
    }
  }

  return text || "untitled";
}

/**
 * Sanitizes a filename extracted from a URL to ensure it is safe to save to disk.
 */
export function sanitizeFilename(filename: string): string {
  if (!filename) return "file";
  let cleaned = decodeURIComponent(filename);
  cleaned = cleaned.replace(/[\x00-\x1f\x7f-\x9f]/g, "");
  cleaned = cleaned.replace(/[/\\:*?"<>|]/g, "_");
  cleaned = cleaned.replace(/^\.+|\.+$/g, "");
  return cleaned || "file";
}
