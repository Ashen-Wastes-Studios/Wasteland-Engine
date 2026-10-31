export type DownloadItem = {
  id: string;
  url: string;
  filename: string;
  size: number | null;
  progress: number;
  status: "pending" | "downloading" | "completed" | "failed";
  error?: string;
};
