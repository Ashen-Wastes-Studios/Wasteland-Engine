import { useState, useCallback } from "react";
import type { Extension, ContentScript } from "../extensions";

const EXTENSIONS_KEY = "ai-browser-extensions";

function loadExtensions(): Extension[] {
  try {
    const raw = localStorage.getItem(EXTENSIONS_KEY);
    return raw ? (JSON.parse(raw) as Extension[]) : [];
  } catch {
    return [];
  }
}

export function useExtensions() {
  const [extensions, setExtensions] = useState<Extension[]>(loadExtensions);

  const persist = useCallback((items: Extension[]) => {
    localStorage.setItem(EXTENSIONS_KEY, JSON.stringify(items));
  }, []);

  const toggleExtension = useCallback(
    (id: string) => {
      setExtensions((prev) => {
        const next = prev.map((ext) => (ext.id === id ? { ...ext, enabled: !ext.enabled } : ext));
        persist(next);
        return next;
      });
    },
    [persist],
  );

  const addExtension = useCallback(
    (extension: Extension) => {
      setExtensions((prev) => {
        const next = [...prev, extension];
        persist(next);
        return next;
      });
    },
    [persist],
  );

  const removeExtension = useCallback(
    (id: string) => {
      setExtensions((prev) => {
        const next = prev.filter((ext) => ext.id !== id);
        persist(next);
        return next;
      });
    },
    [persist],
  );

  const addContentScript = useCallback(
    (extensionId: string, script: ContentScript) => {
      setExtensions((prev) => {
        const next = prev.map((ext) =>
          ext.id === extensionId
            ? { ...ext, contentScripts: [...ext.contentScripts, script] }
            : ext,
        );
        persist(next);
        return next;
      });
    },
    [persist],
  );

  return { extensions, toggleExtension, addExtension, removeExtension, addContentScript };
}
