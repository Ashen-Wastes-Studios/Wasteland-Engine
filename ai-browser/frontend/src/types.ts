export type Theme = "dark" | "light";

export type SearchEngine = "searxng" | "google" | "duckduckgo" | "bing";

export type Settings = {
  searxngUrl: string;
  llmUrl: string;
  llmModel: string;
  downloadsPath: string;
  enableDownloads: boolean;
  theme: Theme;
  searchEngine: SearchEngine;
};

export const DEFAULT_SETTINGS: Settings = {
  searxngUrl: "http://localhost:8888",
  llmUrl: "http://localhost:11434",
  llmModel: "qwen2.5:3b",
  downloadsPath: "",
  enableDownloads: true,
  theme: "dark",
  searchEngine: "searxng",
};
