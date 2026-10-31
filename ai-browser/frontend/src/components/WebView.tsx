import { useEffect, useRef, useState } from "react";
import { useExtensionInjector } from "../hooks/useExtensionInjector";
import { useEnabledContentScripts } from "../hooks/useEnabledContentScripts";
import { ContextMenu, ContextMenuItem } from "./ContextMenu";
import { NewTabPage } from "./NewTabPage";
import type { Extension } from "../extensions";
import type { Bookmark } from "../shared-types";

export function WebView({
  url,
  extensions,
  bookmarks,
  onBack,
  onForward,
  onRefresh,
  onOpenExternal,
  onSavePage,
}: {
  url: string;
  extensions: Extension[];
  bookmarks?: Bookmark[];
  onBack?: () => void;
  onForward?: () => void;
  onRefresh?: () => void;
  onOpenExternal?: () => void;
  onSavePage?: () => void;
}) {
  const iframeRef = useRef<HTMLIFrameElement>(null);
  const [loading, setLoading] = useState(false);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [menu, setMenu] = useState<{ x: number; y: number } | null>(null);
  const scripts = useEnabledContentScripts(extensions, url);

  useExtensionInjector(iframeRef, scripts, url);

  useEffect(() => {
    if (!url || url === "about:blank") {
      setLoading(false);
      setLoadError(null);
      return;
    }

    setLoading(true);
    setLoadError(null);

    const timer = setTimeout(() => {
      setLoading(false);
    }, 4000);

    const handleLoad = () => {
      setLoading(false);
      setLoadError(null);
    };

    const handleError = () => {
      setLoading(false);
      setLoadError("This page could not be loaded in the embedded view.");
    };

    const iframe = iframeRef.current;
    if (iframe) {
      iframe.src = url;
      iframe.addEventListener("load", handleLoad);
      iframe.addEventListener("error", handleError);
    }

    return () => {
      clearTimeout(timer);
      if (iframe) {
        iframe.removeEventListener("load", handleLoad);
        iframe.removeEventListener("error", handleError);
      }
    };
  }, [url]);

  const openNative = async () => {
    try {
      const { invoke } = await import("@tauri-apps/api/core");
      await invoke("open_webview", { url });
    } catch {
      window.open(url, "_blank");
    }
  };

  const handleContextMenu = (e: React.MouseEvent) => {
    e.preventDefault();
    setMenu({ x: e.clientX, y: e.clientY });
  };

  if (url === "about:blank") {
    return (
      <div style={{ padding: 24, opacity: 0.7 }} onContextMenu={handleContextMenu}>
        <NewTabPage bookmarks={bookmarks || []} onNavigate={onOpenExternal || (() => {})} />
      </div>
    );
  }

  return (
    <div style={{ position: "relative", width: "100%", height: "100%" }} onContextMenu={handleContextMenu}>
      {loading && (
        <div
          style={{
            position: "absolute",
            inset: 0,
            display: "flex",
            alignItems: "center",
            justifyContent: "center",
            background: "var(--card)",
            color: "var(--muted-foreground)",
            fontSize: 13,
            zIndex: 2,
          }}
        >
          Loading {url}...
        </div>
      )}
      {loadError && (
        <div
          style={{
            position: "absolute",
            inset: 0,
            display: "flex",
            alignItems: "center",
            justifyContent: "center",
            background: "var(--card)",
            color: "#ffb3b3",
            fontSize: 13,
            padding: 24,
            textAlign: "center",
            zIndex: 2,
          }}
        >
          <div>
            <div style={{ marginBottom: 8, fontWeight: 600 }}>Webview load failed</div>
            <div>{loadError}</div>
            <div style={{ marginTop: 8, display: "flex", gap: 12, justifyContent: "center" }}>
              <button
                onClick={openNative}
                style={{
                  padding: "6px 12px",
                  background: "var(--accent)",
                  border: "none",
                  color: "#fff",
                  cursor: "pointer",
                  borderRadius: 4,
                }}
              >
                Open in native webview
              </button>
              <a
                href={url}
                target="_blank"
                rel="noreferrer"
                style={{ color: "var(--accent)" }}
              >
                Open in external browser
              </a>
            </div>
          </div>
        </div>
      )}
      <iframe
        ref={iframeRef}
        title="webview"
        sandbox="allow-scripts allow-same-origin allow-forms allow-popups"
        style={{ width: "100%", height: "100%", border: "none" }}
      />
      {menu && (
        <ContextMenu x={menu.x} y={menu.y} onClose={() => setMenu(null)}>
          <ContextMenuItem onClick={() => { onBack?.(); setMenu(null); }}>Back</ContextMenuItem>
          <ContextMenuItem onClick={() => { onForward?.(); setMenu(null); }}>Forward</ContextMenuItem>
          <ContextMenuItem onClick={() => { onRefresh?.(); setMenu(null); }}>Reload</ContextMenuItem>
          <ContextMenuItem onClick={() => { onOpenExternal?.(); setMenu(null); }}>Open in external browser</ContextMenuItem>
          <ContextMenuItem onClick={() => { onSavePage?.(); setMenu(null); }}>Save page as...</ContextMenuItem>
        </ContextMenu>
      )}
    </div>
  );
}


