export type Settings = {
  searxngUrl: string;
  llmUrl: string;
  llmModel: string;
  downloadsPath: string;
  enableDownloads: boolean;
};

export const DEFAULT_SETTINGS: Settings = {
  searxngUrl: "http://localhost:8888",
  llmUrl: "http://localhost:11434",
  llmModel: "qwen2.5:3b",
  downloadsPath: "",
  enableDownloads: true,
};
