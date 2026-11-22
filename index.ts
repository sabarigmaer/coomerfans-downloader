import * as readline from "node:readline/promises";
import { stdin as input, stdout as output } from "node:process";
import { CoomerDownloader } from "./src/downloader";

function printBanner(): void {
  console.log(`
┌──────────────────────────────────────────────────┐
│             CoomerFans Downloader                │
│    Fast, resumable scraper & media downloader    │
└──────────────────────────────────────────────────┘
`);
}

function parseCliArgs() {
  const args = process.argv.slice(2);
  let url: string | undefined;
  let mode: "list" | "download" | undefined;
  let outputDir: string = "./downloads";
  let delayMs: number = 500;

  for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    if (arg === "--list" || arg === "-l") {
      mode = "list";
    } else if (arg === "--download" || arg === "-d") {
      mode = "download";
    } else if (arg === "--mode" || arg === "-m") {
      const next = args[++i]?.toLowerCase();
      if (next === "list" || next === "download") {
        mode = next;
      }
    } else if (arg === "--output" || arg === "-o") {
      outputDir = args[++i] || outputDir;
    } else if (arg === "--delay") {
      delayMs = parseInt(args[++i] || "500", 10);
    } else if (arg === "--help" || arg === "-h") {
      console.log(`
Usage:
  bun run index.ts [url] [options]

Options:
  -l, --list             Download file list (collects post titles page-wise, no files)
  -d, --download         Download content (downloads media inside .post-body, resumable)
  -o, --output <dir>     Output directory (default: ./downloads)
  --delay <ms>           Delay between requests in ms (default: 500)
  -h, --help             Show this help message

Examples:
  # Interactive mode:
  bun run index.ts

  # Download file list:
  bun run index.ts https://coomerfans.com/u/onlyfans/313401/rbbca --list

  # Download content (resumable):
  bun run index.ts https://coomerfans.com/u/onlyfans/313401/rbbca --download
`);
      process.exit(0);
    } else if (!arg.startsWith("-") && !url) {
      url = arg;
    }
  }

  return { url, mode, outputDir, delayMs };
}

async function main() {
  const parsed = parseCliArgs();
  let targetUrl = parsed.url;
  let selectedMode = parsed.mode;

  const rl = readline.createInterface({ input, output });

  try {
    printBanner();

    // 1. Get URL if not provided via command line
    while (!targetUrl) {
      const answer = await rl.question(
        "Enter CoomerFans URL (e.g. https://coomerfans.com/u/onlyfans/313401/rbbca):\n> "
      );
      const trimmed = answer.trim();
      if (!trimmed) {
        console.log("Please enter a valid URL.");
        continue;
      }
      try {
        new URL(trimmed);
        targetUrl = trimmed;
      } catch {
        console.log("Invalid URL format. Please try again.");
      }
    }

    // 2. Select option if not provided via flag
    if (!selectedMode) {
      console.log(`\nTarget URL: ${targetUrl}\n`);
      console.log("Please choose an option:");
      console.log("  [1] Download file list");
      console.log("      Crawls page by page, gets the title of each post and stores them page-wise (no files).");
      console.log("  [2] Download content");
      console.log("      Crawls page by page, goes inside each post and downloads files inside .post-body.");
      console.log("      Maintains progress so it can be resumed if interrupted.");
      console.log("  [q] Quit\n");

      while (!selectedMode) {
        const choice = (await rl.question("Enter choice (1 or 2): ")).trim().toLowerCase();
        if (choice === "1" || choice === "list") {
          selectedMode = "list";
        } else if (choice === "2" || choice === "download" || choice === "content") {
          selectedMode = "download";
        } else if (choice === "q" || choice === "quit" || choice === "exit") {
          console.log("Exiting.");
          process.exit(0);
        } else {
          console.log("Invalid selection. Please enter 1, 2, or q.");
        }
      }
    }

    rl.close();

    const downloader = new CoomerDownloader({
      outputDir: parsed.outputDir,
      delayMs: parsed.delayMs,
    });

    if (selectedMode === "list") {
      await downloader.downloadFileList(targetUrl);
    } else {
      await downloader.downloadContent(targetUrl);
    }
    process.exit(0);
  } catch (err: any) {
    console.error(`\n[Fatal Error]: ${err.message || err}`);
    process.exit(1);
  } finally {
    rl.close();
  }
}

main();
