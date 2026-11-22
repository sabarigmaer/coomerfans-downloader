export interface PostFile {
  url: string;
  filename: string;
  size?: number;
  status: "pending" | "downloading" | "completed" | "failed";
  localPath?: string;
  error?: string;
}

export interface PostItem {
  id: string;
  title: string;
  safeTitle: string;
  url: string;
  date?: string;
  description?: string;
  files?: PostFile[];
  status: "pending" | "in_progress" | "completed" | "failed";
  error?: string;
}

export interface PageData {
  pageNumber: number;
  pageUrl: string;
  postCount: number;
  status: "pending" | "in_progress" | "completed";
  posts: PostItem[];
}

export interface CreatorInfo {
  service: string;
  userId: string;
  username: string;
  normalizedUrl: string;
}

export interface ProgressState {
  creator: string;
  service: string;
  sourceUrl: string;
  outputDir: string;
  currentPageNumber: number;
  currentPageUrl: string;
  totalPagesScanned: number;
  totalPostsCount: number;
  totalFilesDownloaded: number;
  totalBytesDownloaded: number;
  isFinished: boolean;
  lastUpdated: string;
  pages: Record<string, PageData>;
}

export interface FileListOutput {
  creator: string;
  service: string;
  sourceUrl: string;
  totalPages: number;
  totalPosts: number;
  generatedAt: string;
  pages: {
    pageNumber: number;
    pageUrl: string;
    postCount: number;
    posts: {
      id: string;
      title: string;
      safeTitle: string;
      url: string;
      date?: string;
      status?: "pending" | "in_progress" | "completed" | "failed";
      files?: PostFile[];
    }[];
  }[];
}
