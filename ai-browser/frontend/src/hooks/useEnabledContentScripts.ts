import { useMemo } from "react";
import type { Extension } from "../extensions";

export function useEnabledContentScripts(extensions: Extension[], url: string) {
  return useMemo(() => {
    if (!url || url === "about:blank") return [];
    const scripts: { id: string; matches: string[]; js: string[]; css: string[] }[] = [];
    for (const ext of extensions) {
      if (!ext.enabled) continue;
      for (const script of ext.contentScripts) {
        if (script.matches.some((m) => m === "<all_urls>" || url.startsWith(m))) {
          scripts.push(script);
        }
      }
    }
    return scripts;
  }, [extensions, url]);
}
