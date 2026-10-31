import { useEffect } from "react";
import type { ExtensionScript } from "../extension-scripts";

export function useExtensionInjector(
  iframeRef: React.RefObject<HTMLIFrameElement | null>,
  scripts: ExtensionScript[],
  url: string,
) {
  useEffect(() => {
    const iframe = iframeRef.current;
    if (!iframe || !url || url === "about:blank") return;

    const tryInject = () => {
      try {
        const doc = iframe.contentDocument;
        if (!doc) return false;
        for (const script of scripts) {
          if (!script.matches.some((m) => url.startsWith(m))) continue;
          for (const css of script.css) {
            const style = doc.createElement("style");
            style.textContent = css;
            doc.head.appendChild(style);
          }
          for (const js of script.js) {
            const tag = doc.createElement("script");
            tag.textContent = js;
            doc.head.appendChild(tag);
          }
        }
        return true;
      } catch {
        return false;
      }
    };

    const injected = tryInject();
    if (injected) return;

    const handler = () => {
      const injected = tryInject();
      if (injected) {
        iframe.removeEventListener("load", handler);
      }
    };
    iframe.addEventListener("load", handler);
    return () => iframe.removeEventListener("load", handler);
  }, [iframeRef, scripts, url]);
}
