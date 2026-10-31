import { useState, useCallback } from "react";
import type { DownloadItem } from "../downloads";

const DOWNLOADS_KEY = "ai-browser-downloads";

function loadDownloads(): DownloadItem[] {
  try {
    const raw = localStorage.getItem(DOWNLOADS_KEY);
    return raw ? (JSON.parse(raw) as DownloadItem[]) : [];
  } catch {
    return [];
  }
}

export function useDownloads() {
  const [downloads, setDownloads] = useState<DownloadItem[]>(loadDownloads);

  const persist = useCallback((items: DownloadItem[]) => {
    localStorage.setItem(DOWNLOADS_KEY, JSON.stringify(items.slice(0, 100)));
  }, []);

  const addDownload = useCallback(
    (url: string, filename?: string) => {
      const item: DownloadItem = {
        id: crypto.randomUUID(),
        url,
        filename: filename || url.split("/").pop() || "download",
        size: null,
        progress: 0,
        status: "pending",
      };
      setDownloads((prev) => {
        const next = [item, ...prev];
        persist(next);
        return next;
      });
      return item;
    },
    [persist],
  );

  const updateDownload = useCallback(
    (id: string, patch: Partial<DownloadItem>) => {
      setDownloads((prev) => {
        const next = prev.map((item) => (item.id === id ? { ...item, ...patch } : item));
        persist(next);
        return next;
      });
    },
    [persist],
  );

  const removeDownload = useCallback(
    (id: string) => {
      setDownloads((prev) => {
        const next = prev.filter((item) => item.id !== id);
        persist(next);
        return next;
      });
    },
    [persist],
  );

  const clearCompleted = useCallback(() => {
    setDownloads((prev) => {
      const next = prev.filter((item) => item.status !== "completed");
      persist(next);
      return next;
    });
  }, [persist]);

  return { downloads, addDownload, updateDownload, removeDownload, clearCompleted };
}
